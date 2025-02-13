
// Fast JSON encoder/decoder implementation for Python
//

#include <Python.h>
#include <stddef.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

typedef struct JSONData {
    char *str; // the actual json string
    char *end; // pointer to the string end
    char *ptr; // pointer to the current parsing position
    int  all_unicode; // make all output strings unicode if true
    int source_unicode;
} JSONData;

static PyObject* encode_object(PyObject *object);
static PyObject* encode_unicode(PyObject *object);
static PyObject* encode_tuple(PyObject *object);
static PyObject* encode_list(PyObject *object);
static PyObject* encode_dict(PyObject *object);

static PyObject* decode_json(JSONData *jsondata);
static PyObject* decode_null(JSONData *jsondata);
static PyObject* decode_bool(JSONData *jsondata);
static PyObject* decode_string(JSONData *jsondata);
static PyObject* decode_inf(JSONData *jsondata);
static PyObject* decode_nan(JSONData *jsondata);
static PyObject* decode_number(JSONData *jsondata);
static PyObject* decode_array(JSONData *jsondata);
static PyObject* decode_object(JSONData *jsondata);

static PyObject *JSON_Error;
static PyObject *JSON_EncodeError;
static PyObject *JSON_DecodeError;


#define _string(x) #x
#define string(x) _string(x)

#if PY_VERSION_HEX < 0x02050000
typedef int Py_ssize_t;
#define PY_SSIZE_T_MAX INT_MAX
#define PY_SSIZE_T_MIN INT_MIN

#define SSIZE_T_F "%d"
#else
#define SSIZE_T_F "%zd"
#endif

#define True  1
#define False 0

#ifndef INFINITY
#define INFINITY HUGE_VAL
#endif

#ifndef NAN
#define NAN (HUGE_VAL - HUGE_VAL)
#endif

#ifndef Py_IS_NAN
#define Py_IS_NAN(X) ((X) != (X))
#endif

#define skipSpaces(d) while(isspace(*((d)->ptr))) (d)->ptr++

#define MOD_ERROR_VAL NULL
#define MOD_SUCCESS_VAL(val) val

static inline void
PyUnicode_ConcatAndDel(PyObject **left, PyObject *right)
{
    PyObject *newobj;
    newobj = PyUnicode_Concat(*left, right);
    Py_DECREF(*left);
    Py_DECREF(right);
    *left = newobj;
}


/* ------------------------------ Decoding ----------------------------- */

static PyObject*
decode_null(JSONData *jsondata)
{
    ptrdiff_t left;

    left = jsondata->end - jsondata->ptr;

    if (left >= 4 && strncmp(jsondata->ptr, "null", 4)==0) {
        jsondata->ptr += 4;
        Py_INCREF(Py_None);
        return Py_None;
    } else {
        PyErr_Format(JSON_DecodeError, "cannot parse JSON description: %.20s",
                     jsondata->ptr);
        return NULL;
    }
}


static PyObject*
decode_bool(JSONData *jsondata)
{
    ptrdiff_t left;

    left = jsondata->end - jsondata->ptr;

    if (left >= 4 && strncmp(jsondata->ptr, "true", 4)==0) {
        jsondata->ptr += 4;
        Py_INCREF(Py_True);
        return Py_True;
    } else if (left >= 5 && strncmp(jsondata->ptr, "false", 5)==0) {
        jsondata->ptr += 5;
        Py_INCREF(Py_False);
        return Py_False;
    } else {
        PyErr_Format(JSON_DecodeError, "cannot parse JSON description: %.20s",
                     jsondata->ptr);
        return NULL;
    }
}


static PyObject*
decode_string(JSONData *jsondata)
{
    int c, escaping, unicode_escape, string_escape, slash_unescape, has_non_ascii;
    Py_ssize_t len;
    char *ptr;
    PyObject *object = NULL;

    // look for the closing quote
    escaping = unicode_escape = string_escape = slash_unescape = has_non_ascii = False;
    ptr = jsondata->ptr + 1;
    while (True) {
        c = *ptr;
        if (c == 0) {
            PyErr_Format(JSON_DecodeError,
                         "unterminated string starting at position " SSIZE_T_F,
                         (Py_ssize_t)(jsondata->ptr - jsondata->str));
            return NULL;
        }
        if (!escaping) {
            if (c == '\\') {
                escaping = True;
            } else if (c == '"') {
                break;
            } else if (!isascii(c)) {
                has_non_ascii = True;
            }
        } else {
            switch(c) {
            case 'u':
                unicode_escape = True;
                break;
            case '"':
            case 'r':
            case 'n':
            case 't':
            case 'b':
            case 'f':
            case '\\':
                string_escape = True;
                break;
            case '/':
                slash_unescape = True;
                break;
            }
            escaping = False;
        }
        ptr++;
    }

    if (slash_unescape) {
        char *src, *dest;
        src = dest = jsondata->ptr + 1;
        while(src < ptr) {
            if (*src != '\\' || *(src+1) != '/') {
                *dest = *src;
                dest++;
            }
            src++;
        }
        len = dest - jsondata->ptr - 1;
    }
    else
        len = ptr - jsondata->ptr - 1;

    if (unicode_escape || 1 == jsondata->all_unicode || (jsondata->source_unicode && has_non_ascii)) {
        if (!jsondata->source_unicode && has_non_ascii) { // probably this is UTF-8 with unicode_escape
            
            PyObject *tmpObject = PyUnicode_DecodeUTF8(jsondata->ptr+1, len, NULL);
            if (tmpObject != NULL) {
                object = PyUnicode_AsRawUnicodeEscapeString(tmpObject); 
                Py_XDECREF(tmpObject);

                if (object) {
                    tmpObject = object;
                    object = PyUnicode_DecodeUnicodeEscape(PyUnicode_AS_UNICODE(tmpObject), PyUnicode_GET_SIZE(tmpObject), NULL);
                    Py_XDECREF(tmpObject);
                }
            }
        } else {
            object = PyUnicode_DecodeUnicodeEscape(jsondata->ptr+1, len, NULL);
        }

        if (object != NULL && 2 == jsondata->all_unicode) {
            PyObject *tmpObject = PyUnicode_AsUTF8String(object);
            Py_XDECREF(object);
            object = tmpObject;
        }
    }
    else if (string_escape)
        object = PyUnicode_DecodeUnicodeEscape(jsondata->ptr+1, len, NULL);
    else
        object = PyUnicode_FromStringAndSize(jsondata->ptr+1, len);

    if (object == NULL) {
        PyObject *type, *value, *tb, *reason;

        PyErr_Fetch(&type, &value, &tb);
        if (type == NULL) {
            PyErr_Format(JSON_DecodeError,
                         "invalid string starting at position " SSIZE_T_F,
                         (Py_ssize_t)(jsondata->ptr - jsondata->str));
        } else {
            if (PyErr_GivenExceptionMatches(type, PyExc_UnicodeDecodeError)) {
                reason = PyObject_GetAttrString(value, "reason");
                PyErr_Format(JSON_DecodeError, "cannot decode string starting"
                             " at position " SSIZE_T_F ": %s",
                             (Py_ssize_t)(jsondata->ptr - jsondata->str),
                             reason ? PyUnicode_AsUTF8(reason) : "bad format");
                Py_XDECREF(reason);
            } else {
                PyErr_Format(JSON_DecodeError,
                             "invalid string starting at position " SSIZE_T_F,
                             (Py_ssize_t)(jsondata->ptr - jsondata->str));
            }
        }
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(tb);
    } else {
        jsondata->ptr = ptr+1;
    }

    return object;
}


static PyObject*
decode_inf(JSONData *jsondata)
{
    PyObject *object;
    ptrdiff_t left;

    left = jsondata->end - jsondata->ptr;

    if (left >= 8 && strncmp(jsondata->ptr, "Infinity", 8)==0) {
        jsondata->ptr += 8;
        object = PyFloat_FromDouble(INFINITY);
        return object;
    } else if (left >= 9 && strncmp(jsondata->ptr, "+Infinity", 9)==0) {
        jsondata->ptr += 9;
        object = PyFloat_FromDouble(INFINITY);
        return object;
    } else if (left >= 9 && strncmp(jsondata->ptr, "-Infinity", 9)==0) {
        jsondata->ptr += 9;
        object = PyFloat_FromDouble(-INFINITY);
        return object;
    } else {
        PyErr_Format(JSON_DecodeError, "cannot parse JSON description: %.20s",
                     jsondata->ptr);
        return NULL;
    }
}


static PyObject*
decode_nan(JSONData *jsondata)
{
    PyObject *object;
    ptrdiff_t left;

    left = jsondata->end - jsondata->ptr;

    if (left >= 3 && strncmp(jsondata->ptr, "NaN", 3)==0) {
        jsondata->ptr += 3;
        object = PyFloat_FromDouble(NAN);
        return object;
    } else {
        PyErr_Format(JSON_DecodeError, "cannot parse JSON description: %.20s",
                     jsondata->ptr);
        return NULL;
    }
}


#define skipDigits(ptr) while(isdigit(*(ptr))) (ptr)++

static PyObject*
decode_number(JSONData *jsondata)
{
    PyObject *object, *str;
    int is_float;
    char *ptr;

    // validate number and check if it's floating point or not
    ptr = jsondata->ptr;
    is_float = False;

    if (*ptr == '-' || *ptr == '+')
        ptr++;

    if (*ptr == '0') {
        ptr++;
        if (isdigit(*ptr))
            goto number_error;
    } else if (isdigit(*ptr))
        skipDigits(ptr);
    else
        goto number_error;

    if (*ptr == '.') {
       is_float = True;
       ptr++;
       if (!isdigit(*ptr))
           goto number_error;
       skipDigits(ptr);
    }

    if (*ptr == 'e' || *ptr == 'E') {
       is_float = True;
       ptr++;
       if (*ptr == '+' || *ptr == '-')
           ptr++;
       if (!isdigit(*ptr))
           goto number_error;
       skipDigits(ptr);
    }

    str = PyUnicode_FromStringAndSize(jsondata->ptr, ptr - jsondata->ptr);
    if (str == NULL)
        return NULL;

    if (is_float)
        object = PyFloat_FromString(str);
    else
        object = PyLong_FromUnicodeObject(str, 10);

    Py_DECREF(str);

    if (object == NULL)
        goto number_error;

    jsondata->ptr = ptr;

    return object;

number_error:
    PyErr_Format(JSON_DecodeError, "invalid number starting at position "
                 SSIZE_T_F, (Py_ssize_t)(jsondata->ptr - jsondata->str));
    return NULL;
}


typedef enum {
    ArrayItem_or_ClosingBracket=0,
    Comma_or_ClosingBracket,
    ArrayItem,
    ArrayDone
} ArrayState;

static PyObject*
decode_array(JSONData *jsondata)
{
    PyObject *object, *item;
    ArrayState next_state;
    int c, result;
    char *start;

    object = PyList_New(0);

    start = jsondata->ptr;
    jsondata->ptr++;

    next_state = ArrayItem_or_ClosingBracket;

    while (next_state != ArrayDone) {
        skipSpaces(jsondata);
        c = *jsondata->ptr;
        if (c == 0) {
            PyErr_Format(JSON_DecodeError, "unterminated array starting at "
                         "position " SSIZE_T_F,
                         (Py_ssize_t)(start - jsondata->str));
            goto failure;
        }
        switch (next_state) {
        case ArrayItem_or_ClosingBracket:
            if (c == ']') {
                jsondata->ptr++;
                next_state = ArrayDone;
                break;
            }
        case ArrayItem:
            if (c==',' || c==']') {
                PyErr_Format(JSON_DecodeError, "expecting array item at "
                             "position " SSIZE_T_F,
                             (Py_ssize_t)(jsondata->ptr - jsondata->str));
                goto failure;
            }
            item = decode_json(jsondata);
            if (item == NULL)
                goto failure;
            result = PyList_Append(object, item);
            Py_DECREF(item);
            if (result == -1)
                goto failure;
            next_state = Comma_or_ClosingBracket;
            break;
        case Comma_or_ClosingBracket:
            if (c == ']') {
                jsondata->ptr++;
                next_state = ArrayDone;
            } else if (c == ',') {
                jsondata->ptr++;
                next_state = ArrayItem;
            } else {
                PyErr_Format(JSON_DecodeError, "expecting ',' or ']' at "
                             "position " SSIZE_T_F,
                             (Py_ssize_t)(jsondata->ptr - jsondata->str));
                goto failure;
            }
            break;
        case ArrayDone:
            // this will never be reached, but keep compilers happy
            break;
        }
    }

    return object;

failure:
    Py_DECREF(object);
    return NULL;
}


typedef enum {
    DictionaryKey_or_ClosingBrace=0,
    Comma_or_ClosingBrace,
    DictionaryKey,
    DictionaryDone
} DictionaryState;

static PyObject*
decode_object(JSONData *jsondata)
{
    PyObject *object, *key, *value;
    DictionaryState next_state;
    int c, result;
    char *start;

    object = PyDict_New();

    start = jsondata->ptr;
    jsondata->ptr++;

    next_state = DictionaryKey_or_ClosingBrace;

    while (next_state != DictionaryDone) {
        skipSpaces(jsondata);
        c = *jsondata->ptr;
        if (c == 0) {
            PyErr_Format(JSON_DecodeError, "unterminated object starting at "
                         "position " SSIZE_T_F,
                         (Py_ssize_t)(start - jsondata->str));
            goto failure;;
        }

        switch (next_state) {
        case DictionaryKey_or_ClosingBrace:
            if (c == '}') {
                jsondata->ptr++;
                next_state = DictionaryDone;
                break;
            }
        case DictionaryKey:
            if (c != '"') {
                PyErr_Format(JSON_DecodeError, "expecting object property name "
                             "at position " SSIZE_T_F,
                             (Py_ssize_t)(jsondata->ptr - jsondata->str));
                goto failure;
            }

            key = decode_json(jsondata);
            if (key == NULL)
                goto failure;

            skipSpaces(jsondata);
            if (*jsondata->ptr != ':') {
                PyErr_Format(JSON_DecodeError, "missing colon after object "
                             "property name at position " SSIZE_T_F,
                             (Py_ssize_t)(jsondata->ptr - jsondata->str));
                Py_DECREF(key);
                goto failure;
            } else {
                jsondata->ptr++;
            }

            skipSpaces(jsondata);
            if (*jsondata->ptr==',' || *jsondata->ptr=='}') {
                PyErr_Format(JSON_DecodeError, "expecting object property "
                             "value at position " SSIZE_T_F,
                             (Py_ssize_t)(jsondata->ptr - jsondata->str));
                Py_DECREF(key);
                goto failure;
            }

            value = decode_json(jsondata);
            if (value == NULL) {
                Py_DECREF(key);
                goto failure;
            }

            result = PyDict_SetItem(object, key, value);
            Py_DECREF(key);
            Py_DECREF(value);
            if (result == -1)
                goto failure;
            next_state = Comma_or_ClosingBrace;
            break;
        case Comma_or_ClosingBrace:
            if (c == '}') {
                jsondata->ptr++;
                next_state = DictionaryDone;
            } else if (c == ',') {
                jsondata->ptr++;
                next_state = DictionaryKey;
            } else {
                PyErr_Format(JSON_DecodeError, "expecting ',' or '}' at "
                             "position " SSIZE_T_F,
                             (Py_ssize_t)(jsondata->ptr - jsondata->str));
                goto failure;
            }
            break;
        case DictionaryDone:
            // this will never be reached, but keep compilers happy
            break;
        }
    }

    return object;

failure:
    Py_DECREF(object);
    return NULL;
}


static PyObject*
decode_json(JSONData *jsondata)
{
    PyObject *object;

    skipSpaces(jsondata);
    switch(*jsondata->ptr) {
    case 0:
        PyErr_SetString(JSON_DecodeError, "empty JSON description");
        return NULL;
    case '{':
        if (Py_EnterRecursiveCall(" while decoding a JSON object"))
            return NULL;
        object = decode_object(jsondata);
        Py_LeaveRecursiveCall();
        break;
    case '[':
        if (Py_EnterRecursiveCall(" while decoding a JSON array"))
            return NULL;
        object = decode_array(jsondata);
        Py_LeaveRecursiveCall();
        break;
    case '"':
        object = decode_string(jsondata);
        break;
    case 't':
    case 'f':
        object = decode_bool(jsondata);
        break;
    case 'n':
        object = decode_null(jsondata);
        break;
    case 'N':
        object = decode_nan(jsondata);
        break;
    case 'I':
        object = decode_inf(jsondata);
        break;
    case '+':
    case '-':
        if (*(jsondata->ptr+1) == 'I') {
            object = decode_inf(jsondata);
            break;
        }
        // fall through
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        object = decode_number(jsondata);
        break;
    default:
        PyErr_SetString(JSON_DecodeError, "cannot parse JSON description");
        return NULL;
    }

    return object;
}


/* ------------------------------ Encoding ----------------------------- */

/*
 * This function is an almost verbatim copy of unicodeescape_string() from
 * Python's unicodeobject.c with the following differences:
 *
 * - it always quotes the output using double quotes.
 * - it uses \u00hh instead of \xhh in output.
 * - it also quotes \b and \f
 */
static PyObject*
encode_unicode(PyObject *unicode)
{
    PyObject *repr;
    Py_UNICODE *s;
    Py_ssize_t size;
    Py_UNICODE *p;

    static const char *hexdigit = "0123456789abcdef";
#ifdef Py_UNICODE_WIDE
    const Py_ssize_t expandsize = 12;
#else
    const Py_ssize_t expandsize = 6;
#endif

    /* Initial allocation is based on the longest-possible unichr
       escape.

       In wide (UTF-32) builds '\U00xxxxxx' is 10 chars per source
       unichr, so in this case it's the longest unichr escape. In
       narrow (UTF-16) builds this is five chars per source unichr
       since there are two unichrs in the surrogate pair, so in narrow
       (UTF-16) builds it's not the longest unichr escape.

       In wide or narrow builds '\uxxxx' is 6 chars per source unichr,
       so in the narrow (UTF-16) build case it's the longest unichr
       escape.
    */

    s = PyUnicode_AS_UNICODE(unicode);
    size = PyUnicode_GET_SIZE(unicode);

    if (size > (PY_SSIZE_T_MAX-2-1)/expandsize) {
        PyErr_SetString(PyExc_OverflowError,
                        "unicode object is too large to make repr");
        return NULL;
    }

    repr = PyUnicode_FromStringAndSize(NULL, 2 + expandsize*size + 1);
    if (repr == NULL)
        return NULL;

    p = PyUnicode_AS_UNICODE(repr);

    *p++ = '"';

    while (size-- > 0) {
        Py_UNICODE ch = *s++;

        /* Escape quotes */
        if ((ch == (Py_UNICODE) PyUnicode_AS_UNICODE(repr)[0] || ch == '\\')) {
            *p++ = '\\';
            *p++ = (char) ch;
            continue;
        }

#ifdef Py_UNICODE_WIDE
        /* Map 21-bit characters to UTF-16 surrogate pairs */
        else if (ch >= 0x10000) {
            unsigned short ucs1, ucs2;
            ucs1 = (unsigned short)(((ch - 0x10000) >> 10) & 0x03FF) + 0xD800;
            ucs2 = (unsigned short)((ch - 0x10000) & 0x03FF) + 0xDC00;

            *p++ = '\\';
            *p++ = 'u';
            *p++ = hexdigit[(ucs1 >> 12) & 0x000F];
            *p++ = hexdigit[(ucs1 >> 8) & 0x000F];
            *p++ = hexdigit[(ucs1 >> 4) & 0x000F];
            *p++ = hexdigit[ucs1 & 0x000F];
            *p++ = '\\';
            *p++ = 'u';
            *p++ = hexdigit[(ucs2 >> 12) & 0x000F];
            *p++ = hexdigit[(ucs2 >> 8) & 0x000F];
            *p++ = hexdigit[(ucs2 >> 4) & 0x000F];
            *p++ = hexdigit[ucs2 & 0x000F];
            continue;
        }
#endif

        /* Map 16-bit characters to '\uxxxx' */
        if (ch >= 256) {
            *p++ = '\\';
            *p++ = 'u';
            *p++ = hexdigit[(ch >> 12) & 0x000F];
            *p++ = hexdigit[(ch >> 8) & 0x000F];
            *p++ = hexdigit[(ch >> 4) & 0x000F];
            *p++ = hexdigit[ch & 0x000F];
        }

        /* Map special whitespace to '\t', \n', '\r', '\f', '\b' */
        else if (ch == '\t') {
            *p++ = '\\';
            *p++ = 't';
        }
        else if (ch == '\n') {
            *p++ = '\\';
            *p++ = 'n';
        }
        else if (ch == '\r') {
            *p++ = '\\';
            *p++ = 'r';
        }
        else if (ch == '\f') {
            *p++ = '\\';
            *p++ = 'f';
        }
        else if (ch == '\b') {
            *p++ = '\\';
            *p++ = 'b';
        }

        /* Map non-printable US ASCII to '\u00hh' */
        else if (ch < ' ' || ch >= 0x7F) {
            *p++ = '\\';
            *p++ = 'u';
            *p++ = '0';
            *p++ = '0';
            *p++ = hexdigit[(ch >> 4) & 0x000F];
            *p++ = hexdigit[ch & 0x000F];
        }

        /* Copy everything else as-is */
        else
            *p++ = (char) ch;
    }

    *p++ = PyUnicode_AS_UNICODE(repr)[0];

    *p = '\0';
    PyUnicode_Resize(&repr, p - PyUnicode_AS_UNICODE(repr));
    return repr;
}


/*
 * This function is an almost verbatim copy of tuplerepr() from
 * Python's tupleobject.c with the following differences:
 *
 * - it uses encode_object() to get the object's JSON reprezentation.
 * - it uses [] as decorations instead of () (to masquerade as a JSON array).
 */

static PyObject*
encode_tuple(PyObject *tuple)
{
    Py_ssize_t i, n;
    PyObject *s, *temp;
    PyObject *pieces, *result = NULL;
    PyTupleObject *v = (PyTupleObject*) tuple;

    n = Py_SIZE(v);
    if (n == 0)
        return PyUnicode_FromString("[]");

    pieces = PyTuple_New(n);
    if (pieces == NULL)
        return NULL;

    /* Do repr() on each element. */
    for (i = 0; i < n; ++i) {
        s = encode_object(v->ob_item[i]);
        if (s == NULL)
            goto Done;
        PyTuple_SET_ITEM(pieces, i, s);
    }

    /* Add "[]" decorations to the first and last items. */
    assert(n > 0);
    s = PyUnicode_FromString("[");
    if (s == NULL)
        goto Done;
    temp = PyTuple_GET_ITEM(pieces, 0);
    PyUnicode_ConcatAndDel(&s, temp);
    PyTuple_SET_ITEM(pieces, 0, s);
    if (s == NULL)
        goto Done;

    s = PyUnicode_FromString("]");
    if (s == NULL)
        goto Done;
    temp = PyTuple_GET_ITEM(pieces, n-1);
    PyUnicode_ConcatAndDel(&temp, s);
    PyTuple_SET_ITEM(pieces, n-1, temp);
    if (temp == NULL)
        goto Done;

    /* Paste them all together with ", " between. */
    s = PyUnicode_FromString(", ");
    if (s == NULL)
        goto Done;
    result = PyUnicode_Join(s, pieces);
    Py_DECREF(s);

Done:
    Py_DECREF(pieces);
    return result;
}

/*
 * This function is an almost verbatim copy of list_repr() from
 * Python's listobject.c with the following differences:
 *
 * - it uses encode_object() to get the object's JSON reprezentation.
 * - it doesn't use the ellipsis to represent a list with references
 *   to itself, instead it raises an exception as such lists cannot be
 *   represented in JSON.
 */
static PyObject*
encode_list(PyObject *list)
{
    Py_ssize_t i;
    PyObject *s, *temp;
    PyObject *pieces = NULL, *result = NULL;
    PyListObject *v = (PyListObject*) list;

    i = Py_ReprEnter((PyObject*)v);
    if (i != 0) {
        if (i > 0) {
            PyErr_SetString(JSON_EncodeError, "a list with references to "
                            "itself is not JSON encodable");
        }
        return NULL;
    }

    if (Py_SIZE(v) == 0) {
        result = PyUnicode_FromString("[]");
        goto Done;
    }

    pieces = PyList_New(0);
    if (pieces == NULL)
        goto Done;

    /* Do repr() on each element.  Note that this may mutate the list,
     * so must refetch the list size on each iteration. */
    for (i = 0; i < Py_SIZE(v); ++i) {
        int status;
        s = encode_object(v->ob_item[i]);
        if (s == NULL)
            goto Done;
        status = PyList_Append(pieces, s);
        Py_DECREF(s);  /* append created a new ref */
        if (status < 0)
            goto Done;
    }

    /* Add "[]" decorations to the first and last items. */
    assert(PyList_GET_SIZE(pieces) > 0);
    s = PyUnicode_FromString("[");
    if (s == NULL)
        goto Done;
    temp = PyList_GET_ITEM(pieces, 0);
    PyUnicode_ConcatAndDel(&s, temp);
    PyList_SET_ITEM(pieces, 0, s);
    if (s == NULL)
        goto Done;

    s = PyUnicode_FromString("]");
    if (s == NULL)
        goto Done;
    temp = PyList_GET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1);
    PyUnicode_ConcatAndDel(&temp, s);
    PyList_SET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1, temp);
    if (temp == NULL)
        goto Done;

    /* Paste them all together with ", " between. */
    s = PyUnicode_FromString(", ");
    if (s == NULL)
        goto Done;
    result = PyUnicode_Join(s, pieces);
    Py_DECREF(s);

Done:
    Py_XDECREF(pieces);
    Py_ReprLeave((PyObject *)v);
    return result;
}


/*
 * This function is an almost verbatim copy of dict_repr() from
 * Python's dictobject.c with the following differences:
 *
 * - it uses encode_object() to get the object's JSON reprezentation.
 * - only accept strings for keys.
 * - it doesn't use the ellipsis to represent a dictionary with references
 *   to itself, instead it raises an exception as such dictionaries cannot
 *   be represented in JSON.
 */
static PyObject*
encode_dict(PyObject *dict)
{
    Py_ssize_t i;
    PyObject *s, *temp, *colon = NULL;
    PyObject *pieces = NULL, *result = NULL;
    PyObject *key, *value;
    PyDictObject *mp = (PyDictObject*) dict;

    i = Py_ReprEnter((PyObject *)mp);
    if (i != 0) {
        if (i > 0) {
            PyErr_SetString(JSON_EncodeError, "a dict with references to "
                            "itself is not JSON encodable");
        }
        return NULL;
    }

    if (mp->ma_used == 0) {
        result = PyUnicode_FromString("{}");
        goto Done;
    }

    pieces = PyList_New(0);
    if (pieces == NULL)
        goto Done;

    colon = PyUnicode_FromString(": ");
    if (colon == NULL)
        goto Done;

    /* Do repr() on each key+value pair, and insert ": " between them.
     * Note that repr may mutate the dict. */
    i = 0;
    while (PyDict_Next((PyObject *)mp, &i, &key, &value)) {
        int status;

        if (!PyUnicode_Check(key)) {
            PyErr_SetString(JSON_EncodeError, "JSON encodable dictionaries "
                            "must have string/unicode keys");
            goto Done;
        }

        /* Prevent repr from deleting value during key format. */
        Py_INCREF(value);
        temp = encode_object(key);
        s = PyUnicode_Concat(temp, colon);
        Py_DECREF(temp);
        PyUnicode_ConcatAndDel(&s, encode_object(value));
        Py_DECREF(value);
        if (s == NULL)
            goto Done;
        status = PyList_Append(pieces, s);
        Py_DECREF(s);  /* append created a new ref */
        if (status < 0)
            goto Done;
    }

    /* Add "{}" decorations to the first and last items. */
    assert(PyList_GET_SIZE(pieces) > 0);
    s = PyUnicode_FromString("{");
    if (s == NULL)
        goto Done;
    temp = PyList_GET_ITEM(pieces, 0);
    PyUnicode_ConcatAndDel(&s, temp);
    PyList_SET_ITEM(pieces, 0, s);
    if (s == NULL)
        goto Done;

    s = PyUnicode_FromString("}");
    if (s == NULL)
        goto Done;
    temp = PyList_GET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1);
    PyUnicode_ConcatAndDel(&temp, s);
    PyList_SET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1, temp);
    if (temp == NULL)
        goto Done;

    /* Paste them all together with ", " between. */
    s = PyUnicode_FromString(", ");
    if (s == NULL)
        goto Done;
    result = PyUnicode_Join(s, pieces);
    Py_DECREF(s);

Done:
    Py_XDECREF(pieces);
    Py_XDECREF(colon);
    Py_ReprLeave((PyObject *)mp);
    return result;
}


static PyObject*
encode_object(PyObject *object)
{
    if (object == Py_True) {
        return PyUnicode_FromString("true");
    } else if (object == Py_False) {
        return PyUnicode_FromString("false");
    } else if (object == Py_None) {
        return PyUnicode_FromString("null");
    } else if (PyUnicode_Check(object)) {
        return encode_unicode(object);
    } else if (PyFloat_Check(object)) {
        double val = PyFloat_AS_DOUBLE(object);
        if (Py_IS_NAN(val)) {
            return PyUnicode_FromString("NaN");
        } else if (Py_IS_INFINITY(val)) {
            if (val > 0) {
                return PyUnicode_FromString("Infinity");
            } else {
                return PyUnicode_FromString("-Infinity");
            }
        } else {
            return PyObject_Repr(object);
        }
    } else if (PyLong_Check(object)) {
        return PyObject_Str(object);
    } else if (PyList_Check(object)) {
        PyObject *result;
        if (Py_EnterRecursiveCall(" while encoding a JSON array from a Python list"))
            return NULL;
        result = encode_list(object);
        Py_LeaveRecursiveCall();
        return result;
    } else if (PyTuple_Check(object)) {
        PyObject *result;
        if (Py_EnterRecursiveCall(" while encoding a JSON array from a Python tuple"))
            return NULL;
        result = encode_tuple(object);
        Py_LeaveRecursiveCall();
        return result;
    } else if (PyDict_Check(object)) { // use PyMapping_Check(object) instead? -Dan
        PyObject *result;
        if (Py_EnterRecursiveCall(" while encoding a JSON object"))
            return NULL;
        result = encode_dict(object);
        Py_LeaveRecursiveCall();
        return result;
    } else {
        PyErr_SetString(JSON_EncodeError, "object is not JSON encodable");
        return NULL;
    }
}


/* Encode object into its JSON representation */

static PyObject*
JSON_encode(PyObject *self, PyObject *object)
{
    return encode_object(object);
}


/* Decode JSON representation into pyhton objects */

static PyObject*
JSON_decode(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"json", "all_unicode", NULL};
    int all_unicode = False; // by default return unicode only when needed
    PyObject *object, *string, *str;
    JSONData jsondata;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|i:decode", kwlist,
                                     &string, &all_unicode))
        return NULL;

    if (PyUnicode_Check(string)) {
        str = PyUnicode_AsRawUnicodeEscapeString(string);
        if (str == NULL) {
            return NULL;
        }
        jsondata.source_unicode = True;
    } else {
        Py_INCREF(string);
        str = string;
        jsondata.source_unicode = False;
    }

    if (PyBytes_AsStringAndSize(str, &(jsondata.str), NULL) == -1) {
        Py_DECREF(str);
        return NULL; // not a string object or it contains null bytes
    }

    jsondata.ptr = jsondata.str;
    jsondata.end = jsondata.str + PyUnicode_GET_SIZE(str);
    jsondata.all_unicode = all_unicode;

    object = decode_json(&jsondata);

    if (object != NULL) {
        skipSpaces(&jsondata);
        if (jsondata.ptr < jsondata.end) {
            PyErr_Format(JSON_DecodeError, "extra data after JSON description"
                         " at position " SSIZE_T_F,
                         (Py_ssize_t)(jsondata.ptr - jsondata.str));
            Py_DECREF(str);
            Py_DECREF(object);
            return NULL;
        }
    }

    Py_DECREF(str);

    return object;
}


/* List of functions defined in the module */

static PyMethodDef cjson_methods[] = {
    {"encode", (PyCFunction)JSON_encode,  METH_O,
    PyDoc_STR("encode(object) -> generate the JSON representation for object.")},

    {"decode", (PyCFunction)JSON_decode,  METH_VARARGS|METH_KEYWORDS,
    PyDoc_STR("decode(string, all_unicode=False) -> parse the JSON representation into\n"
              "python objects. The optional argument `all_unicode', specifies how to\n"
              "convert the strings in the JSON representation into python objects.\n"
              "If it is False (default), it will return strings everywhere possible\n"
              "and unicode objects only where necessary, else it will return unicode\n"
              "objects everywhere (this is slower).")},

    {NULL, NULL}  // sentinel
};

PyDoc_STRVAR(module_doc,
"Fast JSON encoder/decoder module."
);

/* Initialization function for the module (*must* be called initcjson) */

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"e2icjson",          /* m_name */
	"e2icjson",          /* m_doc */
	-1,                  /* m_size */
	cjson_methods,       /* m_methods */
	NULL,                /* m_reload */
	NULL,                /* m_traverse */
	NULL,                /* m_clear */
	NULL,                /* m_free */
};

PyMODINIT_FUNC PyInit_e2icjson(void)
{
    PyObject *m;

    m = PyModule_Create(&moduledef);

    if (m == NULL)
        return MOD_ERROR_VAL;

    JSON_Error = PyErr_NewException("e2icjson.Error", NULL, NULL);
    if (JSON_Error == NULL)
        return;
    Py_INCREF(JSON_Error);
    PyModule_AddObject(m, "Error", JSON_Error);

    JSON_EncodeError = PyErr_NewException("e2icjson.EncodeError", JSON_Error, NULL);
    if (JSON_EncodeError == NULL)
        return;
    Py_INCREF(JSON_EncodeError);
    PyModule_AddObject(m, "EncodeError", JSON_EncodeError);

    JSON_DecodeError = PyErr_NewException("e2icjson.DecodeError", JSON_Error, NULL);
    if (JSON_DecodeError == NULL)
        return;
    Py_INCREF(JSON_DecodeError);
    PyModule_AddObject(m, "DecodeError", JSON_DecodeError);

    // Module version (the MODULE_VERSION macro is defined by setup.py)
    PyModule_AddStringConstant(m, "__version__", string(MODULE_VERSION));
    return MOD_SUCCESS_VAL(m);
}


