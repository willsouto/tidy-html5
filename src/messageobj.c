/* messageobj.c
 * Provides an external, extensible API for message reporting.
 *
 * (c) 2017 HTACG
 * See tidy.h for the copyright notice.
 */

#include "messageobj.h"
#include "message.h"
#include "tidy-int.h"
#include "limits.h"
#include "tmbstr.h"
#if !defined(NDEBUG) && defined(_MSC_VER)
#include "sprtf.h"
#endif


/*********************************************************************
 * BuildArgArray Support - declarations and forward declarations
 *********************************************************************/


/** A record of a single argument and its type. An array these
**  represents the arguments supplied to a format string, ordered
**  in the same position as they occur in the format string. Because
**  Windows doesn't support modern positional arguments, Tidy doesn't
**  either.
*/

#define FORMAT_LENGTH 21

struct printfArg {
    TidyFormatParameterType type;  /* type of the argument    */
    int formatStart;               /* where the format starts */
    int formatLength;              /* length of the format    */
    char format[FORMAT_LENGTH];    /* buffer for the format   */
    union {                        /* the argument            */
        int i;
        uint ui;
        int32_t i32;
        uint32_t ui32;
        int64_t ll;
        uint64_t ull;
        double d;
        const char *s;
        size_t *ip;
#ifdef WIN32
        const WCHAR *ws;
#endif
    } u;
};



/** Returns a pointer to an allocated array of `printfArg` given a format
 ** string and a va_list, or NULL if not successful or no parameters were
 ** given. Parameter `rv` will return with the count of zero or more
 ** parameters if successful, else -1.
 **
 */
static struct printfArg *BuildArgArray( TidyDocImpl *doc, ctmbstr fmt, va_list ap, int *rv );


/*********************************************************************
 * Tidy Message Object Support
 *********************************************************************/


/** Create an internal representation of a Tidy message with all of
 ** the information that that we know about the message.
 **
 ** The function signature doesn't have to stay static and is a good
 ** place to add instantiation if expanding the API.
 **
 ** We currently know the doc, node, code, line, column, level, and
 ** args, will pre-calculate all of the other members upon creation.
 ** This ensures that we can use members directly, immediately,
 ** without having to use accessors internally.
 **
 ** If any message callback filters are setup by API clients, they
 ** will be called here.
 **
 ** This version serves as the designated initializer and as such
 ** requires every known parameter.
 */
static TidyMessageImpl *tidyMessageCreateInitV( TidyDocImpl *doc,
                                                Node *node,
                                                uint code,
                                                int line,
                                                int column,
                                                TidyReportLevel level,
                                                va_list args )
{
    TidyMessageImpl *result = TidyDocAlloc(doc, sizeof(TidyMessageImpl));
    TidyDoc tdoc = tidyImplToDoc(doc);
    va_list args_copy;
    enum { sizeMessageBuf=2048 };
    ctmbstr pattern;

    /* Things we know... */

    result->tidyDoc = doc;
    result->tidyNode = node;
    result->code = code;
    result->line = line;
    result->column = column;
    result->level = level;

    /* Things we create... */

    va_copy(args_copy, args);
    result->arguments = BuildArgArray(doc, tidyDefaultString(code), args_copy, &result->argcount);
    va_end(args_copy);

    result->messageKey = TY_(tidyErrorCodeAsKey)(code);

    result->messageFormatDefault = tidyDefaultString(code);
    result->messageFormat = tidyLocalizedString(code);

    result->messageDefault = TidyDocAlloc(doc, sizeMessageBuf);
    va_copy(args_copy, args);
    TY_(tmbvsnprintf)(result->messageDefault, sizeMessageBuf, result->messageFormatDefault, args_copy);
    va_end(args_copy);

    result->message = TidyDocAlloc(doc, sizeMessageBuf);
    va_copy(args_copy, args);
    TY_(tmbvsnprintf)(result->message, sizeMessageBuf, result->messageFormat, args_copy);
    va_end(args_copy);

    result->messagePosDefault = TidyDocAlloc(doc, sizeMessageBuf);
    result->messagePos = TidyDocAlloc(doc, sizeMessageBuf);

    if ( cfgBool(doc, TidyEmacs) && cfgStr(doc, TidyEmacsFile) )
    {
        /* Change formatting to be parsable by GNU Emacs */
        TY_(tmbsnprintf)(result->messagePosDefault, sizeMessageBuf, "%s:%d:%d: ", cfgStr(doc, TidyEmacsFile), line, column);
        TY_(tmbsnprintf)(result->messagePos, sizeMessageBuf, "%s:%d:%d: ", cfgStr(doc, TidyEmacsFile), line, column);
    }
    else
    {
        /* traditional format */
        TY_(tmbsnprintf)(result->messagePosDefault, sizeMessageBuf, tidyDefaultString(LINE_COLUMN_STRING), line, column);
        TY_(tmbsnprintf)(result->messagePos, sizeMessageBuf, tidyLocalizedString(LINE_COLUMN_STRING), line, column);
    }

    result->messagePrefixDefault = tidyDefaultString(level);

    result->messagePrefix = tidyLocalizedString(level);

    if ( line > 0 && column > 0 )
        pattern = "%s%s%s";      /* pattern in there's location information */
    else
        pattern = "%.0s%s%s";    /* otherwise if there isn't */

    if ( level > TidyFatal )
        pattern = "%.0s%.0s%s";  /* dialog doesn't have pos or prefix */

    result->messageOutputDefault = TidyDocAlloc(doc, sizeMessageBuf);
    TY_(tmbsnprintf)(result->messageOutputDefault, sizeMessageBuf, pattern,
                     result->messagePosDefault, result->messagePrefixDefault,
                     result->messageDefault);

    result->messageOutput = TidyDocAlloc(doc, sizeMessageBuf);
    TY_(tmbsnprintf)(result->messageOutput, sizeMessageBuf, pattern,
                     result->messagePos, result->messagePrefix,
                     result->message);


    result->allowMessage = yes;

    /* reportFilter is a simple error filter that provides minimal information
       to callback functions, and includes the message buffer in LibTidy's
       configured localization. As it's a "legacy" API, it does not receive
       TidyDialogue messages.*/
    if ( (result->level <= TidyFatal) && doc->reportFilter )
    {
        result->allowMessage = result->allowMessage & doc->reportFilter( tdoc, result->level, result->line, result->column, result->messageOutput );
    }

    /* reportCallback is intended to allow LibTidy users to localize messages
       via their own means by providing a key and the parameters to fill it. 
       As it's a "legacy" API, it does not receive TidyDialogue messages. */
    if ( (result->level <= TidyFatal) && doc->reportCallback )
    {
        TidyDoc tdoc = tidyImplToDoc( doc );
        va_copy(args_copy, args);
        result->allowMessage = result->allowMessage & doc->reportCallback( tdoc, result->level, result->line, result->column, result->messageKey, args_copy );
        va_end(args_copy);
    }

    /* messageCallback is the newest interface to interrogate Tidy's
       emitted messages. */
    if ( doc->messageCallback )
    {
        result->allowMessage = result->allowMessage & doc->messageCallback( tidyImplToMessage(result) );
    }

    return result;
}


TidyMessageImpl *TY_(tidyMessageCreate)( TidyDocImpl *doc,
                                         uint code,
                                         TidyReportLevel level,
                                         ... )
{
    TidyMessageImpl *result;
    va_list args;
    va_start(args, level);
    result = tidyMessageCreateInitV(doc, NULL, code, 0, 0, level, args);
    va_end(args);
    
    return result;
}


TidyMessageImpl *TY_(tidyMessageCreateWithNode)( TidyDocImpl *doc,
                                                 Node *node,
                                                 uint code,
                                                 TidyReportLevel level,
                                                 ... )
{
    TidyMessageImpl *result;
    va_list args_copy;
    int line = ( node ? node->line :
                ( doc->lexer ? doc->lexer->lines : 0 ) );
    int col  = ( node ? node->column :
                ( doc->lexer ? doc->lexer->columns : 0 ) );
    
    va_start(args_copy, level);
    result = tidyMessageCreateInitV(doc, node, code, line, col, level, args_copy);
    va_end(args_copy);
    
    return result;
}


TidyMessageImpl *TY_(tidyMessageCreateWithLexer)( TidyDocImpl *doc,
                                                  uint code,
                                                  TidyReportLevel level,
                                                  ... )
{
    TidyMessageImpl *result;
    va_list args_copy;
    int line = ( doc->lexer ? doc->lexer->lines : 0 );
    int col  = ( doc->lexer ? doc->lexer->columns : 0 );
    
    va_start(args_copy, level);
    result = tidyMessageCreateInitV(doc, NULL, code, line, col, level, args_copy);
    va_end(args_copy);
    
    return result;
}


void TY_(tidyMessageRelease)( TidyMessageImpl *message )
{
    if ( !message )
        return;
    TidyDocFree( tidyDocToImpl(message->tidyDoc), message->arguments );
    TidyDocFree( tidyDocToImpl(message->tidyDoc), message->messageDefault );
    TidyDocFree( tidyDocToImpl(message->tidyDoc), message->message );
    TidyDocFree( tidyDocToImpl(message->tidyDoc), message->messagePosDefault );
    TidyDocFree( tidyDocToImpl(message->tidyDoc), message->messagePos );
    TidyDocFree( tidyDocToImpl(message->tidyDoc), message->messageOutputDefault );
    TidyDocFree( tidyDocToImpl(message->tidyDoc), message->messageOutput );
}


/*********************************************************************
 * Modern Message Callback Functions
 *********************************************************************/


ctmbstr TY_(getMessageKey)( TidyMessageImpl message )
{
    return message.messageKey;
}

int TY_(getMessageLine)( TidyMessageImpl message )
{
    return message.line;
}

int TY_(getMessageColumn)( TidyMessageImpl message )
{
    return message.column;
}

TidyReportLevel TY_(getMessageLevel)( TidyMessageImpl message )
{
    return message.level;
}

ctmbstr TY_(getMessageFormatDefault)( TidyMessageImpl message )
{
    return message.messageFormatDefault;
}

ctmbstr TY_(getMessageFormat)( TidyMessageImpl message )
{
    return message.messageFormat;
}

ctmbstr TY_(getMessageDefault)( TidyMessageImpl message )
{
    return message.messageDefault;
}

ctmbstr TY_(getMessage)( TidyMessageImpl message )
{
    return message.message;
}

ctmbstr TY_(getMessagePosDefault)( TidyMessageImpl message )
{
    return message.messagePosDefault;
}

ctmbstr TY_(getMessagePos)( TidyMessageImpl message )
{
    return message.messagePos;
}

ctmbstr TY_(getMessagePrefixDefault)( TidyMessageImpl message )
{
    return message.messagePrefixDefault;
}

ctmbstr TY_(getMessagePrefix)( TidyMessageImpl message )
{
    return message.messagePrefix;
}


ctmbstr TY_(getMessageOutputDefault)( TidyMessageImpl message )
{
    return message.messageOutputDefault;
}

ctmbstr TY_(getMessageOutput)( TidyMessageImpl message )
{
    return message.messageOutput;
}


/*********************************************************************
 * Message Argument Interrogation
 *********************************************************************/


TidyIterator TY_(getMessageArguments)( TidyMessageImpl message )
{
    if (message.argcount > 0)
        return (TidyIterator) (size_t)1;
    else
        return (TidyIterator) (size_t)0;
}

TidyMessageArgument TY_(getNextMessageArgument)( TidyMessageImpl message, TidyIterator* iter )
{
    size_t item = 0;
    size_t itemIndex;
    assert( iter != NULL );
    
    itemIndex = (size_t)*iter;
    
    if ( itemIndex >= 1 && itemIndex <= message.argcount )
    {
        item = itemIndex - 1;
        itemIndex++;
    }
    
    /* Just as TidyIterator is really just a dumb, one-based index, the
       TidyMessageArgument is really just a dumb, zero-based index; however
       this type of iterator and opaque interrogation is simply how Tidy
       does things. */
    *iter = (TidyIterator)( itemIndex <= message.argcount ? itemIndex : (size_t)0 );
    return (TidyMessageArgument)item;
}


TidyFormatParameterType TY_(getArgType)( TidyMessageImpl message, TidyMessageArgument* arg )
{
    int argNum = (int)*arg;
    assert( argNum <= message.argcount );
    
    return message.arguments[argNum].type;
}


ctmbstr TY_(getArgFormat)( TidyMessageImpl message, TidyMessageArgument* arg )
{
    int argNum = (int)*arg;
    assert( argNum <= message.argcount );
    
    return message.arguments[argNum].format;
}


ctmbstr TY_(getArgValueString)( TidyMessageImpl message, TidyMessageArgument* arg )
{
    int argNum = (int)*arg;
    assert( argNum <= message.argcount );
    assert( message.arguments[argNum].type == tidyFormatType_STRING);
    
    return message.arguments[argNum].u.s;
}


uint TY_(getArgValueUInt)( TidyMessageImpl message, TidyMessageArgument* arg )
{
    int argNum = (int)*arg;
    uint result = 0;
    Bool typeIsValid = yes;
    assert( argNum <= message.argcount );

    /* Tidy only uses %u currently, but we'll return a larger uint if that's
       what we have and the current uint supports its size. */
    switch (message.arguments[argNum].type)
    {
        case tidyFormatType_UINTN:
            result = (uint)message.arguments[argNum].u.ui;
            break;
            
        case tidyFormatType_UINT16:
            if ( sizeof(uint) <= sizeof(uint16_t))
                result = (uint)message.arguments[argNum].u.i;
            else
                typeIsValid = no;
            break;
            
        case tidyFormatType_UINT32:
            if ( sizeof(uint) <= sizeof(uint32_t))
                result = (uint)message.arguments[argNum].u.ui32;
            else
                typeIsValid = no;
            break;
            
        case tidyFormatType_UINT64:
            if ( sizeof(uint) <= sizeof(uint64_t))
                result = (uint)message.arguments[argNum].u.ull;
            else
                typeIsValid = no;
            break;
            
        default:
            typeIsValid = no;
            break;
    }
    
    assert(typeIsValid == yes);
    
    return result;
}


int TY_(getArgValueInt)( TidyMessageImpl message, TidyMessageArgument* arg )
{
    int argNum = (int)*arg;
    int result = 0;
    Bool typeIsValid = yes;
    assert( argNum <= message.argcount );
    
    /* Tidy only uses %d currently, but we'll return a larger int or uint
       if that's what we have and the current int supports its size. */
    switch (message.arguments[argNum].type)
    {
        case tidyFormatType_INTN:
            result = (int)message.arguments[argNum].u.i;
            break;
        
        case tidyFormatType_INT16:
            if ( sizeof(int) <= sizeof(int16_t))
                result = (int)message.arguments[argNum].u.i;
            else
                typeIsValid = no;
            break;
            
        case tidyFormatType_INT32:
            if ( sizeof(int) <= sizeof(int32_t))
                result = (int)message.arguments[argNum].u.i32;
            else
                typeIsValid = no;
            break;
            
        case tidyFormatType_INT64:
            if ( sizeof(int) <= sizeof(int64_t))
                result = (int)message.arguments[argNum].u.ll;
            else
                typeIsValid = no;
            break;
            
        /* Special testing for uints: if they're small enough to
           fit, then we'll allow them. */
            
        case tidyFormatType_UINTN:
            if ( message.arguments[argNum].u.ui <= INT_MAX )
                result = (int)message.arguments[argNum].u.ui;
            else
                typeIsValid = no;
            break;

        case tidyFormatType_UINT16:
            if ( sizeof(int) <= sizeof(int16_t) && message.arguments[argNum].u.i <= INT_MAX )
                result = (int)message.arguments[argNum].u.i;
            else
                typeIsValid = no;
            break;
            
        case tidyFormatType_UINT32:
            if ( sizeof(int) <= sizeof(int32_t) && (message.arguments[argNum].u.ui32 <= INT_MAX) )
                result = (int)message.arguments[argNum].u.ui32;
            else
                typeIsValid = no;
            break;
            
        case tidyFormatType_UINT64:
            if ( sizeof(int) <= sizeof(int64_t) && message.arguments[argNum].u.ull <= INT_MAX )
                result = (int)message.arguments[argNum].u.ull;
            else
                typeIsValid = no;
            break;
            
        default:
            typeIsValid = no;
            break;
    }
    
    assert(typeIsValid == yes);
    
    return result;
}


double TY_(getArgValueDouble)( TidyMessageImpl message, TidyMessageArgument* arg )
{
    int argNum = (int)*arg;
    assert( argNum <= message.argcount );
    assert( message.arguments[argNum].type == tidyFormatType_DOUBLE);
    
    return message.arguments[argNum].u.d;
}



/*********************************************************************
 * BuildArgArray support
 * Adapted loosely from Mozilla `prprf.c`, Mozilla Public License:
 *   - https://www.mozilla.org/en-US/MPL/2.0/
 *********************************************************************/


/** Returns a pointer to an allocated array of `printfArg` given a format
 ** string and a va_list, or NULL if not successful or no parameters were
 ** given. Parameter `rv` will return with the count of zero or more
 ** parameters if successful, else -1.
 **
 ** We'll also be sure to use the document's allocator if specified, thus
 ** the requirement to pass in a TidyDocImpl.
 */
static struct printfArg* BuildArgArray( TidyDocImpl *doc, ctmbstr fmt, va_list ap, int* rv )
{
    int number = 0; /* the quantity of valid arguments found; returned as rv. */
    int cn = -1;    /* keeps track of which parameter index is current. */
    int i = 0;      /* typical index. */
    int pos = -1;   /* starting position of current argument. */
    const char* p;  /* current position in format string. */
    char c;         /* current character. */
    struct printfArg* nas;
    
    /* first pass: determine number of valid % to allocate space. */
    
    p = fmt;
    *rv = 0;
    
    while( ( c = *p++ ) != 0 )
    {
        if( c != '%' )
            continue;
        
        if( ( c = *p++ ) == '%' )	/* skip %% case */
            continue;
        else
            number++;
    }
        

    if( number == 0 )
        return NULL;

    
    nas = (struct printfArg*)TidyDocAlloc( doc, number * sizeof( struct printfArg ) );
    if( !nas )
    {
        *rv = -1;
        return NULL;
    }


    for( i = 0; i < number; i++ )
    {
        nas[i].type = tidyFormatType_UNKNOWN;
    }
    
    
    /* second pass: set nas[].type and location. */
    
    p = fmt;
    while( ( c = *p++ ) != 0 )
    {
        if( c != '%' )
            continue;
        
        if( ( c = *p++ ) == '%' )
            continue; /* skip %% case */

        pos = p - fmt - 2; /* p already incremented twice */

        /* width -- width via parameter */
        if (c == '*')
        {
            /* not supported feature */
            *rv = -1;
            break;
        }
        
        /* width field -- skip */
        while ((c >= '0') && (c <= '9'))
        {
            c = *p++;
        }
        
        /* precision */
        if (c == '.')
        {
            c = *p++;
            if (c == '*') {
                /* not supported feature */
                *rv = -1;
                break;
            }
            
            while ((c >= '0') && (c <= '9'))
            {
                c = *p++;
            }
        }
        
        
        cn++;
        
        /* size */
        nas[cn].type = tidyFormatType_INTN;
        if (c == 'h')
        {
            nas[cn].type = tidyFormatType_INT16;
            c = *p++;
        } else if (c == 'L')
        {
            nas[cn].type = tidyFormatType_INT64;
            c = *p++;
        } else if (c == 'l')
        {
            nas[cn].type = tidyFormatType_INT32;
            c = *p++;
            if (c == 'l') {
                nas[cn].type = tidyFormatType_INT64;
                c = *p++;
            }
        } else if (c == 'z')
        {
            if (sizeof(size_t) == sizeof(int32_t))
            {
                nas[ cn ].type = tidyFormatType_INT32;
            } else if (sizeof(size_t) == sizeof(int64_t))
            {
                nas[ cn ].type = tidyFormatType_INT64;
            } else
            {
                nas[ cn ].type = tidyFormatType_UNKNOWN;
            }
            c = *p++;
        }
        
        /* format */
        switch (c)
        {
            case 'd':
            case 'c':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
            case 'X':
                break;
                
            case 'e':
            case 'f':
            case 'g':
                nas[ cn ].type = tidyFormatType_DOUBLE;
                break;
                
            case 'p':
                if (sizeof(void *) == sizeof(int32_t))
                {
                    nas[ cn ].type = tidyFormatType_UINT32;
                } else if (sizeof(void *) == sizeof(int64_t))
                {
                    nas[ cn ].type = tidyFormatType_UINT64;
                } else if (sizeof(void *) == sizeof(int))
                {
                    nas[ cn ].type = tidyFormatType_UINTN;
                } else
                {
                    nas[ cn ].type = tidyFormatType_UNKNOWN;
                }
                break;
                
            case 'S':
#ifdef WIN32
                nas[ cn ].type = TYPE_WSTRING;
                break;
#endif
            case 'C':
            case 'E':
            case 'G':
                nas[ cn ].type = tidyFormatType_UNKNOWN;
                break;
                
            case 's':
                nas[ cn ].type = tidyFormatType_STRING;
                break;
                
            case 'n':
                nas[ cn ].type = tidyFormatType_INTSTR;
                break;
                
            default:
                nas[ cn ].type = tidyFormatType_UNKNOWN;
                break;
        }
        
        /* position and format */
        nas[cn].formatStart = pos;
        nas[cn].formatLength = (p - fmt) - pos;
        
        /* the format string exceeds the buffer length */
        if ( nas[cn].formatLength >= FORMAT_LENGTH )
        {
            *rv = -1;
            break;
        }
        else
        {
            strncpy(nas[cn].format, fmt + nas[cn].formatStart, nas[cn].formatLength);
        }
        

        /* Something's not right. */
        if( nas[ cn ].type == tidyFormatType_UNKNOWN )
        {
            *rv = -1;
            break;
        }
    }
    
    
    /* third pass: fill the nas[cn].ap */
    
    if( *rv < 0 )
    {
        TidyDocFree( doc, nas );;
        return NULL;
    }
    
    cn = 0;
    while( cn < number )
    {
        if( nas[cn].type == tidyFormatType_UNKNOWN )
        {
            cn++;
            continue;
        }
        
        switch( nas[cn].type )
        {
            case tidyFormatType_INT16:
            case tidyFormatType_UINT16:
            case tidyFormatType_INTN:
                nas[cn].u.i = va_arg( ap, int );
                break;
                
            case tidyFormatType_UINTN:
                nas[cn].u.ui = va_arg( ap, unsigned int );
                break;
                
            case tidyFormatType_INT32:
                nas[cn].u.i32 = va_arg( ap, int32_t );
                break;
                
            case tidyFormatType_UINT32:
                nas[cn].u.ui32 = va_arg( ap, uint32_t );
                break;
                
            case tidyFormatType_INT64:
                nas[cn].u.ll = va_arg( ap, int64_t );
                break;
                
            case tidyFormatType_UINT64:
                nas[cn].u.ull = va_arg( ap, uint64_t );
                break;
                
            case tidyFormatType_STRING:
                nas[cn].u.s = va_arg( ap, char* );
                break;
                
#ifdef WIN32
            case tidyFormatType_WSTRING:
                nas[cn].u.ws = va_arg( ap, WCHAR* );
                break;
#endif
                
            case tidyFormatType_INTSTR:
                nas[cn].u.ip = va_arg( ap, size_t* );
                break;
                
            case tidyFormatType_DOUBLE:
                nas[cn].u.d = va_arg( ap, double );
                break;
                
            default:
                TidyDocFree( doc, nas );
                *rv = -1;
                return NULL;
        }
        
        cn++;
    }
    
    *rv = number;
    return nas;
}

