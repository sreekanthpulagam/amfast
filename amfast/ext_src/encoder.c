#include <Python.h>
#include <string.h>
#include <time.h>

#ifndef DATETIME_H
#include <datetime.h>
#endif

#include "amf_common.h"

// Use to test for endianness at run time.
const int endian_test = 1;
#define is_bigendian() ((*(char*)&endian_test) == 0)

// ------------------------ DECLARATIONS --------------------------------- //

// ---- GLOBALS
static PyObject *xml_dom_mod;
static PyObject *amfast_mod;
static PyObject *class_def_mod;
static PyObject *amfast_Error;
static PyObject *amfast_EncodeError;
static int big_endian; // Flag == 1 if architecture is big_endian, == 0 if not

// ---- ENCODING CONTEXT

/* Context for encoding. */
typedef struct {
    PyObject *include_private; // PyBool, if True encode attributes starting with '_'.
    PyObject *class_def_mapper; // Object for getting a ClassDef for an object.
    PyObject *get_class_def_method_name; // Name of the method to call to retrieve a class def.
    PyObject *array_collection_def; // Keep a copy of a ClassDef for array collections
    PyObject *object_proxy_def; // Keep a copy of a ClassDef for ObjectProxies
    ObjectContext *string_refs; // Keep track of string references
    ObjectContext *object_refs; // Keep track of object references
    ObjectContext *class_refs; // Keep track of class definitions references
    char *buf; // Output buffer
    size_t buf_len; // Current length of string in output buffer
    size_t buf_size; // Current size of output buffer
    int use_array_collections; // Flag == 1 to encode lists and tuples to ArrayCollection
    int use_object_proxies; // Flag == 1 to encode dicts to ObjectProxy
    int use_references; // Flag == 1 to encode multiply occuring objects as references
    int use_legacy_xml; // Flag == 1 to encode XML as legacy XMLDocument instead of E4X
    int use_amf3; // Flag == 1 to encode packet messages in AMF3
} EncoderContext;

static EncoderContext* _create_encoder_context(size_t size, int amf3);
static EncoderContext* _copy_encoder_context(EncoderContext *context, int amf3);
static int _destroy_encoder_context(EncoderContext *context);
static int _increase_buffer_size(EncoderContext *context, size_t size);
static int _amf_write_string_size(EncoderContext *context, char *value, size_t size);
static int _amf_write_string(EncoderContext *context, char *value);
static int _amf_write_byte(EncoderContext *context, char value);

// ---- ENCODING

/*
 * Use these functions to serialize Python objects.
 *
 * write... functions that encode including type marker.
 * serialize... functions that encode with reference.
 * encode... functions that encode PyObjects to AMF
 */

static int _encode_double(EncoderContext *context, double value);
static int encode_float(EncoderContext *context, PyObject *value);
static int encode_long(EncoderContext *context, PyObject *value);
static int _encode_int(EncoderContext *context, int value);
static int write_int(EncoderContext *context, PyObject *value);
static int encode_none(EncoderContext *context);
static int encode_bool(EncoderContext *context, PyObject *value);
static int serialize_unicode(EncoderContext *context, PyObject *value);
static int encode_unicode(EncoderContext *context, PyObject *value);
static int serialize_string(EncoderContext *context, PyObject *value);
static int encode_string(EncoderContext *context, PyObject *value);
static int serialize_object_as_string(EncoderContext *context, PyObject *value);
static int write_list(EncoderContext *context, PyObject *value);
static int serialize_list(EncoderContext *context, PyObject *value);
static int encode_list(EncoderContext *context, PyObject *value);
static int _encode_array_collection_header(EncoderContext *context);
static int serialize_dict(EncoderContext *context, PyObject *value);
static int encode_dict(EncoderContext *context, PyObject *value);
static int _encode_dynamic_dict(EncoderContext *context, PyObject *value);
static int _encode_object_proxy_header(EncoderContext *context);
static int serialize_date(EncoderContext *context, PyObject *value);
static int encode_date(EncoderContext *context, PyObject *value);
static int encode_reference(EncoderContext *context, ObjectContext *object_context, PyObject *value, int bit);
static int write_xml(EncoderContext *context, PyObject *value);
static int serialize_xml(EncoderContext *context, PyObject *value);
static int serialize_object(EncoderContext *context, PyObject *value);
static int encode_object(EncoderContext *context, PyObject *value);
static int serialize_class_def(EncoderContext *context, PyObject *value);
static int encode_class_def(EncoderContext *context, PyObject *value);
static int serialize_byte_array(EncoderContext *context, PyObject *value);
static int encode_byte_array(EncoderContext *context, PyObject *value);

static int check_xml(PyObject *value);

// AMF0
static int encode_bool_AMF0(EncoderContext *context, PyObject *value);
static int encode_int_AMF0(EncoderContext *context, PyObject *value);
static int encode_long_AMF0(EncoderContext *context, PyObject *value);
static int encode_float_AMF0(EncoderContext *context, PyObject *value);
static int _encode_ushort(EncoderContext *context, unsigned short value);
static int _encode_ulong(EncoderContext *context, unsigned int value);
static int write_string_AMF0(EncoderContext *context, PyObject *value);
static int encode_string_AMF0(EncoderContext *context, PyObject *value, int allow_long);
static int write_unicode_AMF0(EncoderContext *context, PyObject *value);
static int encode_unicode_AMF0(EncoderContext *context, PyObject *value, int allow_long);
static int write_reference_AMF0(EncoderContext *context, PyObject *value);
static int write_list_AMF0(EncoderContext *context, PyObject *value, int write_reference);
static int write_dict_AMF0(EncoderContext *context, PyObject *value);
static int _encode_dynamic_dict_AMF0(EncoderContext *context, PyObject *value);
static int encode_object_as_string_AMF0(EncoderContext *context, PyObject *value, int allow_long);
static int write_date_AMF0(EncoderContext *context, PyObject *value);
static int encode_class_def_AMF0(EncoderContext *context, PyObject *value);
static int _encode_object_AMF0(EncoderContext *context, PyObject *value);
static int write_object_AMF0(EncoderContext *context, PyObject *value);
static int write_anonymous_object_AMF0(EncoderContext *context, PyObject *value);
static int _encode_packet(EncoderContext *context, PyObject *value);
static int encode_packet_header_AMF0(EncoderContext *context, PyObject *value);
static int encode_packet_message_AMF0(EncoderContext *context, PyObject *value);

static PyObject* encode(PyObject *self, PyObject *args, PyObject *kwargs);
static PyObject * class_def_from_class(EncoderContext *context, PyObject *value);
static PyObject * attributes_from_object(EncoderContext *context, PyObject *value);
static PyObject* static_attr_vals_from_class_def(PyObject *class_def, PyObject *value);
static PyObject* dynamic_attrs_from_class_def(PyObject *class_def, PyObject *value);
static int _encode(EncoderContext *context, PyObject *value);
static int _encode_AMF0(EncoderContext *context, PyObject *value);
static int _encode_object(EncoderContext *context, PyObject *value);

// ------------------------ ENCODING CONTEXT ------------------------ //

/* Create a new EncoderContext. */
static EncoderContext* _create_encoder_context(size_t size, int amf3)
{
    EncoderContext *context;
    context = (EncoderContext*)malloc(sizeof(EncoderContext));
    if (!context) {
        PyErr_SetNone(PyExc_MemoryError);
        return NULL;
    }

    // Set python object pointers to NULL
    context->include_private = NULL;
    context->class_def_mapper = NULL;
    context->get_class_def_method_name = NULL;
    context->array_collection_def = NULL;
    context->object_proxy_def = NULL;

    // Setup buffer
    context->buf_size = size; // initial buffer size.
    context->buf_len = 0; // Nothing is in the buffer yet
    context->buf = (char*)malloc(sizeof(char*) * context->buf_size); // Create buffer
    if (!context->buf) {
        PyErr_SetNone(PyExc_MemoryError);
        free(context);
        return NULL;
    }

    if (amf3) {
        context->string_refs = create_object_context(64);
        if (!context->string_refs) {
            free(context->buf);
            free(context);
            return NULL;
        }
    } else {
        context->string_refs = NULL;
    }

    context->object_refs = create_object_context(64);
    if (!context->object_refs) {
        if (context->string_refs) {
            destroy_object_context(context->string_refs);
        }
        free(context->buf);
        free(context);
        return NULL;
    }

    if (amf3) {
        context->class_refs = create_object_context(64);
        if (!context->class_refs) {
            if (context->string_refs) {
                destroy_object_context(context->string_refs);
            }
            if (context->object_refs) {
                destroy_object_context(context->object_refs);
            }
            free(context->buf);
            free(context);
            return NULL;
        }
    } else {
        context->class_refs = NULL;
    }

    context->use_references = 1;
    context->use_amf3 = 0;
    return context;
}

/*
 * Creates a new context, and copies over the existing values.
 *
 * Use this when you need to reset reference counts.
 */
static EncoderContext* _copy_encoder_context(EncoderContext *context, int amf3)
{
    EncoderContext *new_context = _create_encoder_context(context->buf_size, amf3);
    if (!new_context)
        return NULL;

    new_context->use_references = context->use_references;
    new_context->use_amf3 = context->use_amf3;
    new_context->use_array_collections = context->use_array_collections;
    new_context->use_object_proxies = context->use_object_proxies;

    new_context->class_def_mapper = context->class_def_mapper;
    if (new_context->class_def_mapper) {
        Py_INCREF(new_context->class_def_mapper);
    }

    new_context->get_class_def_method_name = context->get_class_def_method_name;
    if (new_context->get_class_def_method_name) {
        Py_INCREF(new_context->get_class_def_method_name);
    }

    new_context->include_private = context->include_private;
    if (new_context->include_private) {
        Py_INCREF(new_context->include_private);
    }

    new_context->array_collection_def = context->array_collection_def;
    if (new_context->array_collection_def) {
        Py_INCREF(new_context->array_collection_def);
    }

    new_context->object_proxy_def = context->object_proxy_def;
    if (new_context->object_proxy_def) {
        Py_INCREF(new_context->object_proxy_def);
    }

    return new_context;
}

/* De-allocate an EncoderContext. */
static int _destroy_encoder_context(EncoderContext *context)
{
    if (context->string_refs) {
        destroy_object_context(context->string_refs);
    }

    if (context->object_refs) {
        destroy_object_context(context->object_refs);
    }

    if (context->class_refs) {
        destroy_object_context(context->class_refs);
    }

    if (context->class_def_mapper) {
        Py_DECREF(context->class_def_mapper);
    }

    if (context->get_class_def_method_name) {
        Py_DECREF(context->get_class_def_method_name);
    }

    if (context->include_private) {
        Py_DECREF(context->include_private);
    }
  
    if (context->array_collection_def) {
        Py_DECREF(context->array_collection_def);
    }

    if (context->object_proxy_def) {
        Py_DECREF(context->object_proxy_def);
    }

    free(context->buf);
    free(context);
    return 1;
}

/*
 * Expand the size of the buffer.
 *
 * size == amount to expand by.
 */
static int _increase_buffer_size(EncoderContext *context, size_t size)
{
    const size_t new_len = context->buf_len + size;
    size_t current_size = context->buf_size;

    while (new_len > current_size) {
        // Buffer is not large enough.
        // Double its memory, so that we don't need to realloc everytime.
        current_size *= 2;
    }

    if (current_size != context->buf_size) {
        context->buf_size = current_size;
        context->buf = (char*)realloc(context->buf, sizeof(char*) * context->buf_size);
        if (!context->buf) {
            PyErr_SetNone(PyExc_MemoryError);
            return 0;
        }
    }

    return 1;
}

/* Concat a byte array with a known size into the buffer. */
static int _amf_write_string_size(EncoderContext *context, char *value, size_t size)
{
    if (_increase_buffer_size(context, size) != 1)
        return 0;

    memcpy(context->buf + context->buf_len, value, size);
    context->buf_len += size;
    return 1;
}

/* Concat a string value to the buffer (must be \0 terminated). */
static int _amf_write_string(EncoderContext *context, char *value)
{
    return _amf_write_string_size(context, value, strlen(value));
}

/* Concat a single byte to the buffer. */
static int _amf_write_byte(EncoderContext *context, char value)
{
    if (_increase_buffer_size(context, 1) != 1) {
        return 0;
    }

    context->buf[context->buf_len] = value;
    context->buf_len += 1;
    return 1;
}

// ------------------------ ENCODING -------------------------------- //

/* Encode a native C double. */
static int _encode_double(EncoderContext *context, double value)
{
   // Put bytes from double into byte array
   union aligned { // use the same memory for d_value and c_value
       double d_value;
       char c_value[8];
   } d_aligned;
   char *char_value = d_aligned.c_value;
   d_aligned.d_value = value;

   // AMF numbers are encoded in big endianness
   if (big_endian) {
       return _amf_write_string_size(context, char_value, 8);
   } else {
       // Flip endianness
       char flipped[8] = {char_value[7], char_value[6], char_value[5], char_value[4], char_value[3], char_value[2], char_value[1], char_value[0]};
       return _amf_write_string_size(context, flipped, 8);
   }
}

/* Encode a PyFloat. */
static int encode_float(EncoderContext *context, PyObject *value)
{
    double n = PyFloat_AsDouble(value);
    if (n == -1.0)
        return 0;
    return _encode_double(context, n);
}

/* Encode a PyLong. */
static int encode_long(EncoderContext *context, PyObject *value)
{
    double n = PyLong_AsDouble(value);
    if (n == -1.0)
        return 0;
    return _encode_double(context, n);
}

/* Encode a native C int. */
static int _encode_int(EncoderContext *context, int value)
{
    char tmp[4];
    size_t tmp_size;

    /*
     * Int can be up to 4 bytes long.
     *
     * The first bit of the first 3 bytes
     * is set if another byte follows.
     *
     * The integer value is the last 7 bits from
     * the first 3 bytes and the 8 bits of the last byte
     * (29 bits).
     *
     * The int is negative if the 1st bit of the 29 int is set.
     */
    value &= 0x1fffffff; // Ignore 1st 3 bits of 32 bit int, since we're encoding to 29 bit.
    if (value < 0x80) {
        tmp_size = 1;
        tmp[0] = value;
    } else if (value < 0x4000) {
        tmp_size = 2;
        tmp[0] = (value >> 7 & 0x7f) | 0x80; // Shift bits by 7 to fill 1st byte and set next byte flag
        tmp[1] = value & 0x7f; // Shift bits by 7 to fill 2nd byte, leave next byte flag unset
    } else if (value < 0x200000) {
        tmp_size = 3;
        tmp[0] = (value >> 14 & 0x7f) | 0x80;
        tmp[1] = (value >> 7 & 0x7f) | 0x80;
        tmp[2] = value & 0x7f;
    } else if (value < 0x40000000) {
        tmp_size = 4;
        tmp[0] = (value >> 22 & 0x7f) | 0x80;
        tmp[1] = (value >> 15 & 0x7f) | 0x80;
        tmp[2] = (value >> 8 & 0x7f) | 0x80; // Shift bits by 8, since we can use all bits in the 4th byte
        tmp[3] = (value & 0xff);
    } else {
        PyErr_SetString(amfast_EncodeError, "Int is too big to be encoded by AMF.");
        return 0;        
    }

    return _amf_write_string_size(context, tmp, tmp_size);
}

/* Writes a PyInt. */
static int write_int(EncoderContext *context, PyObject *value)
{
    long n = PyInt_AsLong(value);
    if (n < MAX_INT && n > MIN_INT) {
        // Int is in valid AMF3 int range.
        if (_amf_write_byte(context, INT_TYPE) != 1)
            return 0;
        return _encode_int(context, n);
    } else {
        // Int is too big, it must be encoded as a double
        if (_amf_write_byte(context, DOUBLE_TYPE) != 1)
            return 0;
        return _encode_double(context, (double)n);
    }
}

/* Encode a Py_None. */
static int encode_none(EncoderContext *context)
{
    return _amf_write_byte(context, NULL_TYPE);
}

/* Encode a PyBool. */
static int encode_bool(EncoderContext *context, PyObject *value)
{
    if (value == Py_True) {
        return _amf_write_byte(context, TRUE_TYPE);
    } else {
        return _amf_write_byte(context, FALSE_TYPE);
    }
}

/* Serialize a PyUnicode. */
static int serialize_unicode(EncoderContext *context, PyObject *value)
{
    // Check for empty string
    if (PyUnicode_GET_SIZE(value) == 0) {
        // References are never used for empty strings.
        return _amf_write_byte(context, EMPTY_STRING_TYPE);
    }

    // Check for idx
    int return_value = encode_reference(context, context->string_refs, value, 0);
    if (return_value > -1)
        return return_value;

    return encode_unicode(context, value); 
}

/* Encode a PyUnicode. */
static int encode_unicode(EncoderContext *context, PyObject *value)
{
    PyObject *PyString_value = PyUnicode_AsUTF8String(value);
    return encode_string(context, PyString_value);
}

/* Serialize a PyString. */
static int serialize_string(EncoderContext *context, PyObject *value)
{
    // Check for empty string
    if (PyString_GET_SIZE(value) == 0) {
        // References are never used for empty strings.
        return _amf_write_byte(context, EMPTY_STRING_TYPE);
    }

    // Check for idx
    int return_value = encode_reference(context, context->string_refs, value, 0);
    if (return_value > -1)
        return return_value;

    return encode_string(context, value);
}

/* Encode a PyString as UTF8. */
static int encode_string(EncoderContext *context, PyObject *value)
{
    /*
     *  TODO: The following code assumes all strings
     *  are already UTF8 or ASCII.
     *
     *  Should we check to make sure,
     *  or just let the client pick it up?
     */
    char *char_value = PyString_AS_STRING(value);
    Py_ssize_t string_len = PyString_GET_SIZE(value);
    if (!char_value)
        return 0;

    // Add size of string to header
    if (!_encode_int(context, ((int)string_len) << 1 | REFERENCE_BIT))
        return 0;

    // Write string.
    return _amf_write_string_size(context, char_value, (int)string_len);
}

/* Encode a PyString or a PyUnicode. */
static int serialize_object_as_string(EncoderContext *context, PyObject *value)
{
    if (PyString_Check(value)) {
        return serialize_string(context, value);
    } else if (PyUnicode_Check(value)) {
        return serialize_unicode(context, value);
    }

    PyObject *str_rep = PyObject_Str(value);
    if (!str_rep)
        return 0;

    int return_value = serialize_string(context, str_rep);
    Py_DECREF(str_rep);
    return return_value;
}

/* Encode a PyString or a PyUnicode to AMF0. */
static int encode_object_as_string_AMF0(EncoderContext *context, PyObject *value, int allow_long)
{
    if (PyUnicode_Check(value)) {
        return encode_unicode_AMF0(context, value, allow_long);
    } else if (PyString_Check(value)) {
        return encode_string_AMF0(context, value, allow_long);
    }

    PyObject *str_rep = PyObject_Str(value);
    if (!str_rep)
        return 0;

    int return_value = encode_string_AMF0(context, str_rep, allow_long);
    Py_DECREF(str_rep);
    return return_value;
}

/* Encode an ArrayCollection header. */
static int _encode_array_collection_header(EncoderContext *context)
{
    // If ClassDef object has not been created,
    // for ArrayCollection, create it.
    if (!context->array_collection_def) {
        PyObject *method_name = PyString_FromString("getClassDefByAlias");
        if (!method_name)
            return 0;

        PyObject *alias = PyString_FromString("flex.messaging.io.ArrayCollection");
        if (!alias) {
            Py_DECREF(method_name);
            return 0;
        }

        PyObject *class_def = PyObject_CallMethodObjArgs(context->class_def_mapper, method_name, alias, NULL);
        Py_DECREF(method_name);
        Py_DECREF(alias);

        if (!class_def)
            return 0;

        context->array_collection_def = class_def;
    }

    // Write ArrayCollectionClassDef to buf.
    if (!serialize_class_def(context, context->array_collection_def))
        return 0;

    // Add an extra item to the index for the array following the collection
    if (!map_next_object_ref(context->object_refs, Py_None))
        return 0;

    return _amf_write_byte(context, ARRAY_TYPE);
}

/* Writes a PyList or PyTuple. */
static int write_list(EncoderContext *context, PyObject *value)
{
    // Write type marker
    if (context->use_array_collections == 1) {
        if (!_amf_write_byte(context, OBJECT_TYPE))
            return 0;
    } else {
        if (_amf_write_byte(context, ARRAY_TYPE) != 1)
            return 0;
    }
    
    return serialize_list(context, value);
}

/* Serializes a PyList or PyTuple. */
static int serialize_list(EncoderContext *context, PyObject *value)
{
    // Check for idx
    int return_value = encode_reference(context, context->object_refs, value, 0);
    if (return_value > -1)
        return return_value;

    // Write ArrayCollection header
    if (context->use_array_collections == 1) {
        if (!_encode_array_collection_header(context))
            return 0;
    }

    return encode_list(context, value);
}

/* Encode a PyList or PyTuple. */
static int encode_list(EncoderContext *context, PyObject *value)
{
    // Add size of list to header
    Py_ssize_t value_len = PySequence_Size(value);
    if (value_len < 0)
        return 0;

    if (!_encode_int(context, ((int)value_len) << 1 | NULL_TYPE))
        return 0;

    // We're never writing associative array items
    if (!_amf_write_byte(context, NULL_TYPE))
        return 0;

    // Encode each value in the list
    int i;
    for (i = 0; i < value_len; i++) {
        // Increment ref count in case 
        // list is modified by someone else.
        PyObject *list_item = PySequence_GetItem(value, i);
        if (!list_item)
            return 0;

        int return_value = _encode(context, list_item);
        Py_DECREF(list_item);
        if (!return_value)
            return 0;
    }

    return 1;
}

/* Encode an ObjectProxy header. */
static int _encode_object_proxy_header(EncoderContext *context)
{
    // If ClassDef object has not been created
    // for ObjectProxy, create it.
    if (!context->object_proxy_def) {
        PyObject *method_name = PyString_FromString("getClassDefByAlias");
        if (!method_name) 
            return 0;

        PyObject *alias = PyString_FromString("flex.messaging.io.ObjectProxy");
        if (!alias) {
            Py_DECREF(method_name);
            return 0;
        }

        PyObject *class_def = PyObject_CallMethodObjArgs(context->class_def_mapper, method_name, alias, NULL);
        Py_DECREF(method_name);
        Py_DECREF(alias);

        if (!class_def)
            return 0;

        context->object_proxy_def = class_def;
    }

    // Encode object proxy class def.
    if (!serialize_class_def(context, context->object_proxy_def))
         return 0;

    // Add an extra item to the index for the object following the proxy
    if (!map_next_object_ref(context->object_refs, Py_None))
        return 0;

    // Include type marker
    return _amf_write_byte(context, OBJECT_TYPE);
}

/* Serialize a PyDict. */
static int serialize_dict(EncoderContext *context, PyObject *value)
{
    // Check for idx
    int return_value = encode_reference(context, context->object_refs, value, 0);
    if (return_value > -1) {
        return return_value;
    }

    // Write ObjectProxy header
    if (context->use_object_proxies == 1) {
        if (!_encode_object_proxy_header(context))
            return 0;
    }

    return encode_dict(context, value);
}

/* Encode a dict. */
static int encode_dict(EncoderContext *context, PyObject *value)
{
    if (!serialize_class_def(context, Py_None)) {
        return 0;
    }
    
    return _encode_dynamic_dict(context, value);
}

/* Encode the key/value pairs of a dict. */
static int _encode_dynamic_dict(EncoderContext *context, PyObject *value)
{
    PyObject *key;
    PyObject *val;
    Py_ssize_t idx = 0;

    while (PyDict_Next(value, &idx, &key, &val)) {
        if (!serialize_object_as_string(context, key))
            return 0;

        if (!_encode(context, val)) {
            return 0;
        }
    }

    // Terminate key/value pairs with empty string
    if (!_amf_write_byte(context, EMPTY_STRING_TYPE))
        return 0; 

    return 1;
}

/* Serialize a PyDate. */
static int serialize_date(EncoderContext *context, PyObject *value)
{
    // Check for idx
    int return_value = encode_reference(context, context->object_refs, value, 0);
    if (return_value > -1)
        return return_value;

    return encode_date(context, value);
}

/* Encode a PyDate. */
static int encode_date(EncoderContext *context, PyObject *value)
{
    // Reference header
    if (!_encode_int(context, REFERENCE_BIT))
        return 0;

    // Call python function to get datetime
    if (!amfast_mod) {
        amfast_mod = PyImport_ImportModule("amfast");
        if(!amfast_mod) {
            return 0;
        }
    }

    PyObject *epoch_func = PyObject_GetAttrString(amfast_mod, "epoch_from_date");
    if (!epoch_func)
        return 0;

    PyObject *epoch_time = PyObject_CallFunctionObjArgs(epoch_func, value, NULL);
    Py_DECREF(epoch_func);
    if (!epoch_time)
        return 0;

    double micro_time = PyLong_AsDouble(epoch_time);
    Py_DECREF(epoch_time);

    return _encode_double(context, micro_time);
}

/* 
 * Encode a referenced value.
 *
 * bit argument specifies which bit to is the reference bit (0 = low bit)
 *
 * Returns -1 if reference was not found,
 * 1 if reference was found
 * 0 if error.
 */
static int encode_reference(EncoderContext *context, ObjectContext *object_context, PyObject *value, int bit)
{
    // Using references is an option set in the context
    if (context->use_references != 1) {
        return -1;
    }

    int idx = get_idx_from_ref(object_context, value);
    if (idx > -1) {
       if (idx < MAX_INT) {// Max reference count
           if (!_encode_int(context, (idx << (bit + 1)) | (0x00 + bit)))
               return 0;
           return 1;
       }
    }

    // Object is not indexed, add index
    if (!map_next_object_ref(object_context, value))
        return 0;

    return -1;
}

/*
 * Encode an AMF0 Reference.
 *
 * Returns -1 if reference was not found,
 * 1 if reference was found
 * 0 if error.
 */
static int write_reference_AMF0(EncoderContext *context, PyObject *value)
{
    // Using references is an option set in the context
    if (context->use_references != 1)
        return -1;

    int idx = get_idx_from_ref(context->object_refs, value);
    if (idx > -1) {
        if (idx < MAX_USHORT) {
            if (!_amf_write_byte(context, REF_AMF0))
                return 0;

            if (!_encode_ushort(context, (unsigned short)idx))
                return 0;

            return 1;
        }
    }

    // Object is not indexed, add index
    if (!map_next_object_ref(context->object_refs, value))
        return 0;

    return -1;
}

/* Serializes a PyByteArray or a PyString. */
static int serialize_byte_array(EncoderContext *context, PyObject *value)
{
    // Check for idx
    int return_value = encode_reference(context, context->object_refs, value, 0);
    if (return_value > -1)
        return return_value;

    // Length prefix
    Py_ssize_t value_len;
    if (PyString_CheckExact(value)) {
        value_len = PyString_GET_SIZE(value);
    }
    #ifdef Py_BYTEARRAYOBJECT_H
    // ByteArray encoding is only available in 2.6+
    else if (PyByteArray_Check(value)) {
        value_len = PyByteArray_GET_SIZE(value);
    }
    #endif
    else {
        PyErr_SetString(amfast_EncodeError, "Cannot encode non ByteArray/String as byte array.");
        return 0;
    }

    if (!_encode_int(context, ((int)value_len) << 1 | REFERENCE_BIT))
        return 0;

    return encode_byte_array(context, value);
}

/* Serializes a PyByteArray or a PyString. */
static int encode_byte_array(EncoderContext *context, PyObject *value)
{
    Py_ssize_t value_len;
    char *byte_value;
    
    if (PyString_CheckExact(value)) {
        value_len = PyString_GET_SIZE(value);
        byte_value = PyString_AS_STRING(value);
    }
    #ifdef Py_BYTEARRAYOBJECT_H
    // ByteArray encoding is only available in 2.6+
    else if (PyByteArray_Check(value)) {
        value_len = PyByteArray_GET_SIZE(value);
        byte_value = PyByteArray_AS_STRING(value);
    }
    #endif
    else {
        PyErr_SetString(amfast_EncodeError, "Cannot encode non ByteArray/String as byte array.");
        return 0;
    }
    
    if (!byte_value)
        return 0;

    return _amf_write_string_size(context, byte_value, (size_t)value_len);
}

/* Writes an xml.dom.Document object. */
static int write_xml(EncoderContext *context, PyObject *value)
{
    int byte_marker;

    if (context->use_legacy_xml == 1) {
        printf("LEGACY XML   !!\n");
        byte_marker = XML_DOC_TYPE;
    } else {
        printf("NEW XML !!\n");
        byte_marker = XML_TYPE;
    }

    if (!_amf_write_byte(context, byte_marker))
        return 0;

    return serialize_xml(context, value);
}

/* Serializes an xml.dom.Document object. */
static int serialize_xml(EncoderContext *context, PyObject *value)
{
    // Check for idx
    int return_value = encode_reference(context, context->object_refs, value, 0);
    if (return_value > -1)
        return return_value;

    PyObject *unicode_value = PyObject_CallMethod(value, "toxml", NULL);
    if (!unicode_value)
        return 0;

    return_value = encode_unicode(context, unicode_value);
    Py_DECREF(unicode_value);
    return return_value;
}

/* Returns 1 if a PyObject is a xml.dom.Document object. */
static int check_xml(PyObject *value)
{
    if (!xml_dom_mod) {
        // Import xml.dom
        xml_dom_mod = PyImport_ImportModule("xml.dom.minidom");
        if (!xml_dom_mod)
            return -1;
    }

    PyObject *doc_class = PyObject_GetAttrString(xml_dom_mod, "Document");
    if (!doc_class)
        return -1;
    
    int return_value = PyObject_IsInstance(value, doc_class);
    Py_DECREF(doc_class);
    return return_value;
}

/* Serialize a Python object. */
static int serialize_object(EncoderContext *context, PyObject *value)
{
    // Check for idx
    int return_value = encode_reference(context, context->object_refs, value, 0);
    if (return_value > -1)
        return return_value;

    return encode_object(context, value);
}

/* Encode a Python object. */
static int encode_object(EncoderContext *context, PyObject *value)
{

    // Use object's class to get ClassDef
    PyObject *class_ = PyObject_GetAttrString(value, "__class__");
    if (!class_)
        return 0;

    PyObject *class_def = class_def_from_class(context, value);
    if (!class_def)
        return 0;

    if (class_def == Py_None) {
        // No ClassDef was found, encode as an anonymous object
        Py_DECREF(class_def);

        PyObject *dict = attributes_from_object(context, value);
        if (!dict)
            return 0;

        int return_value = encode_dict(context, dict);
        Py_DECREF(dict);
        return return_value;
    }

    // Class has a ClassDef
    // encode class definition
    if (!serialize_class_def(context, class_def)) {
        Py_DECREF(class_def);
        return 0;
    }

    // Encode externizeable
    if (PyObject_HasAttrString(class_def, "EXTERNIZEABLE_CLASS_DEF")) {
        PyObject *method_name = PyString_FromString("writeByteString");
        if (!method_name) {
            Py_DECREF(class_def);
            return 0;
        }

        PyObject *byte_array = PyObject_CallMethodObjArgs(class_def, method_name, value, NULL);
        Py_DECREF(method_name);
        Py_DECREF(class_def);
        if (!byte_array)
            return 0;

        int return_value = encode_byte_array(context, byte_array);
        Py_DECREF(byte_array);
        return return_value; // We don't need to encode anything else.
    }

    // Encode static attrs
    PyObject *static_attrs = static_attr_vals_from_class_def(class_def, value);
    if (!static_attrs) {
        Py_DECREF(class_def);
        return 0;
    }
    
    Py_ssize_t static_attr_len = PySequence_Size(static_attrs);
    if (static_attr_len == -1) {
        Py_DECREF(class_def);
        Py_DECREF(static_attrs);
        return 0;
    }

    int i;
    for (i = 0; i < static_attr_len; i++) {
        PyObject *static_attr = PySequence_GetItem(static_attrs, i);
        if (!static_attr) {
            Py_DECREF(static_attrs);
            Py_DECREF(class_def);
            return 0;
        }

        int return_value = _encode(context, static_attr);
        Py_DECREF(static_attr);
        if (!return_value) {
            Py_DECREF(static_attrs);
            Py_DECREF(class_def);
            return 0;
        }
    }
    Py_DECREF(static_attrs);

    // Encode dynamic attrs
    if (PyObject_HasAttrString(class_def, "DYNAMIC_CLASS_DEF")) {
        PyObject *dynamic_attrs = dynamic_attrs_from_class_def(class_def, value);
        if (!dynamic_attrs) {
            Py_DECREF(class_def);
            return 0;
        }

        int return_value = _encode_dynamic_dict(context, dynamic_attrs);
        Py_DECREF(dynamic_attrs);
        if (!return_value) {
            Py_DECREF(class_def);
            return 0;
        }
    }

    Py_DECREF(class_def);
    return 1;
}

/* Serialize a class definition. */
static int serialize_class_def(EncoderContext *context, PyObject *value)
{
    // Check for idx
    int return_value = encode_reference(context, context->class_refs, value, 1);
    if (return_value > -1)
        return return_value;

    return encode_class_def(context, value);
}

/* Encode a class definition. */
static int encode_class_def(EncoderContext *context, PyObject *value)
{
    if (value == Py_None) {
        // Encode as anonymous object
        if (!_amf_write_byte(context, DYNAMIC))
            return 0;

        // Anonymous object alias is an empty string
        if (!_amf_write_byte(context, EMPTY_STRING_TYPE))
            return 0;
        return 1;
    }

    // Encode class type
    if (!PyObject_HasAttrString(value, "CLASS_DEF")) {
        PyErr_SetString(amfast_EncodeError, "Invalid class definition object.");
        return 0;
    }

    // Determine header type
    int header;
    if (PyObject_HasAttrString(value, "EXTERNIZEABLE_CLASS_DEF")) {
        header = EXTERNIZEABLE;
    } else if (PyObject_HasAttrString(value, "DYNAMIC_CLASS_DEF")) {
        header = DYNAMIC;
    } else {
        header = STATIC;
    }

    PyObject *class_alias = PyObject_GetAttrString(value, "alias");
    if (!class_alias)
       return 0;

    // Don't need to encode static attrs of externizeable.
    if (header == EXTERNIZEABLE) {
       if (!_encode_int(context, header)) {
           Py_DECREF(class_alias);
           return 0;
       }
       
       int return_value = serialize_object_as_string(context, class_alias);
       Py_DECREF(class_alias);
       return return_value;
    }

    // Encode number of static attrs in header.
    PyObject *static_attrs = PyObject_GetAttrString(value, "static_attrs");
    if (!static_attrs) {
       Py_DECREF(class_alias);
       return 0;
    }

    Py_ssize_t static_attr_len = PySequence_Size(static_attrs);
    if (static_attr_len == -1) {
       Py_DECREF(class_alias);
       Py_DECREF(static_attrs);
       return 0;
    }

    if (static_attr_len > (MAX_INT >> 4)) {
       Py_DECREF(class_alias);
       Py_DECREF(static_attrs);
       PyErr_SetString(amfast_EncodeError, "ClassDef has too many attributes.");
       return 0;
    }
    header |= ((int)static_attr_len) << 4;

    if (!_encode_int(context, header)) {
       Py_DECREF(class_alias);
       Py_DECREF(static_attrs);
       return 0;
    }

    int return_value = serialize_object_as_string(context, class_alias);
    Py_DECREF(class_alias);
    if (!return_value) {
       Py_DECREF(static_attrs);
       return 0;
    }

    // Encode static attr names
    int i;
    for (i = 0; i < static_attr_len; i++) {
        PyObject *attr_name = PySequence_GetItem(static_attrs, i);
        if (!attr_name) {
            Py_DECREF(static_attrs);
            return 0;
        }
        
        int return_value = serialize_object_as_string(context, attr_name);
        Py_DECREF(attr_name);
        if (!return_value)
            return 0;
    }

    Py_DECREF(static_attrs);
    return 1;
}

/* Encode a Python boolean in AMF0. */
static int encode_bool_AMF0(EncoderContext *context, PyObject *value)
{
    int return_value;
    if (value == Py_True) {
        return_value = _amf_write_byte(context, TRUE_AMF0);
    } else {
        return_value = _amf_write_byte(context, FALSE_AMF0);
    }

    return return_value;
}

/* Encode a PyInt in AMF0. */
static int encode_int_AMF0(EncoderContext *context, PyObject *value)
{
    long long_val = PyInt_AsLong(value);
    PyObject *py_long_val = PyLong_FromLong(long_val);
    if (!py_long_val)
        return 0;

    int return_value = encode_long_AMF0(context, py_long_val);
    Py_DECREF(py_long_val);
    return return_value;
}

/* Encode a PyLong in AMF0. */
static int encode_long_AMF0(EncoderContext *context, PyObject *value)
{
    return _encode_double(context, PyLong_AsDouble(value));
}

/* Encode a PyFloat in AMF0. */
static int encode_float_AMF0(EncoderContext *context, PyObject *value)
{
    return _encode_double(context, PyFloat_AsDouble(value));
}

/* Encode a native C unsigned short. */
static int _encode_ushort(EncoderContext *context, unsigned short value)
{
   // Put bytes from short into byte array
   union aligned { // use the same memory for d_value and c_value
       unsigned short d_value;
       char c_value[2];
   } d;
   char *char_value = d.c_value;
   d.d_value = value;

   // AMF numbers are encoded in big endianness
   if (big_endian) {
       return _amf_write_string_size(context, char_value, 2);
   } else {
       // Flip endianness
       char flipped[2] = {char_value[1], char_value[0]};
       return _amf_write_string_size(context, flipped, 2);
   }
}

/* Encode a native C unsigned int. */
static int _encode_ulong(EncoderContext *context, unsigned int value)
{
   // Put bytes from long into byte array
   union aligned { // use the same memory for d_value and c_value
       unsigned int d_value;
       char c_value[4];
   } d;
   char *char_value = d.c_value;
   d.d_value = value;

   // AMF numbers are encoded in big endianness
   if (big_endian) {
       return _amf_write_string_size(context, char_value, 4);
   } else {
       // Flip endianness
       char flipped[4] = {char_value[3], char_value[2], char_value[1], char_value[0]};
       return _amf_write_string_size(context, flipped, 4);
   }
}

/* Write a PyString. */
static int write_string_AMF0(EncoderContext *context, PyObject *value)
{
    PyObject *unicode_value = PyUnicode_FromObject(value);
    if (!unicode_value)
        return 0;

    int return_value = write_unicode_AMF0(context, unicode_value);
    Py_DECREF(unicode_value);
    return return_value;
}

/* Write a PyUnicode in AMF0. */
static int write_unicode_AMF0(EncoderContext *context, PyObject *value)
{
    Py_ssize_t string_len = PyUnicode_GET_SIZE(value);

    if (string_len > 65536) {
        if (!_amf_write_byte(context, LONG_STRING_AMF0))
            return 0;
        return encode_unicode_AMF0(context, value, 1);
    }

    if (!_amf_write_byte(context, STRING_AMF0))
        return 0;

    return encode_unicode_AMF0(context, value, 0);
}

/*
 * Encode a PyUnicode in AMF0.
 *
 * allow_long
 * 1 = allow long values
 * 0 = don't allow long values
 * -1 = force long value
 */
static int encode_unicode_AMF0(EncoderContext *context, PyObject *value, int allow_long)
{
    PyObject *PyString_value = PyUnicode_AsUTF8String(value);
    if (!PyString_value)
        return 0;

    int return_value = encode_string_AMF0(context, PyString_value, allow_long);
    Py_DECREF(PyString_value);
    return return_value;
}

/*
 * Encode a PyString in AMF0.
 *
 * allow_long
 * 1 = allow long values
 * 0 = don't allow long values
 * -1 = force long value
 */
static int encode_string_AMF0(EncoderContext *context, PyObject *value, int allow_long)
{
    char *char_value = PyString_AS_STRING(value);
    Py_ssize_t string_len = PyString_GET_SIZE(value);
    if (!char_value)
        return 0;

    if (string_len > 65536 && allow_long == 0) {
        PyErr_SetString(amfast_EncodeError, "Long string not allowed.");
        return 0;
    } else if (string_len > 65536 || allow_long == -1) {
        if (!_encode_ulong(context, (unsigned int)string_len))
            return 0;
    } else {
        if (!_encode_ushort(context, (unsigned short)string_len))
            return 0;
    }

    return _amf_write_string_size(context, char_value, (size_t)string_len);
}

/* Write a PyList to AMF0. */
static int write_list_AMF0(EncoderContext *context, PyObject *value, int write_reference)
{
    if (write_reference) {
        int return_value = write_reference_AMF0(context, value);
        if (return_value == 0 || return_value == 1) {
            return return_value;
        }
    }

    // No reference
    if (!_amf_write_byte(context, ARRAY_AMF0))
        return 0;

    Py_ssize_t array_len = PySequence_Size(value);
    if (array_len < 0)
        return 0;

    if (!_encode_ulong(context, (unsigned int)array_len))
        return 0;

    int i;
    for (i = 0; i < array_len; i++) {
        // Increment ref count in case 
        // list is modified by someone else.
        PyObject *list_item = PySequence_GetItem(value, (Py_ssize_t)i);
        if (!list_item)
            return 0;

        int return_value = _encode_AMF0(context, list_item);
        Py_DECREF(list_item);
        if (!return_value)
            return 0;
    }

    return 1;
}

/* Write a PyDict to AMF0. */
static int write_dict_AMF0(EncoderContext *context, PyObject *value)
{
    int return_value = write_reference_AMF0(context, value);
    if (return_value == 0 || return_value == 1) {
        return return_value;
    }

    // No reference
    if (!_amf_write_byte(context, OBJECT_AMF0))
        return 0;

    return _encode_dynamic_dict_AMF0(context, value);
}

/* Write the contents of a Dict to AMF0. */
static int _encode_dynamic_dict_AMF0(EncoderContext *context, PyObject *value)
{
    PyObject *key;
    PyObject *val;
    Py_ssize_t idx = 0;

    while (PyDict_Next(value, &idx, &key, &val)) {
        if (!encode_object_as_string_AMF0(context, key, 0))
            return 0;

        if (!_encode_AMF0(context, val)) {
            return 0;
        }
    }

    // Terminate key/value pairs.
    char terminator[3] = {0x00, 0x00, 0x09};
    if (!_amf_write_string_size(context, terminator, 3))
        return 0;

    return 1;
}

/* Encode a Date object to AMF0 .*/
static int write_date_AMF0(EncoderContext *context, PyObject *value)
{
    // Dates can use references
    /*int wrote_ref = write_reference_AMF0(context, value);
    if (wrote_ref == 0 || wrote_ref == 1) {
        return wrote_ref;
    }*/

    if (!_amf_write_byte(context, DATE_AMF0))
        return 0;
 
    // Call python function to get datetime
    if (!amfast_mod) {
        amfast_mod = PyImport_ImportModule("amfast");
        if(!amfast_mod) {
            return 0;
        }
    }

    PyObject *epoch_func = PyObject_GetAttrString(amfast_mod, "epoch_from_date");
    if (!epoch_func)
        return 0;

    PyObject *epoch_time = PyObject_CallFunctionObjArgs(epoch_func, value, NULL);
    Py_DECREF(epoch_func);
    if (!epoch_time)
        return 0;

    double micro_time = PyLong_AsDouble(epoch_time);
    Py_DECREF(epoch_time);

    if (!_encode_double(context, micro_time))
        return 0;
    
    // UTC time zone
    return _encode_ushort(context, 0);
}

/* Write an XML object in AMF0. */
static int write_xml_AMF0(EncoderContext *context, PyObject *value)
{
    if (!_amf_write_byte(context, XML_DOC_AMF0))
        return 0;

    PyObject *unicode_value = PyObject_CallMethod(value, "toxml", NULL);
    if (!unicode_value)
        return 0;

    int return_value = encode_unicode_AMF0(context, unicode_value, -1);
    Py_DECREF(unicode_value);
    return return_value;
}

/* Encode a Python object that doesn't have a C API in AMF0. */
static int _encode_object_AMF0(EncoderContext *context, PyObject *value)
{
    // Check for special object types
    int xml_value = check_xml(value);
    if (xml_value == -1) {
        return 0;
    } else if (xml_value == 1) {
        return write_xml_AMF0(context, value);
    }

    // Generic object
    return write_object_AMF0(context, value);
}

/* Get an object's class def. */
static PyObject* class_def_from_class(EncoderContext *context, PyObject *value)
{
    // Use object's class to get ClassDef
    PyObject *class_ = PyObject_GetAttrString(value, "__class__");
    if (!class_)
        return NULL;

    // Create method name, if it does not exist already.
    if (!context->get_class_def_method_name) {
        context->get_class_def_method_name = PyString_FromString("getClassDefByClass");
        if (!context->get_class_def_method_name) {
            Py_DECREF(class_);
            return NULL;
        }
    }

    PyObject *class_def = PyObject_CallMethodObjArgs(context->class_def_mapper, context->get_class_def_method_name,
        class_, NULL);
    Py_DECREF(class_);
    return class_def;
}

/* Write an anonymous object in AMF0. */
static int write_anonymous_object_AMF0(EncoderContext *context, PyObject *value)
{
    if (!_amf_write_byte(context, OBJECT_AMF0))
        return 0;

    PyObject *dict = attributes_from_object(context, value);
    if (!dict)
        return 0;

    int return_value = _encode_dynamic_dict_AMF0(context, dict);
    Py_DECREF(dict);
    return return_value;
}

/* Get attributes from an anonymous object as a dict. */
static PyObject* attributes_from_object(EncoderContext *context, PyObject *value)
{
    if (!class_def_mod) {
        class_def_mod = PyImport_ImportModule("amfast.class_def");
        if(!class_def_mod) {
            return NULL;
        }
    }

    PyObject *attr_func = PyObject_GetAttrString(class_def_mod, "get_dynamic_attr_vals");
    if (!attr_func)
        return NULL;

    PyObject *dict = PyObject_CallFunctionObjArgs(attr_func, value, Py_None, context->include_private, NULL);
    Py_DECREF(attr_func);
    if (!dict)
        return NULL;

    return dict;
}

/* Encode a ClassDef in AMF0. */
static int encode_class_def_AMF0(EncoderContext *context, PyObject *value)
{
    PyObject *alias = PyObject_GetAttrString(value, "alias");
    if (!alias)
        return 0;

    int return_value = encode_object_as_string_AMF0(context, alias, 0);
    Py_DECREF(alias);
    return return_value;
}

/* Get static attrs. */
static PyObject* static_attr_vals_from_class_def(PyObject *class_def, PyObject *value)
{
    PyObject *method_name = PyString_FromString("getStaticAttrVals");
    if (!method_name)
        return NULL;

    PyObject *static_attrs = PyObject_CallMethodObjArgs(class_def, method_name, value, NULL);
    Py_DECREF(method_name);
    if (!static_attrs)
        return NULL;

    if (!PyList_Check(static_attrs)) {
        PyErr_SetString(amfast_EncodeError, "ClassDef.getStaticAttrVals must return a list.");
        Py_DECREF(static_attrs);
        return NULL;
    }

    return static_attrs;
}

/* Get dynamic attrs. */
static PyObject* dynamic_attrs_from_class_def(PyObject *class_def, PyObject *value)
{
    PyObject *method_name = PyString_FromString("getDynamicAttrVals");
    if (!method_name)
        return NULL;

    PyObject *dynamic_attrs = PyObject_CallMethodObjArgs(class_def, method_name, value, NULL);
    Py_DECREF(method_name);
    if (!dynamic_attrs)
        return NULL;

    if (!PyDict_Check(dynamic_attrs)) {
        PyErr_SetString(amfast_EncodeError, "ClassDef.getDynamicAttrVals must return a dict.");
        Py_DECREF(dynamic_attrs);
        return NULL;
    }

    return dynamic_attrs;
}

/* Write a Python object in AMF0. */
static int write_object_AMF0(EncoderContext *context, PyObject *value)
{
    int wrote_ref = write_reference_AMF0(context, value);
    if (wrote_ref == 0 || wrote_ref == 1) {
        return wrote_ref;
    }

    PyObject *class_def = class_def_from_class(context, value);
    if (!class_def)
        return 0;

    if (class_def == Py_None) {
        Py_DECREF(class_def);
        return write_anonymous_object_AMF0(context, value);
    }

    // Check for AMF3 object
    PyObject *amf3 = PyObject_GetAttrString(class_def, "amf3");
    if (amf3 == Py_True) {
        // Encode this object in AMF3
        Py_DECREF(amf3);
        Py_DECREF(class_def);
        if (!_amf_write_byte(context, AMF3_AMF0))
            return 0;

        // Create new context for AMF3 encoder
        EncoderContext *new_context = _copy_encoder_context(context, 1);
        int return_value = _encode(new_context, value);
        if (!return_value) {
            // AMF3 encode failed
            _destroy_encoder_context(new_context);
            return 0;
        }

        // Set context values = new context values
        return_value = _amf_write_string_size(context, new_context->buf, new_context->buf_len);
        if (!_destroy_encoder_context(new_context))
            return 0;

        return return_value;
    } else {
        Py_DECREF(amf3);
    }

    // Write marker byte
    if (!_amf_write_byte(context, TYPED_OBJ_AMF0)) {
        Py_DECREF(class_def);
        return 0;
    }

    // Class has a ClassDef
    // encode class definition
    if (!encode_class_def_AMF0(context, class_def)) {
        Py_DECREF(class_def);
        return 0;
    }

    // Get all attributes to encode
    PyObject *attrs = PyDict_New();
    if (!attrs) {
        Py_DECREF(class_def);
        return 0;
    }

    // Get static attrs
    PyObject *static_attrs = static_attr_vals_from_class_def(class_def, value);
    if (!static_attrs) {
        Py_DECREF(class_def);
        Py_DECREF(attrs);
        return 0;
    }

    // Get static attr names
    PyObject *static_attr_names = PyObject_GetAttrString(class_def, "static_attrs");
    if (!static_attr_names) {
        Py_DECREF(class_def);
        Py_DECREF(attrs);
        Py_DECREF(static_attrs);
        return 0;
    }

    Py_ssize_t static_attr_len = PySequence_Size(static_attrs);
    if (static_attr_len == -1) {
        Py_DECREF(class_def);
        Py_DECREF(attrs);
        Py_DECREF(static_attrs);
        return 0;
    }

    int i;
    for (i = 0; i < static_attr_len; i++) {
        PyObject *static_attr_name = PySequence_GetItem(static_attr_names, i);
        if (!static_attr_name) {
            Py_DECREF(class_def);
            Py_DECREF(attrs);
            Py_DECREF(static_attrs);
            return 0;
        }

        PyObject *static_attr = PySequence_GetItem(static_attrs, i);
        if (!static_attr) {
            Py_DECREF(class_def);
            Py_DECREF(attrs);
            Py_DECREF(static_attrs);
            Py_DECREF(static_attr_name);
            return 0;
        }

        int return_value = PyDict_SetItem(attrs, static_attr_name, static_attr);
        Py_DECREF(static_attr_name);
        Py_DECREF(static_attr);
        if (return_value == -1) {
            Py_DECREF(class_def);
            Py_DECREF(attrs);
            Py_DECREF(static_attrs);
            return 0;
        }
    }
    Py_DECREF(static_attrs);
    Py_DECREF(static_attr_names);

    // Get dynamic attrs
    if (PyObject_HasAttrString(class_def, "DYNAMIC_CLASS_DEF")) {
        PyObject *dynamic_attrs = dynamic_attrs_from_class_def(class_def, value);
        if (!dynamic_attrs) {
            Py_DECREF(class_def);
            Py_DECREF(attrs);
            return 0;
        }

        int return_value = PyDict_Merge(attrs, dynamic_attrs, 0);
        Py_DECREF(dynamic_attrs);
        if (return_value == -1) {
            Py_DECREF(class_def);
            Py_DECREF(attrs);
            return 0;
        }
    }
    Py_DECREF(class_def);

    int return_value = _encode_dynamic_dict_AMF0(context, attrs);
    Py_DECREF(attrs);
    return return_value;
}

/* Encode an AMF packet. */
static int _encode_packet(EncoderContext *context, PyObject *value)
{
    // write flash version
    PyObject *client_type = PyObject_GetAttrString(value, "version");
    if (!client_type)
        return 0;

    PyObject *flash_8 = PyObject_GetAttrString(value, "FLASH_8");
    if (!flash_8) {
        Py_DECREF(client_type);
        return 0;
    }

    PyObject *flash_com = PyObject_GetAttrString(value, "FLASH_COM");
    if (!flash_com) {
        Py_DECREF(client_type);
        Py_DECREF(flash_8);
        return 0;
    }

    PyObject *flash_9 = PyObject_GetAttrString(value, "FLASH_9");
    if (!flash_9) {
        Py_DECREF(client_type);
        Py_DECREF(flash_8);
        Py_DECREF(flash_com);
        return 0;
    }

    // Set client type
    unsigned short amf_version;
    if (PyUnicode_Compare(client_type, flash_8) == 0) {
        amf_version = FLASH_8;
    } else if (PyUnicode_Compare(client_type, flash_com) == 0) {
        amf_version = FLASH_COM;
    } else if (PyUnicode_Compare(client_type, flash_9) == 0) {
        amf_version = FLASH_9;
    } else {
        PyErr_SetString(amfast_EncodeError, "Unknown client type.");
        Py_DECREF(client_type);
        Py_DECREF(flash_8);
        Py_DECREF(flash_com);
        Py_DECREF(flash_9);
        return 0;
    }

    Py_DECREF(client_type);
    Py_DECREF(flash_8);
    Py_DECREF(flash_com);
    Py_DECREF(flash_9);
    if (!_encode_ushort(context, amf_version))
        return 0;

    // write headers
    PyObject *headers = PyObject_GetAttrString(value, "headers");
    if (!headers)
        return 0;

    Py_ssize_t header_count = PySequence_Size(headers);
    if (header_count == -1) {
        Py_DECREF(headers);
        return 0;
    }
    if (!_encode_ushort(context, (unsigned short)header_count)) {
        Py_DECREF(headers);
        return 0;
    }

    int i;
    for (i = 0; i < header_count; i++) {
        PyObject *header = PySequence_GetItem(headers, i);
        if (!header) {
            Py_DECREF(headers);
            return 0;
        }

        int wrote_header = encode_packet_header_AMF0(context, header);
        Py_DECREF(header);
        if (!wrote_header) {
            Py_DECREF(headers);
            return 0;
        }
    }
    Py_DECREF(headers);

    // write messages
    PyObject *messages = PyObject_GetAttrString(value, "messages");
    if (!messages)
        return 0;

    Py_ssize_t message_count = PySequence_Size(messages);
    if (message_count == -1) {
        Py_DECREF(messages);
        return 0;
    }
    if (!_encode_ushort(context, (unsigned short)message_count)) {
        Py_DECREF(messages);
        return 0;
    }

    for (i = 0; i < message_count; i++) {
        PyObject *message = PySequence_GetItem(messages, i);
        if (!message) {
            Py_DECREF(messages);
            return 0;
        }

        int wrote_message = encode_packet_message_AMF0(context, message);
        Py_DECREF(message);
        if (!wrote_message) {
            Py_DECREF(messages);
            return 0;
        }
    }
    Py_DECREF(messages);

    return 1;
}

/* Encode a Packet header in AMF0. */
static int encode_packet_header_AMF0(EncoderContext *context, PyObject *value)
{
    PyObject *header_name = PyObject_GetAttrString(value, "name");
    if (!header_name)
        return 0;

    int return_value = encode_object_as_string_AMF0(context, header_name, 0);
    Py_DECREF(header_name);
    if (!return_value)
        return 0;

    PyObject *required = PyObject_GetAttrString(value, "required");
    if (!required)
        return 0;

    return_value = encode_bool_AMF0(context, required);
    Py_DECREF(required);
    if (!return_value)
        return 0;

    PyObject *body = PyObject_GetAttrString(value, "value");
    if (!body)
        return 0;

    // Encode header value with a new context, so references are reset
    EncoderContext *new_context = _copy_encoder_context(context, 0);
    return_value = _encode_AMF0(new_context, body);
    Py_DECREF(body);
    if (!return_value) {
        _destroy_encoder_context(new_context);
        return 0;
    }

    if (!_encode_ulong(context, (unsigned int)new_context->buf_len)) {
        _destroy_encoder_context(new_context);
        return 0;
    }

    return_value = _amf_write_string_size(context, new_context->buf, new_context->buf_len);
    _destroy_encoder_context(new_context);
    return return_value;
}

/* Encode a Packet message in AMF0. */
static int encode_packet_message_AMF0(EncoderContext *context, PyObject *value)
{
    PyObject *target = PyObject_GetAttrString(value, "target");
    if (!target)
        return 0;

    int return_value = encode_object_as_string_AMF0(context, target, 0);
    Py_DECREF(target);
    if (!return_value)
        return 0;

    PyObject *response = PyObject_GetAttrString(value, "response");
    if (!response)
        return 0;

    return_value = encode_object_as_string_AMF0(context, response, 0);
    if (!return_value) {
        Py_DECREF(response);
        return 0;
    }

    PyObject *body = PyObject_GetAttrString(value, "value");
    if (!body) {
        Py_DECREF(response);
        return 0;
    }

    // Encode message body with a new context, so references are reset
    EncoderContext *new_context = _copy_encoder_context(context, context->use_amf3);

    if (PySequence_Size(response) > 0 && (PyList_Check(body) || PyTuple_Check(body))) {
        // We're encoding a request,
        // Don't count argument list 
        // in reference count.
        return_value = write_list_AMF0(new_context, body, 0);
    } else {
        if (context->use_amf3) {
            if (!_amf_write_byte(new_context, AMF3_AMF0))
                return 0;
            return_value = _encode(new_context, body);
        } else {
            return_value = _encode_AMF0(new_context, body);
        }
    }
    Py_DECREF(response);
    Py_DECREF(body);
    if (!return_value) {
        _destroy_encoder_context(new_context);
        return 0;
    }

    if (!_encode_ulong(context, (unsigned int)new_context->buf_len)) {
        _destroy_encoder_context(new_context);
        return 0;
    }

    return_value = _amf_write_string_size(context, new_context->buf, new_context->buf_len);
    _destroy_encoder_context(new_context);
    return return_value;
}

/* Encode a Python object that doesn't have a C API in AMF3. */
static int _encode_object(EncoderContext *context, PyObject *value)
{
    // Check for special object types
    int xml_value = check_xml(value);
    if (xml_value == -1) {
        return 0;
    } else if (xml_value == 1) {
        return write_xml(context, value);
    }

    // Generic object
    if (!_amf_write_byte(context, OBJECT_TYPE))
            return 0;

    return serialize_object(context, value);
}

/* Encoding function map for AMF0. */
static int _encode_AMF0(EncoderContext *context, PyObject *value)
{
    // Determine object type
    if (value == Py_None) {
        return _amf_write_byte(context, NULL_AMF0);
    } else if (PyBool_Check(value)) {
        if (!_amf_write_byte(context, BOOL_AMF0))
            return 0;
        return encode_bool_AMF0(context, value);
    } else if (PyInt_Check(value)) {
        if (!_amf_write_byte(context, NUMBER_AMF0))
            return 0;
        return encode_int_AMF0(context, value);
    } else if (PyLong_Check(value)) {
        if (!_amf_write_byte(context, NUMBER_AMF0))
            return 0;
        return encode_long_AMF0(context, value);
    } else if (PyFloat_Check(value)) {
        if (!_amf_write_byte(context, NUMBER_AMF0))
            return 0;
        return encode_float_AMF0(context, value);
    } else if (PyString_Check(value)) {
        return write_string_AMF0(context, value);
    } else if (PyUnicode_Check(value)) {
        return write_unicode_AMF0(context, value);
    } else if (PyList_Check(value) || PyTuple_Check(value)) {
        return write_list_AMF0(context, value, 1);
    } else if (PyDict_Check(value)) {
        return write_dict_AMF0(context, value);
    } else if (PyDateTime_Check(value) || PyDate_Check(value)) {
       return write_date_AMF0(context, value);
    } else {
        return _encode_object_AMF0(context, value);
    }
}

/* Encoding function map. */
static int _encode(EncoderContext *context, PyObject *value)
{
    // Determine object type
    if (value == Py_None) {
        return encode_none(context);
    } else if (PyBool_Check(value)) {
        return encode_bool(context, value);
    } else if (PyInt_Check(value)) {
        return write_int(context, value);
    } else if (PyString_Check(value)) {
        if (!_amf_write_byte(context, STRING_TYPE))
            return 0;
        return serialize_string(context, value);
    } else if (PyUnicode_Check(value)) {
        if (!_amf_write_byte(context, STRING_TYPE))
            return 0;
        return serialize_unicode(context, value);
    } else if (PyFloat_Check(value)) {
        if (!_amf_write_byte(context, DOUBLE_TYPE))
            return 0;
        return encode_float(context, value);
    } else if (PyLong_Check(value)) {
        if (!_amf_write_byte(context, DOUBLE_TYPE))
            return 0;
        return encode_long(context, value);
    } else if (PyList_Check(value) || PyTuple_Check(value)) {
        return write_list(context, value);
    } else if (PyDict_Check(value)) {
        if (!_amf_write_byte(context, OBJECT_TYPE))
            return 0;
        return serialize_dict(context, value);
    } else if (PyDateTime_Check(value) || PyDate_Check(value)) {
        if (!_amf_write_byte(context, DATE_TYPE))
            return 0;
        return serialize_date(context, value);
    }

    #ifdef Py_BYTEARRAYOBJECT_H
    // ByteArray encoding is only available in 2.6+
    else if (PyByteArray_Check(value)) {
        if (!_amf_write_byte(context, BYTE_ARRAY_TYPE))
            return 0;
        return serialize_byte_array(context, value);
    } 
    #endif

    else {
        return _encode_object(context, value);
    }
}

/* Encode a Python object in AMF. */
static PyObject* encode(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *value;
    PyObject *class_def_mapper = Py_None;
    Py_INCREF(class_def_mapper);
    PyObject *include_private = Py_False;
    Py_INCREF(include_private);
    int use_array_collections = 0;
    int use_object_proxies = 0;
    int use_references = 1;
    int use_legacy_xml = 0;
    int amf3 = 0;
    int packet = 0;

    static char *kwlist[] = {"value", "use_array_collections", "use_object_proxies",
        "use_references", "use_legacy_xml", "amf3", "packet", "include_private",
        "class_def_mapper", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|iiiiiiOO", kwlist,
        &value, &use_array_collections, &use_object_proxies, &use_references,
        &use_legacy_xml, &amf3, &packet, &include_private, &class_def_mapper))
        return NULL;

    EncoderContext *context = _create_encoder_context(1024, ((!packet) && amf3));
    if (!context) {
        Py_DECREF(class_def_mapper);
        Py_DECREF(include_private);
        return NULL;
    }

    // Set defaults
    context->use_array_collections = use_array_collections;
    context->use_object_proxies = use_object_proxies;
    context->use_references = use_references;
    context->use_legacy_xml = use_legacy_xml;
    context->use_amf3 = amf3;

    if (include_private != Py_None) {
        context->include_private = include_private;
        Py_INCREF(context->include_private);
        Py_DECREF(Py_False);
    }

    if (class_def_mapper != Py_None) {
        // Use user supplied ClassDefMapper.
        context->class_def_mapper = class_def_mapper;
        Py_INCREF(context->class_def_mapper);
        Py_DECREF(Py_None);
    } else {
        // Create anonymous ClassDefMapper
        Py_DECREF(Py_None);
        if (!class_def_mod) {
            class_def_mod = PyImport_ImportModule("amfast.class_def");
            if(!class_def_mod) {
                _destroy_encoder_context(context);
                return NULL;
            }
        }

        PyObject *class_def = PyObject_GetAttrString(class_def_mod, "ClassDefMapper");
        if (!class_def) {
            _destroy_encoder_context(context);
            return NULL;
        }

        context->class_def_mapper = PyObject_CallFunctionObjArgs(class_def, NULL);
        Py_DECREF(class_def);
        if (!context->class_def_mapper) {
            _destroy_encoder_context(context);
            return NULL;
        }
    }

    int return_value;
    if (packet) {
        return_value = _encode_packet(context, value);
    } else if (context->use_amf3) {
        return_value = _encode(context, value); 
    } else {
        return_value = _encode_AMF0(context, value);
    }

    if (!return_value) {
        _destroy_encoder_context(context);
        return NULL;
    }

    PyObject *return_obj = PyString_FromStringAndSize(context->buf, (Py_ssize_t)context->buf_len);
    _destroy_encoder_context(context);
    return return_obj;
}

/* Expose functions as Python module functions. */
static PyMethodDef encoder_methods[] = {
    {"encode", (PyCFunction)encode, METH_VARARGS | METH_KEYWORDS,
    "Description:\n"
    "=============\n"
    "Encode a Python object in AMF format.\n\n"
    "Useage:\n"
    "===========\n"
    "byte_string = encode(value, **kwargs)\n\n"
    "Optional keyword arguments:\n"
    "============================\n"
    " * amf3 - bool - True to encode as AMF3 format. - Default = False\n"
    " * packet - bool - True to encode a AMF packet. - Default = False\n"
    " * use_array_collections - bool - True to encode lists and tuples as ArrayCollections - Default = False\n"
    " * use_object_proxies - bool - True to encode dicts as ObjectProxys - Default = False\n"
    " * use_references - bool - True to encode multiply occuring objects as references - Default = True\n"
    " * use_legacy_xml - bool - True to encode XML as old XMLDocument instead of E4X - Default = False\n"
    " * include_private - bool - True to encode attributes starting with '_'  - Default = False\n"
    " * class_def_mapper - ClassDefMapper - object that retrieves a ClassDef object used for customizing \n"
    "    serialization of objects - Default = None (all objects are anonymous)\n"},
    {NULL, NULL, 0, NULL}   /* sentinel */
};

PyMODINIT_FUNC
initencoder(void)
{
    PyObject *module;

    module = Py_InitModule("encoder", encoder_methods);
    if (module == NULL)
        return;

    // Setup exceptions
    if (!amfast_mod) {
        amfast_mod = PyImport_ImportModule("amfast");
        if(!amfast_mod) {
            return;
        }
    }

    amfast_Error = PyObject_GetAttrString(amfast_mod, "AmFastError");
    if (amfast_Error == NULL) {
        return;
    }

    amfast_EncodeError = PyErr_NewException("amfast.encoder.EncodeError", amfast_Error, NULL);
    if (amfast_EncodeError == NULL) {
        return;
    }

    Py_INCREF(amfast_EncodeError);
    if (PyModule_AddObject(module, "EncodeError", amfast_EncodeError) == -1) {
        return;
    }

    // Determine endianness of architecture
    if (is_bigendian()) {
        big_endian = 1;
    } else {
        big_endian = 0;
    }

    // Setup date time API
    PyDateTime_IMPORT;
}
