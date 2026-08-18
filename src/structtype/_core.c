#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <float.h>
#include <stdatomic.h>

#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "datetime.h"
#include "structmember.h"

#include "common.h"
#include "itoa.h"
#include "ryu.h"
#include "atof.h"

/* Python version checks */
#define PY311_PLUS (PY_VERSION_HEX >= 0x030b0000)
#define PY312_PLUS (PY_VERSION_HEX >= 0x030c0000)
#define PY313_PLUS (PY_VERSION_HEX >= 0x030d0000)
#define PY314_PLUS (PY_VERSION_HEX >= 0x030e0000)
#define PY315_PLUS (PY_VERSION_HEX >= 0x030f0000)

/* In Python 3.14+, tuples cache their hash value in ob_hash. When a tuple is
 * allocated via tp_alloc (which zero-initializes memory), ob_hash is 0 — a
 * valid cached hash — so tuple_hash() returns 0 without computing the real
 * hash. Reset it to -1 ("not yet computed") after allocation. */
#if PY314_PLUS
#define MS_TUPLE_RESET_HASH(op) (((PyTupleObject *)(op))->ob_hash = -1)
#else
#define MS_TUPLE_RESET_HASH(op) ((void)0)
#endif

/* Hint to the compiler not to store `x` in a register since it is likely to
 * change. Results in much higher performance on GCC, with smaller benefits on
 * clang */
#if defined(__GNUC__)
    #define OPT_FORCE_RELOAD(x) __asm volatile("":"=m"(x)::);
#else
    #define OPT_FORCE_RELOAD(x)
#endif

#ifdef __GNUC__
#define ms_popcount(i) __builtin_popcountll(i)
#else
static int
ms_popcount(uint64_t i) {                            \
    i = i - ((i >> 1) & 0x5555555555555555);  // pairs
    i = (i & 0x3333333333333333) + ((i >> 2) & 0x3333333333333333);  // quads
    i = (i + (i >> 4)) & 0x0F0F0F0F0F0F0F0F;  // groups of 8
    return (uint64_t)(i * 0x0101010101010101) >> 56;  // sum bytes
}
#endif

/* In Python 3.12+, tp_dict is NULL for some core types, PyType_GetDict returns
 * a borrowed reference to the interpreter or cls mapping */
#if PY312_PLUS
#define MS_GET_TYPE_DICT(a) PyType_GetDict(a)
#else
#define MS_GET_TYPE_DICT(a) ((a)->tp_dict)
#endif

#if PY313_PLUS
#define MS_UNICODE_EQ(a, b) (PyUnicode_Compare(a, b) == 0)
#else
#define MS_UNICODE_EQ(a, b) _PyUnicode_EQ(a, b)
#endif

#if defined(Py_GIL_DISABLED) && !PY314_PLUS
#error "Py_GIL_DISABLED is only supported in Python 3.14+"
#endif

#if PY314_PLUS
#ifdef Py_GIL_DISABLED
#define _PyObject_HEAD_INIT(type)    \
    {                               \
        0,                          \
        _Py_STATICALLY_ALLOCATED_FLAG, \
        { 0 },                      \
        0,                          \
        _Py_IMMORTAL_REFCNT_LOCAL,  \
        0,                          \
        (type),                     \
    }
#else
#if SIZEOF_VOID_P > 4
#define _PyObject_HEAD_INIT(type)         \
    {                                     \
        .ob_refcnt = _Py_IMMORTAL_INITIAL_REFCNT,  \
        .ob_flags = _Py_STATIC_FLAG_BITS, \
        .ob_type = (type)                 \
    }
#else
#define _PyObject_HEAD_INIT(type)         \
    {                                     \
        .ob_refcnt = _Py_STATIC_IMMORTAL_INITIAL_REFCNT, \
        .ob_type = (type)                 \
    }
#endif // SIZEOF_VOID_P > 4
#endif // Py_GIL_DISABLED
#else
#ifndef _Py_IMMORTAL_REFCNT
#define _Py_IMMORTAL_REFCNT 999999999
#endif
#define _PyObject_HEAD_INIT(type)         \
    {                                     \
        .ob_refcnt = _Py_IMMORTAL_REFCNT, \
        .ob_type = (type)                 \
    }
#endif // PY314_PLUS

#if PY_VERSION_HEX < 0x030D00A1
static inline int
PyDict_GetItemRef(PyObject *mp, PyObject *key, PyObject **result)
{
#if PY_VERSION_HEX >= 0x03000000
    PyObject *item = PyDict_GetItemWithError(mp, key);
#else
    PyObject *item = _PyDict_GetItemWithError(mp, key);
#endif
    if (item != NULL) {
        Py_INCREF(item);
        *result = item;
        return 1;  // found
    }
    if (!PyErr_Occurred()) {
        *result = NULL;
        return 0;  // not found
    }
    *result = NULL;
    return -1;
}
#endif // PY_VERSION_HEX < 0x030D00A1

#if PY315_PLUS
static inline PyObject *
_PyFrozenDict_NewSteal(PyObject *dict) {
    if (dict == NULL) {
        return NULL;
    }
    PyObject *op = PyFrozenDict_New(dict);
    Py_DECREF(dict);
    return op;
}
#endif

#if PY_VERSION_HEX < 0x030D00B3
#  define Py_BEGIN_CRITICAL_SECTION(op) {
#  define Py_END_CRITICAL_SECTION() }
#  define Py_BEGIN_CRITICAL_SECTION2(a, b) {
#  define Py_END_CRITICAL_SECTION2() }
#endif // PY_VERSION_HEX < 0x030D00B3

#define DIV_ROUND_CLOSEST(n, d) (((int64_t)(n) < 0) ? (((n) - (d)/2)/(d)) : (((n) + (d)/2)/(d)))

/* These macros are used to manually unroll some loops */
#define repeat8(x) { x(0) x(1) x(2) x(3) x(4) x(5) x(6) x(7) }

#define is_digit(c) (c >= '0' && c <= '9')

/* Easy access to NoneType object */
#define NONE_TYPE ((PyObject *)(Py_TYPE(Py_None)))

/* Capacity of a list */
#define LIST_CAPACITY(x) (((PyListObject *)x)->allocated)

/* Get the raw items pointer for a list and tuple */
#define LIST_ITEMS(x) (((PyListObject *)(x))->ob_item)
#define TUPLE_ITEMS(x) (((PyTupleObject *)(x))->ob_item)

/* Fast shrink of bytes & bytearray objects. This doesn't do any memory
 * allocations, it just shrinks the size of the view presented to Python. Since
 * outputs of `encode` should be short lived (immediately written to a
 * socket/file then dropped), this shouldn't result in increased application
 * memory usage. */
# define FAST_BYTES_SHRINK(obj, size) \
    do { \
    Py_SET_SIZE(obj, size); \
    PyBytes_AS_STRING(obj)[size] = '\0'; \
    } while (0);
# define FAST_BYTEARRAY_SHRINK(obj, size) \
    do { \
    Py_SET_SIZE(obj, size); \
    PyByteArray_AS_STRING(obj)[size] = '\0'; \
    } while (0);

/* XXX: Optimized `PyUnicode_AsUTF8AndSize` for strs that we know have
 * a cached unicode representation. */
static inline const char *
unicode_str_and_size_nocheck(PyObject *str, Py_ssize_t *size) {
    if (MS_LIKELY(PyUnicode_IS_COMPACT_ASCII(str))) {
        *size = ((PyASCIIObject *)str)->length;
        return (char *)(((PyASCIIObject *)str) + 1);
    }
    *size = ((PyCompactUnicodeObject *)str)->utf8_length;
    return ((PyCompactUnicodeObject *)str)->utf8;
}

/* XXX: Optimized `PyUnicode_AsUTF8AndSize` */
static inline const char *
unicode_str_and_size(PyObject *str, Py_ssize_t *size) {
    const char *out = unicode_str_and_size_nocheck(str, size);
    if (MS_LIKELY(out != NULL)) return out;
    return PyUnicode_AsUTF8AndSize(str, size);
}

static MS_INLINE char *
ascii_get_buffer(PyObject *str) {
    return (char *)(((PyASCIIObject *)str) + 1);
}

/* Fill in view.buf & view.len from either a Unicode or buffer-compatible
 * object. */
static int
ms_get_buffer(PyObject *obj, Py_buffer *view) {
    if (MS_UNLIKELY(PyUnicode_CheckExact(obj))) {
        view->buf = (void *)unicode_str_and_size(obj, &(view->len));
        if (view->buf == NULL) return -1;
        Py_INCREF(obj);
        view->obj = obj;
        return 0;
    }
    return PyObject_GetBuffer(obj, view, PyBUF_CONTIG_RO);
}

static void
ms_release_buffer(Py_buffer *view) {
    if (MS_LIKELY(!PyUnicode_CheckExact(view->obj))) {
        PyBuffer_Release(view);
    }
    else {
        Py_CLEAR(view->obj);
    }
}

/* Hash algorithm borrowed from cpython 3.10's hashing algorithm for tuples.
 * See https://github.com/python/cpython/blob/4bcef2bb48b3fd82011a89c1c716421b789f1442/Objects/tupleobject.c#L386-L424
 */
#if SIZEOF_PY_UHASH_T > 4
#define MS_HASH_XXPRIME_1 ((Py_uhash_t)11400714785074694791ULL)
#define MS_HASH_XXPRIME_2 ((Py_uhash_t)14029467366897019727ULL)
#define MS_HASH_XXPRIME_5 ((Py_uhash_t)2870177450012600261ULL)
#define MS_HASH_XXROTATE(x) ((x << 31) | (x >> 33))  /* Rotate left 31 bits */
#else
#define MS_HASH_XXPRIME_1 ((Py_uhash_t)2654435761UL)
#define MS_HASH_XXPRIME_2 ((Py_uhash_t)2246822519UL)
#define MS_HASH_XXPRIME_5 ((Py_uhash_t)374761393UL)
#define MS_HASH_XXROTATE(x) ((x << 13) | (x >> 19))  /* Rotate left 13 bits */
#endif

/* Optimized version of PyLong_AsLongLongAndOverflow/PyLong_AsUnsignedLongLong.
 *
 * Returns True if sign * scale won't fit in an `int64` or a `uint64`.
 */
static inline bool
fast_long_extract_parts(PyObject *vv, bool *neg, uint64_t *scale) {
    PyLongObject *v = (PyLongObject *)vv;
    uint64_t prev, x = 0;
    bool negative;

#if PY312_PLUS
    /* CPython 3.12 changed int internal representation */
    int sign = 1 - (v->long_value.lv_tag & _PyLong_SIGN_MASK);
    negative = sign == -1;

    if (_PyLong_IsCompact(v)) {
        x = v->long_value.ob_digit[0];
    }
    else {
        Py_ssize_t i = v->long_value.lv_tag >> _PyLong_NON_SIZE_BITS;
        while (--i >= 0) {
            prev = x;
            x = (x << PyLong_SHIFT) + v->long_value.ob_digit[i];
            if ((x >> PyLong_SHIFT) != prev) {
                return true;
            }
        }
        if (negative && x > (1ull << 63)) {
            return true;
        }
    }
#else
    Py_ssize_t i = Py_SIZE(v);
    negative = false;

    if (MS_LIKELY(i == 1)) {
        x = v->ob_digit[0];
    }
    else if (i != 0) {
        negative = i < 0;
        if (MS_UNLIKELY(negative)) { i = -i; }
        while (--i >= 0) {
            prev = x;
            x = (x << PyLong_SHIFT) + v->ob_digit[i];
            if ((x >> PyLong_SHIFT) != prev) {
                return true;
            }
        }
        if (negative && x > (1ull << 63)) {
            return true;
        }
    }
#endif

    *neg = negative;
    *scale = x;
    return false;
}

/* Access macro to the members which are floating "behind" the object */
#define MS_PyHeapType_GET_MEMBERS(etype) \
    ((PyMemberDef *)(((char *)(etype)) + Py_TYPE(etype)->tp_basicsize))

#define MS_GET_FIRST_SLOT(obj) \
    *((PyObject **)((char *)(obj) + sizeof(PyObject))) \

#define MS_SET_FIRST_SLOT(obj, val) \
    MS_GET_FIRST_SLOT(obj) = (val)


/*************************************************************************
 * Lookup Tables                                                         *
 *************************************************************************/

static const char hex_encode_table[] = "0123456789abcdef";

static const char base64_encode_table[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*************************************************************************
 * GC Utilities                                                          *
 *************************************************************************/

#define MS_TYPE_IS_GC(t) (((PyTypeObject *)(t))->tp_flags & Py_TPFLAGS_HAVE_GC)
#define MS_OBJECT_IS_GC(obj) MS_TYPE_IS_GC(Py_TYPE(obj))
#define MS_IS_TRACKED(o) PyObject_GC_IsTracked(o)

/* Is this object something that is/could be GC tracked? True if
 * - the value supports GC
 * - the value isn't a tuple or the object is tracked (skip tracked checks for non-tuples)
 */
#define MS_MAYBE_TRACKED(x) \
    (MS_TYPE_IS_GC(Py_TYPE(x)) && \
     (!PyTuple_CheckExact(x) || MS_IS_TRACKED(x)))

/*************************************************************************
 * Murmurhash2                                                           *
 *************************************************************************/

static inline uint32_t
unaligned_load(const unsigned char *p) {
    uint32_t out;
    memcpy(&out, p, sizeof(out));
    return out;
}

static inline uint32_t
murmur2(const char *p, Py_ssize_t len) {
    const unsigned char *buf = (unsigned char *)p;
    const uint32_t m = 0x5bd1e995;
    uint32_t hash = (uint32_t)len;

    while(len >= 4) {
        uint32_t k = unaligned_load(buf);
        k *= m;
        k ^= k >> 24;
        k *= m;
        hash *= m;
        hash ^= k;
        buf += 4;
        len -= 4;
    }

    switch(len) {
        case 3:
            hash ^= buf[2] << 16;
            MS_FALLTHROUGH;
        case 2:
            hash ^= buf[1] << 8;
            MS_FALLTHROUGH;
        case 1:
            hash ^= buf[0];
            hash *= m;
    };

    hash ^= hash >> 13;
    hash *= m;
    hash ^= hash >> 15;
    return hash;
}

/*************************************************************************
 * Endian handling macros                                                *
 *************************************************************************/

#define _structtype_store16(to, x) do { \
    ((uint8_t*)to)[0] = (uint8_t)((x >> 8) & 0xff); \
    ((uint8_t*)to)[1] = (uint8_t)(x & 0xff); \
} while (0);

#define _structtype_store32(to, x) do { \
    ((uint8_t*)to)[0] = (uint8_t)((x >> 24) & 0xff); \
    ((uint8_t*)to)[1] = (uint8_t)((x >> 16) & 0xff); \
    ((uint8_t*)to)[2] = (uint8_t)((x >> 8) & 0xff); \
    ((uint8_t*)to)[3] = (uint8_t)(x & 0xff); \
} while (0);

#define _structtype_store64(to, x) do { \
    ((uint8_t*)to)[0] = (uint8_t)((x >> 56) & 0xff); \
    ((uint8_t*)to)[1] = (uint8_t)((x >> 48) & 0xff); \
    ((uint8_t*)to)[2] = (uint8_t)((x >> 40) & 0xff); \
    ((uint8_t*)to)[3] = (uint8_t)((x >> 32) & 0xff); \
    ((uint8_t*)to)[4] = (uint8_t)((x >> 24) & 0xff); \
    ((uint8_t*)to)[5] = (uint8_t)((x >> 16) & 0xff); \
    ((uint8_t*)to)[6] = (uint8_t)((x >> 8) & 0xff); \
    ((uint8_t*)to)[7] = (uint8_t)(x & 0xff); \
} while (0);

#define _structtype_load16(cast, from) ((cast)( \
    (((uint16_t)((uint8_t*)from)[0]) << 8) | \
    (((uint16_t)((uint8_t*)from)[1])     ) ))

#define _structtype_load32(cast, from) ((cast)( \
    (((uint32_t)((uint8_t*)from)[0]) << 24) | \
    (((uint32_t)((uint8_t*)from)[1]) << 16) | \
    (((uint32_t)((uint8_t*)from)[2]) <<  8) | \
    (((uint32_t)((uint8_t*)from)[3])      ) ))

#define _structtype_load64(cast, from) ((cast)( \
    (((uint64_t)((uint8_t*)from)[0]) << 56) | \
    (((uint64_t)((uint8_t*)from)[1]) << 48) | \
    (((uint64_t)((uint8_t*)from)[2]) << 40) | \
    (((uint64_t)((uint8_t*)from)[3]) << 32) | \
    (((uint64_t)((uint8_t*)from)[4]) << 24) | \
    (((uint64_t)((uint8_t*)from)[5]) << 16) | \
    (((uint64_t)((uint8_t*)from)[6]) << 8)  | \
    (((uint64_t)((uint8_t*)from)[7])     )  ))

/*************************************************************************
 * Module level state                                                    *
 *************************************************************************/

/* State of the structtype module */
typedef struct {
    PyObject *EncodeError;
    PyObject *DecodeError;
    PyObject *ValidationError;
    PyObject *StructType;
    PyTypeObject *EnumMetaType;
    PyTypeObject *ABCMetaType;
    PyObject *_abc_init;
    PyObject *struct_lookup_cache;
    PyObject *str___weakref__;
    PyObject *str___dict__;
    PyObject *str___structtype_cached_hash__;
    PyObject *str__value2member_map_;
    PyObject *str___structtype_cache__;
    PyObject *str__value_;
    PyObject *str__missing_;
    PyObject *str_type;
    PyObject *str_strict;
    PyObject *str_sort_keys;
    PyObject *str_decimal_format;
    PyObject *str_uuid_format;
    PyObject *str_utcoffset;
    PyObject *str___origin__;
    PyObject *str___args__;
    PyObject *str___metadata__;
    PyObject *str___total__;
    PyObject *str___required_keys__;
    PyObject *str__fields;
    PyObject *str__field_defaults;
    PyObject *str___post_init__;
    PyObject *str___dataclass_fields__;
    PyObject *str___attrs_attrs__;
    PyObject *str___supertype__;
#if PY312_PLUS
    PyObject *str___value__;
#endif
    PyObject *str___bound__;
    PyObject *str___constraints__;
    PyObject *str_int;
    PyObject *str_is_safe;
    PyObject *UUIDType;
    PyObject *uuid_safeuuid_unknown;
    PyObject *DecimalType;
    PyObject *EnumType;
    PyObject *typing_union;
    PyObject *typing_any;
    PyObject *typing_literal;
    PyObject *typing_classvar;
    PyObject *typing_typevar;
    PyObject *typing_final;
    PyObject *typing_generic;
    PyObject *typing_generic_alias;
    PyObject *typing_annotated_alias;
    PyObject *concrete_types;
    PyObject *get_type_hints;
    PyObject *get_class_annotations;
    PyObject *get_typeddict_info;
    PyObject *get_dataclass_info;
    PyObject *convert_generic_alias;
    PyObject *rebuild;
    PyObject *types_uniontype;
#if PY312_PLUS
    PyObject *typing_typealiastype;
#endif
    PyObject *astimezone;
    PyObject *re_compile;
} StructspecState;

/* Forward declaration of the structtype module definition. */
static struct PyModuleDef structtypemodule;

/* Given a module object, get its per-module state. */
static StructspecState *
structtype_get_state(PyObject *module)
{
    return (StructspecState *)PyModule_GetState(module);
}

/* Find the module instance imported in the currently running sub-interpreter
   and get its state. */
static StructspecState *
structtype_get_global_state(void)
{
    PyObject *module = PyState_FindModule(&structtypemodule);
    return module == NULL ? NULL : structtype_get_state(module);
}

static int
ms_err_truncated(void)
{
    PyErr_SetString(structtype_get_global_state()->DecodeError, "Input data was truncated");
    return -1;
}

static int
ms_decimal_format_error(void)
{
    PyErr_SetString(PyExc_TypeError, "callable returned a value containing a Decimal");
    return -1;
}

/*************************************************************************
 * Utilities                                                             *
 *************************************************************************/

static PyObject*
find_keyword(PyObject *kwnames, PyObject *const *kwstack, PyObject *key)
{
    Py_ssize_t i, nkwargs;

    nkwargs = PyTuple_GET_SIZE(kwnames);
    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);

        /* kwname == key will normally find a match in since keyword keys
           should be interned strings; if not retry below in a new loop. */
        if (kwname == key) {
            return kwstack[i];
        }
    }

    for (i = 0; i < nkwargs; i++) {
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);
        assert(PyUnicode_Check(kwname));
        if (MS_UNICODE_EQ(kwname, key)) {
            return kwstack[i];
        }
    }
    return NULL;
}

static int
check_positional_nargs(Py_ssize_t nargs, Py_ssize_t min, Py_ssize_t max) {
    if (nargs > max) {
        PyErr_SetString(
            PyExc_TypeError,
            "Extra positional arguments provided"
        );
        return 0;
    }
    else if (nargs < min) {
        PyErr_Format(
            PyExc_TypeError,
            "Missing %zd required arguments",
            min - nargs
        );
        return 0;
    }
    return 1;
}

/* A utility incrementally building strings */
typedef struct strbuilder {
    char *sep;
    Py_ssize_t sep_size;
    char *buffer;
    Py_ssize_t size;  /* How many bytes have been written */
    Py_ssize_t capacity;  /* How many bytes can be written */
} strbuilder;

#define strbuilder_extend_literal(self, str) strbuilder_extend(self, str, sizeof(str) - 1)

static bool strbuilder_extend(strbuilder *self, const char *buf, Py_ssize_t nbytes) {
    bool is_first_write = self->size == 0;
    Py_ssize_t required = self->size + nbytes + self->sep_size;

    if (required > self->capacity) {
        self->capacity = (Py_ssize_t)(required * 1.5);
        char *new_buf = PyMem_Realloc(self->buffer, self->capacity);
        if (new_buf == NULL) {
            PyErr_NoMemory();
            PyMem_Free(self->buffer);
            self->buffer = NULL;
            return false;
        }
        self->buffer = new_buf;
    }
    if (self->sep_size && !is_first_write) {
        memcpy(self->buffer + self->size, self->sep, self->sep_size);
        self->size += self->sep_size;
    }
    memcpy(self->buffer + self->size, buf, nbytes);
    self->size += nbytes;
    return true;
}

static bool
strbuilder_extend_unicode(strbuilder *self, PyObject *obj) {
    Py_ssize_t size;
    const char* buf = unicode_str_and_size(obj, &size);
    if (buf == NULL) return false;
    return strbuilder_extend(self, buf, size);
}

static void
strbuilder_reset(strbuilder *self) {
    if (self->capacity != 0 && self->buffer != NULL) {
        PyMem_Free(self->buffer);
    }
    self->buffer = NULL;
    self->size = 0;
    self->capacity = 0;
}

static PyObject *
strbuilder_build(strbuilder *self) {
    PyObject *out = PyUnicode_FromStringAndSize(self->buffer, self->size);
    strbuilder_reset(self);
    return out;
}

static MS_INLINE PyObject *
ms_get_annotate_from_class_namespace(PyObject *namespace) {
    /* We replicate the behavior of the standard library utility to avoid
     * unnecessary function call overhead.
     * https://docs.python.org/3/library/annotationlib.html#annotationlib.get_annotate_from_class_namespace */
    PyObject *annotate;

    annotate = PyDict_GetItemString(namespace, "__annotate__");
    if (annotate != NULL) {
        Py_INCREF(annotate);
        return annotate;
    }

    annotate = PyDict_GetItemString(namespace, "__annotate_func__");
    if (annotate != NULL) {
        Py_INCREF(annotate);
        return annotate;
    }

    Py_RETURN_NONE;
}

/*************************************************************************
 * PathNode                                                              *
 *************************************************************************/

#define PATH_ELLIPSIS -1
#define PATH_STR -2
#define PATH_KEY -3
#define PATH_STR_BRACKET -4

typedef struct PathNode {
    struct PathNode *parent;
    Py_ssize_t index;
    PyObject *object;
} PathNode;

/* reverse the parent pointers in the path linked list */
static PathNode *
pathnode_reverse(PathNode *path) {
    PathNode *current = path, *prev = NULL, *next = NULL;
    while (current != NULL) {
        next = current->parent;
        current->parent = prev;
        prev = current;
        current = next;
    }
    return prev;
}

static PyObject* StructMeta_get_field_name(PyObject*, Py_ssize_t);

static PyObject *
PathNode_ErrSuffix(PathNode *path) {
    strbuilder parts = {0};
    PathNode *path_orig;
    PyObject *out = NULL, *path_repr = NULL, *groups = NULL, *group = NULL;

    if (path == NULL) {
        return PyUnicode_FromString("");
    }

    /* Reverse the parent pointers for easier printing */
    path = pathnode_reverse(path);

    /* Cache the original path to reset the parent pointers later */
    path_orig = path;

    /* Start with the root element */
    if (!strbuilder_extend_literal(&parts, "`$")) goto cleanup;

    while (path != NULL) {
        if (path->index == PATH_STR_BRACKET) {
            if (!strbuilder_extend_literal(&parts, "['")) goto cleanup;
            if (PyUnicode_Check(path->object)) {
                if (!strbuilder_extend_unicode(&parts, path->object)) goto cleanup;
            }
            else {
                PyObject *key_repr = PyObject_Repr(path->object);
                if (key_repr == NULL) goto cleanup;
                if (!strbuilder_extend_unicode(&parts, key_repr)) {
                    Py_DECREF(key_repr);
                    goto cleanup;
                }
                Py_DECREF(key_repr);
            }
            if (!strbuilder_extend_literal(&parts, "']")) goto cleanup;
        }
        else if (path->object != NULL) {
            PyObject *name;
            if (path->index == PATH_STR) {
                name = path->object;
            }
            else {
                name = StructMeta_get_field_name(path->object, path->index);
            }
            if (!strbuilder_extend_literal(&parts, ".")) goto cleanup;
            if (!strbuilder_extend_unicode(&parts, name)) goto cleanup;
        }
        else if (path->index == PATH_ELLIPSIS) {
            if (!strbuilder_extend_literal(&parts, "[...]")) goto cleanup;
        }
        else if (path->index == PATH_KEY) {
            if (groups == NULL) {
                groups = PyList_New(0);
                if (groups == NULL) goto cleanup;
            }
            if (!strbuilder_extend_literal(&parts, "`")) goto cleanup;
            group = strbuilder_build(&parts);
            if (group == NULL) goto cleanup;
            if (PyList_Append(groups, group) < 0) goto cleanup;
            Py_CLEAR(group);
            strbuilder_extend_literal(&parts, "`key");
        }
        else {
            char buf[20];
            char *p = &buf[20];
            Py_ssize_t x = path->index;
            if (!strbuilder_extend_literal(&parts, "[")) goto cleanup;
            while (x >= 100) {
                const int64_t old = x;
                p -= 2;
                x /= 100;
                memcpy(p, DIGIT_TABLE + ((old - (x * 100)) << 1), 2);
            }
            if (x >= 10) {
                p -= 2;
                memcpy(p, DIGIT_TABLE + (x << 1), 2);
            }
            else {
                *--p = (char)(x + '0');
            }
            if (!strbuilder_extend(&parts, p, &buf[20] - p)) goto cleanup;
            if (!strbuilder_extend_literal(&parts, "]")) goto cleanup;
        }
        path = path->parent;
    }
    if (!strbuilder_extend_literal(&parts, "`")) goto cleanup;

    if (groups == NULL) {
        path_repr = strbuilder_build(&parts);
    }
    else {
        group = strbuilder_build(&parts);
        if (group == NULL) goto cleanup;
        if (PyList_Append(groups, group) < 0) goto cleanup;
        PyObject *sep = PyUnicode_FromString(" in ");
        if (sep == NULL) goto cleanup;
        if (PyList_Reverse(groups) < 0) goto cleanup;
        path_repr = PyUnicode_Join(sep, groups);
        Py_DECREF(sep);
    }

    out = PyUnicode_FromFormat(" - at %U", path_repr);

cleanup:
    Py_XDECREF(path_repr);
    Py_XDECREF(group);
    Py_XDECREF(groups);
    pathnode_reverse(path_orig);
    strbuilder_reset(&parts);
    return out;
}

/*************************************************************************
 * Lookup Tables for ints & strings                                      *
 *************************************************************************/

typedef struct Lookup {
    PyObject_VAR_HEAD
    PyObject *tag_field;  /* used for struct lookup table only */
    PyObject *cls;  /* Used for enum lookup table only */
    bool array_like;
} Lookup;

static PyTypeObject IntLookup_Type;
static PyTypeObject StrLookup_Type;

typedef struct IntLookup {
    Lookup common;
    bool compact;
} IntLookup;

typedef struct IntLookupEntry {
    int64_t key;
    PyObject *value;
} IntLookupEntry;

typedef struct IntLookupHashmap {
    IntLookup base;
    IntLookupEntry table[];
} IntLookupHashmap;

typedef struct IntLookupCompact {
    IntLookup base;
    int64_t offset;
    PyObject* table[];
} IntLookupCompact;

typedef struct StrLookupEntry {
    PyObject *key;
    PyObject *value;
} StrLookupEntry;

typedef struct StrLookup {
    Lookup common;
    StrLookupEntry table[];
} StrLookup;

#define Lookup_array_like(obj) ((Lookup *)(obj))->array_like
#define Lookup_tag_field(obj) ((Lookup *)(obj))->tag_field
#define Lookup_IsStrLookup(obj) (Py_TYPE(obj) == &StrLookup_Type)
#define Lookup_IsIntLookup(obj) (Py_TYPE(obj) == &IntLookup_Type)

/* Handles Enum._missing_ calls. Returns a new reference, or NULL on error.
 * Will decref val if non-null. */
static PyObject *
_Lookup_OnMissing(Lookup *lookup, PyObject *val, PathNode *path) {
    if (val == NULL) return NULL;

    StructspecState *mod = structtype_get_global_state();

    if (lookup->cls != NULL) {
        PyObject *out = PyObject_CallMethodOneArg(lookup->cls, mod->str__missing_, val);
        if (out == NULL) {
            PyErr_Clear();
        }
        else if (out == Py_None) {
            Py_DECREF(out);
        }
        else {
            Py_DECREF(val);
            return out;
        }
    }
    PyObject *suffix = PathNode_ErrSuffix(path);
    if (suffix != NULL) {
        PyErr_Format(mod->ValidationError, "Invalid enum value %R%U", val, suffix);
        Py_DECREF(suffix);
    }

    Py_DECREF(val);
    return NULL;
}

static IntLookupEntry *
_IntLookupHashmap_lookup(IntLookupHashmap *self, int64_t key) {
    IntLookupEntry *table = self->table;
    size_t mask = Py_SIZE(self) - 1;
    size_t i = key & mask;

    while (true) {
        IntLookupEntry *entry = &table[i];
        if (MS_LIKELY(entry->key == key)) return entry;
        if (entry->value == NULL) return entry;
        i = (i + 1) & mask;
    }
    Py_UNREACHABLE();
}

static void
_IntLookupHashmap_Set(IntLookupHashmap *self, int64_t key, PyObject *value) {
    IntLookupEntry *entry = _IntLookupHashmap_lookup(self, key);
    Py_XDECREF(entry->value);
    Py_INCREF(value);
    entry->key = key;
    entry->value = value;
}

static PyObject *
IntLookup_New(PyObject *arg, PyObject *tag_field, PyObject *cls, bool array_like) {
    Py_ssize_t nitems;
    PyObject *item, *items = NULL;
    IntLookup *self = NULL;
    int64_t imin = LLONG_MAX, imax = LLONG_MIN;

    if (PyDict_CheckExact(arg)) {
        nitems = PyDict_GET_SIZE(arg);
    }
    else {
        items = PySequence_Tuple(arg);
        if (items == NULL) return NULL;
        nitems = PyTuple_GET_SIZE(items);
    }

    /* Must have at least one item */
    if (nitems == 0) {
        PyErr_Format(
            PyExc_TypeError,
            "Enum types must have at least one item, %R is invalid",
            arg
        );
        goto cleanup;
    }

    /* Find the min/max of items, and error if any item isn't an integer or is
     * out of range */
#define handle(key) \
    do { \
        int overflow = 0; \
        int64_t ival = PyLong_AsLongLongAndOverflow(key, &overflow); \
        if (overflow) { \
            PyErr_SetString( \
                PyExc_NotImplementedError, \
                "Integer values > (2**63 - 1) are not currently supported for " \
                "Enum/Literal/integer tags. If you need this feature, please " \
                "open an issue on GitHub." \
            ); \
            goto cleanup; \
        } \
        if (ival == -1 && PyErr_Occurred()) goto cleanup; \
        if (ival < imin) { \
            imin = ival; \
        } \
        if (ival > imax) { \
            imax = ival; \
        } \
    } while (false)
    if (PyDict_CheckExact(arg)) {
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(arg, &pos, &key, &val)) {
            handle(key);
        }
    }
    else {
        for (Py_ssize_t i = 0; i < nitems; i++) {
            handle(PyTuple_GET_ITEM(items, i));
        }
    }
#undef handle

    /* Calculate range without overflow */
    uint64_t range;
    if (imax > 0) {
        range = imax;
        range -= imin;
    }
    else {
        range = imax - imin;
    }

    if (range < 1.4 * nitems) {
        /* Use compact representation */
        size_t size = range + 1;

        /* XXX: In Python 3.11+ there's not an easy way to allocate an untyped
         * block of memory that is also tracked by the GC. To hack around this
         * we set `tp_itemsize = 1` for `IntLookup_Type`, and manually calculate
         * the size of trailing parts. It's gross, but it works. */
        size_t nextra = (
            sizeof(IntLookupCompact)
            + (size * sizeof(PyObject *))
            - sizeof(IntLookup)
        );
        IntLookupCompact *out = PyObject_GC_NewVar(
            IntLookupCompact, &IntLookup_Type, nextra
        );
        if (out == NULL) goto cleanup;
        /* XXX: overwrite `ob_size`, since we lied above */
        Py_SET_SIZE(out, size);

        out->offset = imin;
        for (size_t i = 0; i < size; i++) {
            out->table[i] = NULL;
        }

#define setitem(key, val) \
    do { \
        int64_t ikey = PyLong_AsLongLong(key); \
        out->table[ikey - imin] = val; \
        Py_INCREF(val); \
    } while (false)

        if (PyDict_CheckExact(arg)) {
            PyObject *key, *val;
            Py_ssize_t pos = 0;
            while (PyDict_Next(arg, &pos, &key, &val)) {
                setitem(key, val);
            }
        }
        else {
            for (Py_ssize_t i = 0; i < nitems; i++) {
                item = PyTuple_GET_ITEM(items, i);
                setitem(item, item);
            }
        }

#undef setitem

        self = (IntLookup *)out;
        self->compact = true;
    }
    else {
        /* Use hashtable */
        size_t needed = nitems * 4 / 3;
        size_t size = 4;
        while (size < (size_t)needed) { size <<= 1; }

        /* XXX: This is hacky, see comment above allocating IntLookupCompact */
        size_t nextra = (
            sizeof(IntLookupHashmap)
            + (size * sizeof(IntLookupEntry))
            - sizeof(IntLookup)
        );
        IntLookupHashmap *out = PyObject_GC_NewVar(
            IntLookupHashmap, &IntLookup_Type, nextra
        );
        if (out == NULL) goto cleanup;
        /* XXX: overwrite `ob_size`, since we lied above */
        Py_SET_SIZE(out, size);

        for (size_t i = 0; i < size; i++) {
            out->table[i].key = 0;
            out->table[i].value = NULL;
        }

        if (PyDict_CheckExact(arg)) {
            PyObject *key, *val;
            Py_ssize_t pos = 0;
            while (PyDict_Next(arg, &pos, &key, &val)) {
                int64_t ival = PyLong_AsLongLong(key);
                _IntLookupHashmap_Set(out, ival, val);
            }
        }
        else {
            for (Py_ssize_t i = 0; i < nitems; i++) {
                PyObject *val = PyTuple_GET_ITEM(items, i);
                int64_t ival = PyLong_AsLongLong(val);
                _IntLookupHashmap_Set(out, ival, val);
            }
        }
        self = (IntLookup *)out;
        self->compact = false;
    }

    /* Store extra metadata (struct lookup only) */
    Py_XINCREF(tag_field);
    self->common.tag_field = tag_field;
    Py_XINCREF(cls);
    self->common.cls = cls;
    self->common.array_like = array_like;

cleanup:
    Py_XDECREF(items);
    if (self != NULL) {
        PyObject_GC_Track(self);
    }
    return (PyObject *)self;
}

static int
IntLookup_traverse(IntLookup *self, visitproc visit, void *arg)
{
    Py_VISIT(self->common.cls);
    Py_VISIT(self->common.tag_field);
    if (self->compact) {
        IntLookupCompact *lk = (IntLookupCompact *)self;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            Py_VISIT(lk->table[i]);
        }
    }
    else {
        IntLookupHashmap *lk = (IntLookupHashmap *)self;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            Py_VISIT(lk->table[i].value);
        }
    }
    return 0;
}

static int
IntLookup_clear(IntLookup *self)
{
    if (self->compact) {
        IntLookupCompact *lk = (IntLookupCompact *)self;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            Py_CLEAR(lk->table[i]);
        }
    }
    else {
        IntLookupHashmap *lk = (IntLookupHashmap *)self;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            Py_CLEAR(lk->table[i].value);
        }
    }
    Py_CLEAR(self->common.cls);
    Py_CLEAR(self->common.tag_field);
    return 0;
}

static void
IntLookup_dealloc(IntLookup *self)
{
    PyObject_GC_UnTrack(self);
    IntLookup_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
IntLookup_GetInt64(IntLookup *self, int64_t key) {
    if (MS_LIKELY(self->compact)) {
        IntLookupCompact *lk = (IntLookupCompact *)self;
        /* Compute the dense-table index in 64-bit space so large offsets
         * cannot wrap through Py_ssize_t on 32-bit builds. */
        if (key >= lk->offset) {
            uint64_t index = (uint64_t)key - (uint64_t)(lk->offset);
            if (index < (uint64_t)Py_SIZE(lk)) {
                return lk->table[(Py_ssize_t)index];
            }
        }
        return NULL;
    }
    return _IntLookupHashmap_lookup((IntLookupHashmap *)self, key)->value;
}

static PyObject *
IntLookup_GetInt64OrError(IntLookup *self, int64_t key, PathNode *path) {
    PyObject *out = IntLookup_GetInt64(self, key);
    if (out != NULL) {
        Py_INCREF(out);
        return out;
    }
    return _Lookup_OnMissing((Lookup *)self, PyLong_FromLongLong(key), path);
}

static PyObject *
IntLookup_GetUInt64(IntLookup *self, uint64_t key) {
    if (key > LLONG_MAX) return NULL;
    return IntLookup_GetInt64(self, key);
}

static PyObject *
IntLookup_GetUInt64OrError(IntLookup *self, uint64_t key, PathNode *path) {
    PyObject *out = IntLookup_GetUInt64(self, key);
    if (out != NULL) {
        Py_INCREF(out);
        return out;
    }
    return _Lookup_OnMissing(
        (Lookup *)self, PyLong_FromUnsignedLongLong(key), path
    );
}

static PyObject *
IntLookup_GetPyIntOrError(IntLookup *self, PyObject *key, PathNode *path) {
    uint64_t x;
    bool neg, overflow;
    PyObject *out = NULL;
    overflow = fast_long_extract_parts(key, &neg, &x);
    if (!overflow) {
        if (neg) {
            out = IntLookup_GetInt64(self, -(int64_t)x);
        }
        else {
            out = IntLookup_GetUInt64(self, x);
        }
    }
    if (out != NULL) {
        Py_INCREF(out);
        return out;
    }
    /* PyNumber_Long call ensures input is actual int */
    return _Lookup_OnMissing((Lookup *)self, PyNumber_Long(key), path);
}

static PyTypeObject IntLookup_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.IntLookup",
    .tp_basicsize = sizeof(IntLookup),
    .tp_itemsize = 1,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_dealloc = (destructor)IntLookup_dealloc,
    .tp_clear = (inquiry)IntLookup_clear,
    .tp_traverse = (traverseproc)IntLookup_traverse,
};

static StrLookupEntry *
_StrLookup_lookup(StrLookup *self, const char *key, Py_ssize_t size)
{
    StrLookupEntry *table = self->table;
    size_t hash = murmur2(key, size);
    size_t perturb = hash;
    size_t mask = Py_SIZE(self) - 1;
    size_t i = hash & mask;

    while (true) {
        StrLookupEntry *entry = &table[i];
        if (entry->value == NULL) return entry;
        Py_ssize_t entry_size;
        const char *entry_key = unicode_str_and_size_nocheck(entry->key, &entry_size);
        if (entry_size == size && memcmp(entry_key, key, size) == 0) return entry;
        /* Collision, perturb and try again */
        perturb >>= 5;
        i = mask & (i*5 + perturb + 1);
    }
    Py_UNREACHABLE();
}

static int
StrLookup_Set(StrLookup *self, PyObject *key, PyObject *value) {
    Py_ssize_t key_size;
    const char *key_str = unicode_str_and_size(key, &key_size);
    if (key_str == NULL) return -1;

    StrLookupEntry *entry = _StrLookup_lookup(self, key_str, key_size);
    entry->key = key;
    Py_INCREF(key);
    entry->value = value;
    Py_INCREF(value);
    return 0;
}

static PyObject *
StrLookup_New(PyObject *arg, PyObject *tag_field, PyObject *cls, bool array_like) {
    Py_ssize_t nitems;
    PyObject *item, *items = NULL;
    StrLookup *self = NULL;

    if (PyDict_CheckExact(arg)) {
        nitems = PyDict_GET_SIZE(arg);
    }
    else {
        items = PySequence_Tuple(arg);
        if (items == NULL) return NULL;
        nitems = PyTuple_GET_SIZE(items);
    }

    /* Must have at least one item */
    if (nitems == 0) {
        PyErr_Format(
            PyExc_TypeError,
            "Enum types must have at least one item, %R is invalid",
            arg
        );
        goto cleanup;
    }

    size_t needed = nitems * 4 / 3;
    size_t size = 4;
    while (size < (size_t)needed) {
        size <<= 1;
    }
    self = PyObject_GC_NewVar(StrLookup, &StrLookup_Type, size);
    if (self == NULL) goto cleanup;
    /* Zero out memory */
    self->common.cls = NULL;
    self->common.tag_field = NULL;
    for (size_t i = 0; i < size; i++) {
        self->table[i].key = NULL;
        self->table[i].value = NULL;
    }

    if (PyDict_CheckExact(arg)) {
        PyObject *key, *val;
        Py_ssize_t pos = 0;

        while (PyDict_Next(arg, &pos, &key, &val)) {
            if (!PyUnicode_CheckExact(key)) {
                PyErr_SetString(PyExc_RuntimeError, "Enum values must be strings");
                Py_CLEAR(self);
                goto cleanup;
            }
            if (StrLookup_Set(self, key, val) < 0) {
                Py_CLEAR(self);
                goto cleanup;
            }
        }
    }
    else {
        for (Py_ssize_t i = 0; i < nitems; i++) {
            item = PyTuple_GET_ITEM(items, i);
            if (!PyUnicode_CheckExact(item)) {
                PyErr_SetString(PyExc_RuntimeError, "Enum values must be strings");
                Py_CLEAR(self);
                goto cleanup;
            }
            if (StrLookup_Set(self, item, item) < 0) {
                Py_CLEAR(self);
                goto cleanup;
            }
        }
    }

    Py_XINCREF(cls);
    self->common.cls = cls;
    Py_XINCREF(tag_field);
    self->common.tag_field = tag_field;
    self->common.array_like = array_like;

cleanup:
    Py_XDECREF(items);
    if (self != NULL) {
        PyObject_GC_Track(self);
    }
    return (PyObject *)self;
}

static int
StrLookup_traverse(StrLookup *self, visitproc visit, void *arg)
{
    Py_VISIT(self->common.cls);
    Py_VISIT(self->common.tag_field);
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        Py_VISIT(self->table[i].key);
        Py_VISIT(self->table[i].value);
    }
    return 0;
}

static int
StrLookup_clear(StrLookup *self)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        Py_CLEAR(self->table[i].key);
        Py_CLEAR(self->table[i].value);
    }
    Py_CLEAR(self->common.cls);
    Py_CLEAR(self->common.tag_field);
    return 0;
}

static void
StrLookup_dealloc(StrLookup *self)
{
    PyObject_GC_UnTrack(self);
    StrLookup_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
StrLookup_Get(StrLookup *self, const char *key, Py_ssize_t size) {
    StrLookupEntry *entry = _StrLookup_lookup(self, key, size);
    return entry->value;
}

static PyObject *
StrLookup_GetOrError(
    StrLookup *self, const char *key, Py_ssize_t size, PathNode *path
) {
    PyObject *out = StrLookup_Get(self, key, size);
    if (out != NULL) {
        Py_INCREF(out);
        return out;
    }
    return _Lookup_OnMissing(
        (Lookup *)self, PyUnicode_FromStringAndSize(key, size), path
    );
}

static PyTypeObject StrLookup_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.StrLookup",
    .tp_basicsize = sizeof(StrLookup),
    .tp_itemsize = sizeof(StrLookupEntry),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_dealloc = (destructor) StrLookup_dealloc,
    .tp_clear = (inquiry)StrLookup_clear,
    .tp_traverse = (traverseproc) StrLookup_traverse,
};

/*************************************************************************
 * Raw                                                                   *
 *************************************************************************/

static PyTypeObject Raw_Type;

typedef struct Raw {
    PyObject_HEAD
    PyObject *base;
    char *buf;
    Py_ssize_t len;
    bool is_view;
    Py_buffer view;
} Raw;

static PyObject *
Raw_New(PyObject *msg) {
    Raw *out = (Raw *)Raw_Type.tp_alloc(&Raw_Type, 0);
    if (out == NULL) return NULL;
    if (PyBytes_CheckExact(msg)) {
        Py_INCREF(msg);
        out->base = msg;
        out->buf = PyBytes_AS_STRING(msg);
        out->len = PyBytes_GET_SIZE(msg);
        out->is_view = false;
    }
    else if (PyUnicode_CheckExact(msg)) {
        out->base = msg;
        out->buf = (char *)unicode_str_and_size(msg, &out->len);
        if (out->buf == NULL) return NULL;
        Py_INCREF(msg);
        out->is_view = false;
    }
    else {
        Py_buffer buffer;
        if (PyObject_GetBuffer(msg, &buffer, PyBUF_CONTIG_RO) < 0) {
            Py_DECREF(out);
            return NULL;
        }
        out->view = buffer;
        out->base = buffer.obj;
        out->buf = buffer.buf;
        out->len = buffer.len;
        out->is_view = true;
    }
    return (PyObject *)out;
}

PyDoc_STRVAR(Raw__doc__,
"Raw(msg="", /)\n"
"--\n"
"\n"
"A buffer containing an encoded message.\n"
"\n"
"Raw objects have two common uses:\n"
"\n"
"- During decoding. Fields annotated with the ``Raw`` type won't be decoded\n"
"  immediately, but will instead return a ``Raw`` object with a view into the\n"
"  original message where that field is encoded. This is useful for decoding\n"
"  fields whose type may only be inferred after decoding other fields.\n"
"- During encoding. Raw objects wrap pre-encoded messages. These can be added\n"
"  as components of larger messages without having to pay the cost of decoding\n"
"  and re-encoding them.\n"
"\n"
"Parameters\n"
"----------\n"
"msg: bytes, bytearray, memoryview, or str, optional\n"
"    A buffer containing an encoded message. One of bytes, bytearray, memoryview,\n"
"    str, or any object that implements the buffer protocol. If not present,\n"
"    defaults to an empty buffer."
);
static PyObject *
Raw_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    PyObject *msg;
    Py_ssize_t nargs, nkwargs;

    nargs = PyTuple_GET_SIZE(args);
    nkwargs = (kwargs == NULL) ? 0 : PyDict_GET_SIZE(kwargs);

    if (nkwargs != 0) {
        PyErr_SetString(
            PyExc_TypeError,
            "Raw takes no keyword arguments"
        );
        return NULL;
    }
    else if (nargs == 0) {
        msg = PyBytes_FromStringAndSize(NULL, 0);
        if (msg == NULL) return NULL;
        /* This looks weird, but is safe since the empty bytes object is an
         * immortal singleton */
        Py_DECREF(msg);
    }
    else if (nargs == 1) {
        msg = PyTuple_GET_ITEM(args, 0);
    }
    else {
        PyErr_Format(
            PyExc_TypeError,
            "Raw expected at most 1 arguments, got %zd",
            nargs
        );
        return NULL;
    }
    return Raw_New(msg);
}

static void
Raw_dealloc(Raw *self)
{
    if (self->base != NULL) {
        if (!self->is_view) {
            Py_DECREF(self->base);
        }
        else {
            ms_release_buffer(&self->view);
        }
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
Raw_FromView(PyObject *buffer_obj, char *data, Py_ssize_t len) {
    Raw *out = (Raw *)Raw_Type.tp_alloc(&Raw_Type, 0);
    if (out == NULL) return NULL;

    Py_buffer buffer;
    if (ms_get_buffer(buffer_obj, &buffer) < 0) {
        Py_DECREF(out);
        return NULL;
    }
    out->view = buffer;
    out->base = buffer.obj;
    out->buf = data;
    out->len = len;
    out->is_view = true;
    return (PyObject *)out;
}

static PyObject *
Raw_richcompare(Raw *self, PyObject *other, int op) {
    if (Py_TYPE(other) != &Raw_Type) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    Raw *raw_other = (Raw *)other;
    bool equal = (
        self == raw_other || (
            (self->len == raw_other->len) &&
            (memcmp(self->buf, raw_other->buf, self->len) == 0)
        )
    );
    bool result = (op == Py_EQ) ? equal : !equal;
    if (result) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static int
Raw_buffer_getbuffer(Raw *self, Py_buffer *view, int flags)
{
    return PyBuffer_FillInfo(view, (PyObject *)self, self->buf, self->len, 1, flags);
}

static PyBufferProcs Raw_as_buffer = {
    .bf_getbuffer = (getbufferproc)Raw_buffer_getbuffer
};

static Py_ssize_t
Raw_length(Raw *self) {
    return self->len;
}

static PySequenceMethods Raw_as_sequence = {
    .sq_length = (lenfunc)Raw_length
};

static PyObject *
Raw_reduce(Raw *self, PyObject *unused)
{
    if (!self->is_view) {
        return Py_BuildValue("O(O)", &Raw_Type, self->base);
    }
    return Py_BuildValue("O(y#)", &Raw_Type, self->buf, self->len);
}

PyDoc_STRVAR(Raw_copy__doc__,
"copy(self)\n"
"--\n"
"\n"
"Copy a Raw object.\n"
"\n"
"If the raw message is backed by a memoryview into a larger buffer (as happens\n"
"during decoding), the message is copied and the reference to the larger buffer\n"
"released. This may be useful to reduce memory usage if a Raw object created\n"
"during decoding will be kept in memory for a while rather than immediately\n"
"decoded and dropped."
);
static PyObject *
Raw_copy(Raw *self, PyObject *unused)
{
    if (!self->is_view) {
        Py_INCREF(self);
        return (PyObject *)self;
    }
    PyObject *buf = PyBytes_FromStringAndSize(self->buf, self->len);
    if (buf == NULL) return NULL;
    PyObject *out = Raw_New(buf);
    Py_DECREF(buf);
    return out;
}

static PyMethodDef Raw_methods[] = {
    {"__reduce__", (PyCFunction)Raw_reduce, METH_NOARGS},
    {"copy", (PyCFunction)Raw_copy, METH_NOARGS, Raw_copy__doc__},
    {NULL, NULL},
};

static PyTypeObject Raw_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype.Raw",
    .tp_doc = Raw__doc__,
    .tp_basicsize = sizeof(Raw),
    .tp_itemsize = sizeof(char),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Raw_new,
    .tp_dealloc = (destructor) Raw_dealloc,
    .tp_as_buffer = &Raw_as_buffer,
    .tp_as_sequence = &Raw_as_sequence,
    .tp_methods = Raw_methods,
    .tp_richcompare = (richcmpfunc) Raw_richcompare,
};

static bool
ensure_is_string(PyObject *val, const char *param) {
    if (PyUnicode_CheckExact(val)) return true;
    PyErr_Format(
        PyExc_TypeError,
        "`%s` must be a str, got %.200s",
        param, Py_TYPE(val)->tp_name
    );
    return false;
}

static bool
ensure_is_bool(PyObject *val, const char *param) {
    if (val == Py_True || val == Py_False) return true;
    PyErr_Format(
        PyExc_TypeError,
        "`%s` must be a bool, got %.200s",
        param, Py_TYPE(val)->tp_name
    );
    return false;
}

static bool
ensure_is_nonnegative_integer(PyObject *val, const char *param) {
    if (!PyLong_CheckExact(val)) {
        PyErr_Format(
            PyExc_TypeError,
            "`%s` must be an int, got %.200s",
            param, Py_TYPE(val)->tp_name
        );
        return false;
    }
    Py_ssize_t x = PyLong_AsSsize_t(val);
    if (x >= 0) return true;
    PyErr_Format(PyExc_ValueError, "`%s` must be >= 0, got %R", param, val);
    return false;
}

static bool
ensure_is_finite_numeric(PyObject *val, const char *param, bool positive) {
    double x;
    if (PyLong_CheckExact(val)) {
        x = PyLong_AsDouble(val);
    }
    else if (PyFloat_CheckExact(val)) {
        x = PyFloat_AS_DOUBLE(val);
        if (!isfinite(x)) {
            PyErr_Format(
                PyExc_ValueError,
                "`%s` must be finite, %R is not supported",
                param, val
            );
            return false;
        }
    }
    else {
        PyErr_Format(
            PyExc_TypeError,
            "`%s` must be an int or float, got %.200s",
            param, Py_TYPE(val)->tp_name
        );
        return false;
    }
    if (positive && x <= 0) {
        PyErr_Format(PyExc_ValueError, "`%s` must be > 0", param);
        return false;
    }
    return true;
}



static bool
_meta_repr_part(strbuilder *builder, const char *prefix, Py_ssize_t prefix_size, PyObject *field, bool *first) {
    if (*first) {
        *first = false;
    }
    else {
        if (!strbuilder_extend_literal(builder, ", ")) return false;
    }
    if (!strbuilder_extend(builder, prefix, prefix_size)) return false;
    PyObject *repr = PyObject_Repr(field);
    if (repr == NULL) return false;
    bool ok = strbuilder_extend_unicode(builder, repr);
    Py_DECREF(repr);
    return ok;
}

static int
_meta_richcompare_part(PyObject *left, PyObject *right) {
    if ((left == NULL) != (right == NULL)) {
        return 0;
    }
    if (left != NULL) {
        return PyObject_RichCompareBool(left, right, Py_EQ);
    }
    else {
        return 1;
    }
}



/*************************************************************************
 * NODEFAULT singleton                                                   *
 *************************************************************************/

PyObject _NoDefault_Object;
#define NODEFAULT &_NoDefault_Object

PyDoc_STRVAR(nodefault__doc__,
"NoDefaultType()\n"
"--\n"
"\n"
"A singleton indicating no default value is configured."
);
static PyObject *
nodefault_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) || (kwargs && PyDict_GET_SIZE(kwargs))) {
        PyErr_SetString(PyExc_TypeError, "NoDefaultType takes no arguments");
        return NULL;
    }
    Py_INCREF(NODEFAULT);
    return NODEFAULT;
}

static PyObject *
nodefault_repr(PyObject *op)
{
    return PyUnicode_FromString("NODEFAULT");
}

static PyObject *
nodefault_reduce(PyObject *op, PyObject *args)
{
    return PyUnicode_FromString("NODEFAULT");
}

static PyMethodDef nodefault_methods[] = {
    {"__reduce__", nodefault_reduce, METH_NOARGS, NULL},
    {NULL, NULL}
};

PyTypeObject NoDefault_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.NoDefaultType",
    .tp_doc = nodefault__doc__,
    .tp_repr = nodefault_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = nodefault_methods,
    .tp_new = nodefault_new,
    .tp_dealloc = 0,
    .tp_itemsize = 0,
    .tp_basicsize = 0
};

PyObject _NoDefault_Object = _PyObject_HEAD_INIT(&NoDefault_Type);

/*************************************************************************
 * UNSET singleton                                                       *
 *************************************************************************/

PyObject _Unset_Object;
#define UNSET &_Unset_Object

PyDoc_STRVAR(unset__doc__,
"UnsetType()\n"
"--\n"
"\n"
"A singleton indicating a field value is unset.\n"
"\n"
"This may be useful for working with message schemas where an unset field\n"
"in an object needs to be treated differently than one containing an explicit\n"
"``None`` value. In this case, you may use ``UNSET`` as the default value,\n"
"rather than ``None`` when defining object schemas. This feature is supported\n"
"for any `structtype.Struct`, `dataclasses` or `attrs` types.\n"
"\n"
"Examples\n"
"--------\n"
">>> from structtype import Struct, UnsetType, UNSET\n"
">>> class Example(Struct):\n"
"...     x: int\n"
"...     y: int | None | UnsetType = UNSET\n"
"\n"
"During encoding, any field containing ``UNSET`` is omitted from the message.\n"
"\n"
">>> Example(1).struct_dump_json()\n"
"b'{\"x\":1}'\n"
">>> Example(1, 2).struct_dump_json()\n"
"b'{\"x\":1,\"y\":2}'\n"
"\n"
"During decoding, if a field isn't explicitly set in the message, the default\n"
"value of ``UNSET`` will be set instead. This lets downstream consumers\n"
"determine whether a field was left unset, or explicitly set to ``None``\n"
"\n"
">>> Example.struct_validate_json(b'{\"x\": 1}')  # unset\n"
"Example(x=1, y=UNSET)\n"
">>> Example.struct_validate_json(b'{\"x\": 1, \"y\": null}')  # explicit null\n"
"Example(x=1, y=None)\n"
">>> Example.struct_validate_json(b'{\"x\": 1, \"y\": 2}')  # explicit value\n"
"Example(x=1, y=2)"
);
static PyObject *
unset_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    if (PyTuple_GET_SIZE(args) || (kwargs && PyDict_GET_SIZE(kwargs))) {
        PyErr_SetString(PyExc_TypeError, "UnsetType takes no arguments");
        return NULL;
    }
    Py_INCREF(UNSET);
    return UNSET;
}

static PyObject *
unset_repr(PyObject *op)
{
    return PyUnicode_FromString("UNSET");
}

static PyObject *
unset_reduce(PyObject *op, PyObject *args)
{
    return PyUnicode_FromString("UNSET");
}

static PyMethodDef unset_methods[] = {
    {"__reduce__", unset_reduce, METH_NOARGS, NULL},
    {NULL, NULL}
};

static int unset_bool(PyObject *obj) {
    return 0;
};

static PyNumberMethods unset_as_number = {
    .nb_bool = unset_bool,
};

PyTypeObject Unset_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype.UnsetType",
    .tp_doc = unset__doc__,
    .tp_repr = unset_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = unset_methods,
    .tp_as_number = &unset_as_number,
    .tp_new = unset_new,
    .tp_dealloc = 0,
    .tp_itemsize = 0,
    .tp_basicsize = 0
};

PyObject _Unset_Object = _PyObject_HEAD_INIT(&Unset_Type);


/*************************************************************************
 * Factory                                                               *
 *************************************************************************/

static PyTypeObject Factory_Type;

typedef struct {
    PyObject_HEAD
    PyObject *factory;
} Factory;

static PyObject *
Factory_New(PyObject *factory) {
    if (!PyCallable_Check(factory)) {
        PyErr_SetString(PyExc_TypeError, "factory must be callable");
        return NULL;
    }

    Factory *out = (Factory *)Factory_Type.tp_alloc(&Factory_Type, 0);
    if (out == NULL) return NULL;

    Py_INCREF(factory);
    out->factory = factory;
    return (PyObject *)out;
}

static PyObject *
Factory_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    Py_ssize_t nkwargs = (kwargs == NULL) ? 0 : PyDict_GET_SIZE(kwargs);
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    if (nkwargs != 0) {
        PyErr_SetString(PyExc_TypeError, "Factory takes no keyword arguments");
        return NULL;
    }
    else if (nargs != 1) {
        PyErr_Format(
            PyExc_TypeError,
            "Factory expected 1 argument, got %zd",
            nargs
        );
        return NULL;
    }
    else {
        return Factory_New(PyTuple_GET_ITEM(args, 0));
    }
}

static PyObject *
Factory_Call(PyObject *self) {
    PyObject *factory = ((Factory *)(self))->factory;
    /* Inline two common factory types */
    if (factory == (PyObject *)(&PyList_Type)) {
        return PyList_New(0);
    }
    else if (factory == (PyObject *)(&PyDict_Type)) {
        return PyDict_New();
    }
    return PyObject_CallNoArgs(factory);
}

static PyObject *
Factory_repr(PyObject *op)
{
    return PyUnicode_FromString("<factory>");
}

static int
Factory_traverse(Factory *self, visitproc visit, void *arg)
{
    Py_VISIT(self->factory);
    return 0;
}

static int
Factory_clear(Factory *self)
{
    Py_CLEAR(self->factory);
    return 0;
}

static void
Factory_dealloc(Factory *self)
{
    PyObject_GC_UnTrack(self);
    Factory_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Factory_members[] = {
    {"factory", T_OBJECT_EX, offsetof(Factory, factory), READONLY, "The factory function"},
    {NULL},
};

static PyTypeObject Factory_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype.Factory",
    .tp_basicsize = sizeof(Factory),
    .tp_itemsize = 0,
    .tp_new = Factory_new,
    .tp_repr = Factory_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)Factory_clear,
    .tp_traverse = (traverseproc)Factory_traverse,
    .tp_dealloc = (destructor) Factory_dealloc,
    .tp_members = Factory_members,
};

/*************************************************************************
 * Field                                                                 *
 *************************************************************************/

static PyTypeObject Field_Type;

typedef struct {
    PyObject_HEAD
    /* Constraint/metadata fields */
    PyObject *gt, *ge, *lt, *le, *multiple_of;
    PyObject *pattern, *regex;
    PyObject *min_length, *max_length, *tz;
    PyObject *title, *description, *examples, *deprecated;
    PyObject *json_schema_extra;
    /* Field-specific fields */
    PyObject *alias;
    /* Codec callables: custom <-> native conversion */
    PyObject *dump;      /* custom object -> serializable value */
    PyObject *validate;  /* serializable value -> custom object */
} Field;

PyDoc_STRVAR(Field__doc__,
"Configuration for a Struct field.\n"
"\n"
"Parameters\n"
"----------\n"
"alias : str, optional\n"
"    An alternative name to use when encoding/decoding this field.\n"
"    If present, this will override any struct-level configuration using\n"
"    the ``rename`` option for this field.\n"
"gt : int or float, optional\n"
"    The annotated value must be greater than ``gt``.\n"
"ge : int or float, optional\n"
"    The annotated value must be greater than or equal to ``ge``.\n"
"lt : int or float, optional\n"
"    The annotated value must be less than ``lt``.\n"
"le : int or float, optional\n"
"    The annotated value must be less than or equal to ``le``.\n"
"multiple_of : int or float, optional\n"
"    The annotated value must be a multiple of ``multiple_of``.\n"
"pattern : str, optional\n"
"    A regex pattern that the annotated value must match against.\n"
"    Note that the pattern is treated as **unanchored**, meaning the\n"
"    ``re.search`` method is used when matching.\n"
"min_length : int, optional\n"
"    The annotated value must have a length >= ``min_length``.\n"
"max_length : int, optional\n"
"    The annotated value must have a length <= ``max_length``.\n"
"tz : bool, optional\n"
"    Configures the timezone-requirements for annotated ``datetime``/\n"
"    ``time`` types. Set to ``True`` to require timezone-aware values,\n"
"    or ``False`` to require timezone-naive values. The default is\n"
"    ``None``, which accepts either.\n"
"title : str, optional\n"
"    The title to use for the annotated value when generating a\n"
"    json-schema.\n"
"description : str, optional\n"
"    The description to use for the annotated value when generating a\n"
"    json-schema.\n"
"examples : list, optional\n"
"    A list of examples to use when generating a json-schema.\n"
"deprecated : bool, optional\n"
"    Mark the annotated value as deprecated in the generated json-schema.\n"
"json_schema_extra : dict, optional\n"
"    A dict of extra fields to set when generating a json-schema.\n"
"    This dict is recursively merged with the generated schema,\n"
"    with ``json_schema_extra`` overriding any conflicting\n"
"    autogenerated fields.\n"
"\n"
"Examples\n"
"--------\n"
"Here we use ``Field`` to add constraints on two different types. The first\n"
"defines a new type alias ``NonNegativeInt``, which is an integer that must be\n"
"``>= 0``. This type alias can be reused in multiple locations. The second uses\n"
"``Field`` inline in a struct definition to restrict the ``name`` string field\n"
"to a maximum length of 32 characters.\n"
"\n"
">>> from typing import Annotated\n"
">>> from structtype import Struct, Field\n"
">>> NonNegativeInt = Annotated[int, Field(ge=0)]\n"
">>> class User(Struct):\n"
"...     name: Annotated[str, Field(max_length=32)]\n"
"...     age: NonNegativeInt\n"
"...\n"
">>> User.struct_validate_json(b'{\"name\": \"alice\", \"age\": 25}')\n"
"User(name='alice', age=25)\n"
);
static PyObject *
Field_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    char *kwlist[] = {
        "gt", "ge", "lt", "le", "multiple_of",
        "pattern", "min_length", "max_length", "tz",
        "alias",
        "title", "description", "examples", "json_schema_extra",
        "deprecated",
        "dump", "validate",
        NULL
    };
    PyObject *gt = NULL, *ge = NULL, *lt = NULL, *le = NULL, *multiple_of = NULL;
    PyObject *pattern = NULL, *min_length = NULL, *max_length = NULL, *tz = NULL;
    PyObject *alias = Py_None;
    PyObject *title = NULL, *description = NULL, *examples = NULL;
    PyObject *deprecated = NULL;
    PyObject *json_schema_extra = NULL;
    PyObject *regex = NULL;
    PyObject *dump = NULL, *validate = NULL;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "|$OOOOOOOOOOOOOOOOO:Field.__new__", kwlist,
            &gt, &ge, &lt, &le, &multiple_of,
            &pattern, &min_length, &max_length, &tz,
            &alias,
            &title, &description, &examples, &json_schema_extra,
            &deprecated,
            &dump, &validate
        )
    )
        return NULL;

#define NONE_TO_NULL(x) do { if (x == Py_None) {x = NULL;} } while(0)
    NONE_TO_NULL(gt);
    NONE_TO_NULL(ge);
    NONE_TO_NULL(lt);
    NONE_TO_NULL(le);
    NONE_TO_NULL(multiple_of);
    NONE_TO_NULL(pattern);
    NONE_TO_NULL(min_length);
    NONE_TO_NULL(max_length);
    NONE_TO_NULL(tz);
    NONE_TO_NULL(title);
    NONE_TO_NULL(description);
    NONE_TO_NULL(examples);
    NONE_TO_NULL(deprecated);
    NONE_TO_NULL(json_schema_extra);
    NONE_TO_NULL(dump);
    NONE_TO_NULL(validate);
#undef NONE_TO_NULL

    if (dump != NULL && !PyCallable_Check(dump)) {
        PyErr_SetString(PyExc_TypeError, "dump must be callable");
        return NULL;
    }
    if (validate != NULL && !PyCallable_Check(validate)) {
        PyErr_SetString(PyExc_TypeError, "validate must be callable");
        return NULL;
    }

    /* Check constraint parameter types/values */
    if (gt != NULL && !ensure_is_finite_numeric(gt, "gt", false)) return NULL;
    if (ge != NULL && !ensure_is_finite_numeric(ge, "ge", false)) return NULL;
    if (lt != NULL && !ensure_is_finite_numeric(lt, "lt", false)) return NULL;
    if (le != NULL && !ensure_is_finite_numeric(le, "le", false)) return NULL;
    if (multiple_of != NULL && !ensure_is_finite_numeric(multiple_of, "multiple_of", true)) return NULL;
    if (pattern != NULL && !ensure_is_string(pattern, "pattern")) return NULL;
    if (min_length != NULL && !ensure_is_nonnegative_integer(min_length, "min_length")) return NULL;
    if (max_length != NULL && !ensure_is_nonnegative_integer(max_length, "max_length")) return NULL;
    if (tz != NULL && !ensure_is_bool(tz, "tz")) return NULL;
    if (deprecated != NULL && !ensure_is_bool(deprecated, "deprecated")) return NULL;

    /* Check multiple constraint restrictions */
    if (gt != NULL && ge != NULL) {
        PyErr_SetString(PyExc_ValueError, "Cannot specify both `gt` and `ge`");
        return NULL;
    }
    if (lt != NULL && le != NULL) {
        PyErr_SetString(PyExc_ValueError, "Cannot specify both `lt` and `le`");
        return NULL;
    }
    bool numeric = (gt != NULL || ge != NULL || lt != NULL || le != NULL || multiple_of != NULL);
    bool other = (pattern != NULL || min_length != NULL || max_length != NULL || tz != NULL);
    if (numeric && other) {
        PyErr_SetString(
            PyExc_ValueError,
            "Cannot mix numeric constraints (gt, lt, ...) with non-numeric "
            "constraints (pattern, min_length, max_length, tz)"
        );
        return NULL;
    }

    /* Check Field-specific defaults */
    if (alias == Py_None) {
        alias = NULL;
    }
    else if (!PyUnicode_CheckExact(alias)) {
        PyErr_SetString(PyExc_TypeError, "alias must be a str or None");
        return NULL;
    }

    /* Check types/values of extra metadata */
    if (title != NULL && !ensure_is_string(title, "title")) return NULL;
    if (description != NULL && !ensure_is_string(description, "description")) return NULL;
    if (examples != NULL && !PyList_CheckExact(examples)) {
        PyErr_Format(
            PyExc_TypeError,
            "`examples` must be a list, got %.200s",
            Py_TYPE(examples)->tp_name
        );
        return NULL;
    }
    if (json_schema_extra != NULL && !PyDict_CheckExact(json_schema_extra)) {
        PyErr_Format(
            PyExc_TypeError,
            "`json_schema_extra` must be a dict, got %.200s",
            Py_TYPE(json_schema_extra)->tp_name
        );
        return NULL;
    }

    /* regex compile pattern if provided */
    if (pattern != NULL) {
        StructspecState *mod = structtype_get_global_state();
        regex = PyObject_CallOneArg(mod->re_compile, pattern);
        if (regex == NULL) return NULL;
    }

    Field *self = (Field *)Field_Type.tp_alloc(&Field_Type, 0);
    if (self == NULL) {
        Py_XDECREF(regex);
        return NULL;
    }

#define SET_FIELD(x) do { Py_XINCREF(x); self->x = x; } while(0)
#define SET_FIELD_OWNED(x) do { self->x = x; } while(0)
    SET_FIELD(gt);
    SET_FIELD(ge);
    SET_FIELD(lt);
    SET_FIELD(le);
    SET_FIELD(multiple_of);
    SET_FIELD(pattern);
    SET_FIELD_OWNED(regex);
    SET_FIELD(min_length);
    SET_FIELD(max_length);
    SET_FIELD(tz);
    Py_XINCREF(alias);
    self->alias = alias;
    SET_FIELD(title);
    SET_FIELD(description);
    SET_FIELD(examples);
    SET_FIELD(deprecated);
    SET_FIELD(json_schema_extra);
    SET_FIELD(dump);
    SET_FIELD(validate);
#undef SET_FIELD
#undef SET_FIELD_OWNED

    return (PyObject *)self;
}

static PyObject *
Field_repr(Field *self) {
    strbuilder builder = {0};
    bool first = true;
    if (!strbuilder_extend_literal(&builder, "structtype.Field(")) return NULL;
#define DO_REPR(field) do { \
    if (self->field != NULL) { \
        if (!_meta_repr_part(&builder, #field "=", sizeof(#field), self->field, &first)) { \
            goto error; \
        } \
    } \
} while(0)
    DO_REPR(gt);
    DO_REPR(ge);
    DO_REPR(lt);
    DO_REPR(le);
    DO_REPR(multiple_of);
    DO_REPR(pattern);
    DO_REPR(min_length);
    DO_REPR(max_length);
    DO_REPR(tz);
    DO_REPR(title);
    DO_REPR(description);
    DO_REPR(examples);
    DO_REPR(deprecated);
    DO_REPR(json_schema_extra);
    DO_REPR(alias);
    DO_REPR(dump);
    DO_REPR(validate);
#undef DO_REPR
    if (!strbuilder_extend_literal(&builder, ")")) goto error;
    return strbuilder_build(&builder);
error:
    strbuilder_reset(&builder);
    return NULL;
}

static PyObject *
Field_rich_repr(PyObject *py_self, PyObject *args) {
    Field *self = (Field *)py_self;
    PyObject *out = PyList_New(0);
    if (out == NULL) goto error;
#define DO_REPR(field) do { \
    if (self->field != NULL) { \
        PyObject *part = Py_BuildValue("(UO)", #field, self->field); \
        if (part == NULL) goto error;\
        int ret = PyList_Append(out, part);\
        Py_DECREF(part);\
        if (ret < 0) goto error;\
    } } while(0)
    DO_REPR(gt);
    DO_REPR(ge);
    DO_REPR(lt);
    DO_REPR(le);
    DO_REPR(multiple_of);
    DO_REPR(pattern);
    DO_REPR(min_length);
    DO_REPR(max_length);
    DO_REPR(tz);
    DO_REPR(title);
    DO_REPR(description);
    DO_REPR(examples);
    DO_REPR(deprecated);
    DO_REPR(json_schema_extra);
    DO_REPR(alias);
    DO_REPR(dump);
    DO_REPR(validate);
#undef DO_REPR
    return out;
error:
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
Field_richcompare(Field *self, PyObject *py_other, int op) {
    int equal = 1;
    PyObject *out;

    if (Py_TYPE(py_other) != &Field_Type) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    if (!(op == Py_EQ || op == Py_NE)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    Field *other = (Field *)py_other;

    /* Only need to loop if self is not other */
    if (MS_LIKELY(self != other)) {
#define DO_COMPARE(field) do { \
        equal = _meta_richcompare_part(self->field, other->field); \
        if (equal < 0) return NULL; \
        if (!equal) goto done; \
    } while (0)
        DO_COMPARE(gt);
        DO_COMPARE(ge);
        DO_COMPARE(lt);
        DO_COMPARE(le);
        DO_COMPARE(multiple_of);
        DO_COMPARE(pattern);
        DO_COMPARE(min_length);
        DO_COMPARE(max_length);
        DO_COMPARE(tz);
        DO_COMPARE(title);
        DO_COMPARE(description);
        DO_COMPARE(examples);
        DO_COMPARE(deprecated);
        DO_COMPARE(json_schema_extra);
        DO_COMPARE(alias);
        DO_COMPARE(dump);
        DO_COMPARE(validate);
    }
#undef DO_COMPARE
done:
    if (op == Py_EQ) {
        out = equal ? Py_True : Py_False;
    }
    else {
        out = (!equal) ? Py_True : Py_False;
    }
    Py_INCREF(out);
    return out;
}

static Py_hash_t
Field_hash(Field *self) {
    Py_ssize_t nfields = 0;
    Py_uhash_t acc = MS_HASH_XXPRIME_5;

#define DO_HASH(field) \
    if (self->field != NULL) { \
        Py_uhash_t lane = PyObject_Hash(self->field); \
        if (lane == (Py_uhash_t)-1) return -1; \
        acc += lane * MS_HASH_XXPRIME_2; \
        acc = MS_HASH_XXROTATE(acc); \
        acc *= MS_HASH_XXPRIME_1; \
        nfields += 1; \
    }
    DO_HASH(gt);
    DO_HASH(ge);
    DO_HASH(lt);
    DO_HASH(le);
    DO_HASH(multiple_of);
    DO_HASH(pattern);
    DO_HASH(min_length);
    DO_HASH(max_length);
    DO_HASH(tz);
    DO_HASH(title);
    DO_HASH(description);
    DO_HASH(deprecated);
    /* Leave out examples & json_schema_extra, since they could be unhashable */
    DO_HASH(alias);
    DO_HASH(dump);
    DO_HASH(validate);
#undef DO_HASH
    acc += nfields ^ (MS_HASH_XXPRIME_5 ^ 3527539UL);
    return (acc == (Py_uhash_t)-1) ?  1546275796 : acc;
}

static PyMethodDef Field_methods[] = {
    {"__rich_repr__", Field_rich_repr, METH_NOARGS, "rich repr"},
    {NULL, NULL},
};

static int
Field_traverse(Field *self, visitproc visit, void *arg)
{
    Py_VISIT(self->gt);
    Py_VISIT(self->ge);
    Py_VISIT(self->lt);
    Py_VISIT(self->le);
    Py_VISIT(self->multiple_of);
    Py_VISIT(self->pattern);
    Py_VISIT(self->regex);
    Py_VISIT(self->min_length);
    Py_VISIT(self->max_length);
    Py_VISIT(self->tz);
    Py_VISIT(self->title);
    Py_VISIT(self->description);
    Py_VISIT(self->examples);
    Py_VISIT(self->deprecated);
    Py_VISIT(self->json_schema_extra);
    Py_VISIT(self->alias);
    Py_VISIT(self->dump);
    Py_VISIT(self->validate);
    return 0;
}

static int
Field_clear(Field *self)
{
    Py_CLEAR(self->gt);
    Py_CLEAR(self->ge);
    Py_CLEAR(self->lt);
    Py_CLEAR(self->le);
    Py_CLEAR(self->multiple_of);
    Py_CLEAR(self->pattern);
    Py_CLEAR(self->regex);
    Py_CLEAR(self->min_length);
    Py_CLEAR(self->max_length);
    Py_CLEAR(self->tz);
    Py_CLEAR(self->title);
    Py_CLEAR(self->description);
    Py_CLEAR(self->examples);
    Py_CLEAR(self->deprecated);
    Py_CLEAR(self->json_schema_extra);
    Py_CLEAR(self->alias);
    Py_CLEAR(self->dump);
    Py_CLEAR(self->validate);
    return 0;
}

static void
Field_dealloc(Field *self)
{
    PyObject_GC_UnTrack(self);
    Field_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Field_members[] = {
    {"gt", T_OBJECT, offsetof(Field, gt), READONLY, NULL},
    {"ge", T_OBJECT, offsetof(Field, ge), READONLY, NULL},
    {"lt", T_OBJECT, offsetof(Field, lt), READONLY, NULL},
    {"le", T_OBJECT, offsetof(Field, le), READONLY, NULL},
    {"multiple_of", T_OBJECT, offsetof(Field, multiple_of), READONLY, NULL},
    {"pattern", T_OBJECT, offsetof(Field, pattern), READONLY, NULL},
    {"min_length", T_OBJECT, offsetof(Field, min_length), READONLY, NULL},
    {"max_length", T_OBJECT, offsetof(Field, max_length), READONLY, NULL},
    {"tz", T_OBJECT, offsetof(Field, tz), READONLY, NULL},
    {"title", T_OBJECT, offsetof(Field, title), READONLY, NULL},
    {"description", T_OBJECT, offsetof(Field, description), READONLY, NULL},
    {"examples", T_OBJECT, offsetof(Field, examples), READONLY, NULL},
    {"deprecated", T_OBJECT, offsetof(Field, deprecated), READONLY, NULL},
    {"json_schema_extra", T_OBJECT, offsetof(Field, json_schema_extra), READONLY, NULL},
    {"alias", T_OBJECT, offsetof(Field, alias), READONLY,
     "An alternative name to use when encoding/decoding this field"},
    {"dump", T_OBJECT, offsetof(Field, dump), READONLY,
     "A callable converting a custom type value to a serializable value"},
    {"validate", T_OBJECT, offsetof(Field, validate), READONLY,
     "A callable converting a serializable value to a custom type value"},
    {NULL},
};

static PyTypeObject Field_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.Field",
    .tp_doc = Field__doc__,
    .tp_basicsize = sizeof(Field),
    .tp_itemsize = 0,
    .tp_new = Field_new,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)Field_clear,
    .tp_traverse = (traverseproc)Field_traverse,
    .tp_dealloc = (destructor) Field_dealloc,
    .tp_repr = (reprfunc)Field_repr,
    .tp_richcompare = (richcmpfunc)Field_richcompare,
    .tp_hash = (hashfunc)Field_hash,
    .tp_methods = Field_methods,
    .tp_members = Field_members,
};

/*************************************************************************
 * AssocList & order handling                                            *
 *************************************************************************/

static int
parse_sort_keys_arg(PyObject *sort_keys, bool *out) {
    if (sort_keys == NULL || sort_keys == Py_None) {
        *out = false;
        return 0;
    }
    int truth = PyObject_IsTrue(sort_keys);
    if (truth < 0) {
        return -1;
    }
    *out = truth != 0;
    return 0;
}


#define ASSOCLIST_SORT_CUTOFF 16

typedef struct {
    const char *key;
    Py_ssize_t key_size;
    PyObject *key_obj;  /* owned ref keeping `key`'s utf8 buffer alive; NULL for CStr keys */
    PyObject *val;      /* owned ref */
} AssocItem;

typedef struct {
    Py_ssize_t size;
    AssocItem items[];
} AssocList;

static AssocList *
AssocList_New(Py_ssize_t capacity) {
    AssocList *out = PyMem_Malloc(sizeof(AssocList) + capacity * sizeof(AssocItem));
    if (out == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    out->size = 0;
    return out;
}

static void
AssocList_Free(AssocList *list) {
    if (list != NULL) {
        for (Py_ssize_t i = 0; i < list->size; i++) {
            Py_XDECREF(list->items[i].key_obj);
            Py_XDECREF(list->items[i].val);
        }
    }
    PyMem_Free(list);
}

static int
AssocList_Append(AssocList *list, PyObject *key, PyObject *val) {
    Py_ssize_t key_size;
    const char* key_buf = unicode_str_and_size(key, &key_size);
    if (key_buf == NULL) return -1;

    Py_INCREF(key);
    Py_INCREF(val);
    list->items[list->size].key = key_buf;
    list->items[list->size].key_size = key_size;
    list->items[list->size].key_obj = key;
    list->items[list->size].val = val;
    list->size++;
    return 0;
}

static AssocList *
AssocList_FromDict(PyObject *dict) {
    PyObject *key, *val;
    Py_ssize_t pos = 0;
    int err = 0;
    AssocList *out;
    Py_BEGIN_CRITICAL_SECTION(dict);
    /* Snapshot the size while holding the critical section so a concurrent
     * mutation can't change the iteration count between allocation and use. */
    out = AssocList_New(PyDict_GET_SIZE(dict));
    if (out != NULL) {
        while (PyDict_Next(dict, &pos, &key, &val)) {
            if (!PyUnicode_Check(key)) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "Only dicts with str keys are supported when `order` is not `None`"
                );
                err = 1;
                break;
            }
            if (AssocList_Append(out, key, val) < 0) {
                err = 1;
                break;
            }
        }
    }
    Py_END_CRITICAL_SECTION();
    if (out == NULL) return NULL;
    if (!err) return out;
    AssocList_Free(out);
    return NULL;
}

static inline int
_AssocItem_lt(AssocItem *left, AssocItem *right) {
    int left_shorter = left->key_size < right->key_size;
    int cmp = memcmp(
        left->key, right->key, (left_shorter) ? left->key_size : right->key_size
    );
    return (cmp < 0) || ((cmp == 0) & left_shorter);
}

static Py_ssize_t
_AssocList_sort_partition(
    AssocList* list, Py_ssize_t lo, Py_ssize_t hi, AssocItem *pivot
) {
    Py_ssize_t i = lo - 1;
    Py_ssize_t j = hi + 1;
    while (true) {
        while (_AssocItem_lt(pivot, &(list->items[--j])));
        while (_AssocItem_lt(&(list->items[++i]), pivot));
        if (i < j) {
            AssocItem tmp = list->items[i];
            list->items[i] = list->items[j];
            list->items[j] = tmp;
        }
        else {
            return j;
        }
    }
}

static void
_AssocList_sort_inner(AssocList* list, Py_ssize_t lo, Py_ssize_t hi) {
    while (lo + ASSOCLIST_SORT_CUTOFF < hi) {
        AssocItem *v1 = &(list->items[lo]);
        AssocItem *v2 = &(list->items[hi]);
        AssocItem *v3 = &(list->items[(lo + hi)/2]);
        AssocItem pivot;

        if (_AssocItem_lt(v1, v2)) {
            if (_AssocItem_lt(v3, v1)) {
                pivot = *v1;
            }
            else if (_AssocItem_lt(v2, v3)) {
                pivot = *v2;
            }
            else {
                pivot = *v3;
            }
        }
        else {
            if (_AssocItem_lt(v3, v2)) {
                pivot = *v2;
            }
            else if (_AssocItem_lt(v1, v3)) {
                pivot = *v1;
            }
            else {
                pivot = *v3;
            }
        }

        Py_ssize_t partition = _AssocList_sort_partition(list, lo, hi, &pivot);
        _AssocList_sort_inner(list, lo, partition);
        lo = partition + 1;
    }
}

static void
AssocList_Sort(AssocList* list) {
    if (list->size > ASSOCLIST_SORT_CUTOFF) {
        _AssocList_sort_inner(list, 0, list->size - 1);
    }

    for (Py_ssize_t i = 1; i < list->size; i++) {
        AssocItem val = list->items[i];
        Py_ssize_t j = i;
        while (j > 0 && _AssocItem_lt(&val, &(list->items[j - 1]))) {
            list->items[j] = list->items[j - 1];
            --j;
        }
        list->items[j] = val;
    }
}

/*************************************************************************
 * Struct, PathNode, and TypeNode Types                                  *
 *************************************************************************/

/* Types */
#define MS_TYPE_ANY                 (1ull << 0)
#define MS_TYPE_NONE                (1ull << 1)
#define MS_TYPE_BOOL                (1ull << 2)
#define MS_TYPE_INT                 (1ull << 3)
#define MS_TYPE_FLOAT               (1ull << 4)
#define MS_TYPE_STR                 (1ull << 5)
#define MS_TYPE_BYTES               (1ull << 6)
#define MS_TYPE_BYTEARRAY           (1ull << 7)
#define MS_TYPE_MEMORYVIEW          (1ull << 8)
#define MS_TYPE_DATETIME            (1ull << 9)
#define MS_TYPE_DATE                (1ull << 10)
#define MS_TYPE_TIME                (1ull << 11)
#define MS_TYPE_TIMEDELTA           (1ull << 12)
#define MS_TYPE_UUID                (1ull << 13)
#define MS_TYPE_DECIMAL             (1ull << 14)
#define MS_TYPE_STRUCT              (1ull << 16)
#define MS_TYPE_STRUCT_ARRAY        (1ull << 17)
#define MS_TYPE_STRUCT_UNION        (1ull << 18)
#define MS_TYPE_STRUCT_ARRAY_UNION  (1ull << 19)
#define MS_TYPE_ENUM                (1ull << 20)
#define MS_TYPE_INTENUM             (1ull << 21)
#define MS_TYPE_CUSTOM              (1ull << 22)
#define MS_TYPE_CUSTOM_GENERIC      (1ull << 23)
#define MS_TYPE_DICT                ((1ull << 24) | (1ull << 25))
#define MS_TYPE_LIST                (1ull << 26)
#define MS_TYPE_SET                 (1ull << 27)
#define MS_TYPE_FROZENSET           (1ull << 28)
#define MS_TYPE_VARTUPLE            (1ull << 29)
#define MS_TYPE_FIXTUPLE            (1ull << 30)
#define MS_TYPE_INTLITERAL          (1ull << 31)
#define MS_TYPE_STRLITERAL          (1ull << 32)
#define MS_TYPE_TYPEDDICT           (1ull << 33)
#define MS_TYPE_DATACLASS           (1ull << 34)
#define MS_TYPE_NAMEDTUPLE          (1ull << 35)
#define MS_TYPE_BOOLLITERAL_TRUE    (1ull << 36)
#define MS_TYPE_BOOLLITERAL_FALSE   (1ull << 37)
// Despite the fact that `frozendict` was added in 3.15,
// this type is always defined for order consistency:
#define MS_TYPE_FROZENDICT          ((1ull << 38) | (1ull << 39))

/* Aliases for commonly used types */
#if PY315_PLUS
#define MS_ANY_DICT                 (MS_TYPE_DICT | MS_TYPE_FROZENDICT)
#else
// `frozendict` was added in 3.15, before that we can ignore this type.
#define MS_ANY_DICT                 MS_TYPE_DICT
#endif

/* Constraints */
#define MS_CONSTR_INT_MIN           (1ull << 42)
#define MS_CONSTR_INT_MAX           (1ull << 43)
#define MS_CONSTR_INT_MULTIPLE_OF   (1ull << 44)
#define MS_CONSTR_FLOAT_GT          (1ull << 45)
#define MS_CONSTR_FLOAT_GE          (1ull << 46)
#define MS_CONSTR_FLOAT_LT          (1ull << 47)
#define MS_CONSTR_FLOAT_LE          (1ull << 48)
#define MS_CONSTR_FLOAT_MULTIPLE_OF (1ull << 49)
#define MS_CONSTR_STR_REGEX         (1ull << 50)
#define MS_CONSTR_STR_MIN_LENGTH    (1ull << 51)
#define MS_CONSTR_STR_MAX_LENGTH    (1ull << 52)
#define MS_CONSTR_BYTES_MIN_LENGTH  (1ull << 53)
#define MS_CONSTR_BYTES_MAX_LENGTH  (1ull << 54)
#define MS_CONSTR_ARRAY_MIN_LENGTH  (1ull << 55)
#define MS_CONSTR_ARRAY_MAX_LENGTH  (1ull << 56)
#define MS_CONSTR_MAP_MIN_LENGTH    (1ull << 57)
#define MS_CONSTR_MAP_MAX_LENGTH    (1ull << 58)
#define MS_CONSTR_TZ_AWARE          (1ull << 59)
#define MS_CONSTR_TZ_NAIVE          (1ull << 60)
#define MS_CONSTR_CODEC             (1ull << 61)
/* Extra flag bit, used by TypedDict/dataclass implementations */
#define MS_EXTRA_FLAG               (1ull << 63)

/* A TypeNode encodes information about all types at the same hierarchy in the
 * type tree. They can encode both single types (`int`) and unions of types
 * (`int | float | list[int]`). The encoding is optimized for the common case
 * of simple scalar types (or unions of these types) with no constraints -
 * these values only require a single uint64_t. More complicated types require
 * extra *details* (`TypeDetail` objects) stored in a variable length array.
 *
 * The encoding is *compressed* - only fields that are set are stored. To know
 * which fields are set, a bitmask of `types` is used, masking both the types
 * and constraints set on the node.
 *
 * The order these details are stored is consistent, allowing the offset of a
 * field to be computed using an efficient bitmask and popcount.
 *
 * The order is documented below:
 *
 * O | STRUCT | STRUCT_ARRAY | STRUCT_UNION | STRUCT_ARRAY_UNION | CUSTOM |
 * O | INTENUM | INTLITERAL |
 * O | ENUM | STRLITERAL |
 * O | TYPEDDICT | DATACLASS |
 * O | NAMEDTUPLE |
 * O | STR_REGEX |
 * T | DICT [key, value] | FROZENDICT [key, value] |
 * T | LIST | SET | FROZENSET | VARTUPLE |
 * I | INT_MIN |
 * I | INT_MAX |
 * I | INT_MULTIPLE_OF |
 * F | FLOAT_GT | FLOAT_GE |
 * F | FLOAT_LT | FLOAT_LE |
 * F | FLOAT_MULTIPLE_OF |
 * S | STR_MIN_LENGTH |
 * S | STR_MAX_LENGTH |
 * S | BYTES_MIN_LENGTH |
 * S | BYTES_MAX_LENGTH |
 * S | ARRAY_MIN_LENGTH |
 * S | ARRAY_MAX_LENGTH |
 * S | MAP_MIN_LENGTH |
 * S | MAP_MAX_LENGTH |
 * C | CODEC |
 * T | FIXTUPLE [size, types ...]
 * */

#define SLOT_00 ( \
    MS_TYPE_STRUCT | MS_TYPE_STRUCT_ARRAY | \
    MS_TYPE_STRUCT_UNION | MS_TYPE_STRUCT_ARRAY_UNION | \
    MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC \
)
#define SLOT_01 (MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)
#define SLOT_02 (MS_TYPE_ENUM | MS_TYPE_STRLITERAL)
#define SLOT_03 (MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS)
#define SLOT_04 MS_TYPE_NAMEDTUPLE
#define SLOT_05 MS_CONSTR_STR_REGEX
#define SLOT_06 MS_ANY_DICT
#define SLOT_07 (MS_TYPE_LIST | MS_TYPE_VARTUPLE | MS_TYPE_SET | MS_TYPE_FROZENSET)
#define SLOT_08 MS_CONSTR_INT_MIN
#define SLOT_09 MS_CONSTR_INT_MAX
#define SLOT_10 MS_CONSTR_INT_MULTIPLE_OF
#define SLOT_11 (MS_CONSTR_FLOAT_GE | MS_CONSTR_FLOAT_GT)
#define SLOT_12 (MS_CONSTR_FLOAT_LE | MS_CONSTR_FLOAT_LT)
#define SLOT_13 MS_CONSTR_FLOAT_MULTIPLE_OF
#define SLOT_14 MS_CONSTR_STR_MIN_LENGTH
#define SLOT_15 MS_CONSTR_STR_MAX_LENGTH
#define SLOT_16 MS_CONSTR_BYTES_MIN_LENGTH
#define SLOT_17 MS_CONSTR_BYTES_MAX_LENGTH
#define SLOT_18 MS_CONSTR_ARRAY_MIN_LENGTH
#define SLOT_19 MS_CONSTR_ARRAY_MAX_LENGTH
#define SLOT_20 MS_CONSTR_MAP_MIN_LENGTH
#define SLOT_21 MS_CONSTR_MAP_MAX_LENGTH

/* Common groups */
#define MS_INT_CONSTRS (SLOT_08 | SLOT_09 | SLOT_10)
#define MS_FLOAT_CONSTRS (SLOT_11 | SLOT_12 | SLOT_13)
#define MS_STR_CONSTRS (SLOT_05 | SLOT_14 | SLOT_15)
#define MS_BYTES_CONSTRS (SLOT_16 | SLOT_17)
#define MS_ARRAY_CONSTRS (SLOT_18 | SLOT_19)
#define MS_MAP_CONSTRS (SLOT_20 | SLOT_21)
#define MS_TIME_CONSTRS (MS_CONSTR_TZ_AWARE | MS_CONSTR_TZ_NAIVE)

typedef union TypeDetail {
    int64_t i64;
    double f64;
    Py_ssize_t py_ssize_t;
    void *pointer;
} TypeDetail;

typedef struct TypeNode {
    uint64_t types;
    TypeDetail details[];
} TypeNode;

/* A simple extension of TypeNode to allow for static allocation */
typedef struct {
    uint64_t types;
    TypeDetail details[1];
} TypeNodeSimple;

typedef struct {
    PyObject_HEAD
    PyObject *int_lookup;
    PyObject *str_lookup;
    bool literal_none;
    bool literal_bool_true;
    bool literal_bool_false;
} LiteralInfo;

typedef struct {
    PyObject *key;
    TypeNode *type;
} TypedDictField;

typedef struct {
    PyObject_VAR_HEAD
    Py_ssize_t nrequired;
#ifdef Py_GIL_DISABLED
    _Atomic(uint8_t) initialized;
#endif
    TypedDictField fields[];
} TypedDictInfo;

typedef struct {
    PyObject *key;
    TypeNode *type;
} DataclassField;

typedef struct {
    PyObject_VAR_HEAD
    PyObject *class;
    PyObject *pre_init;
    PyObject *post_init;
    PyObject *defaults;
#ifdef Py_GIL_DISABLED
    _Atomic(uint8_t) initialized;
#endif
    DataclassField fields[];
} DataclassInfo;

typedef struct {
    PyObject_VAR_HEAD
    PyObject *class;
    PyObject *defaults;
#ifdef Py_GIL_DISABLED
    _Atomic(uint8_t) initialized;
#endif
    TypeNode *types[];
} NamedTupleInfo;

struct StructInfo;

typedef struct {
    const char *buf;
    Py_ssize_t size;
} MS_StrView;

typedef struct {
    PyHeapTypeObject base;
    PyObject *struct_fields;
    PyObject *struct_defaults;
    Py_ssize_t *struct_offsets;
    PyObject *struct_encode_fields;
    MS_StrView *struct_encode_keys;   /* raw UTF-8 views of encode fields, NULL if none */
    MS_StrView struct_tag_field_view; /* raw UTF-8 view of tag field, valid if tag_field set */
    PyObject *struct_module;          /* owning structtype module, or NULL */
    struct StructInfo *struct_info;
    Py_ssize_t nkwonly;
    Py_ssize_t n_trailing_defaults;
    PyObject *struct_tag_field;  /* str or NULL */
    PyObject *struct_tag_value;  /* str or NULL */
    PyObject *struct_tag;        /* True, str, or NULL */
    PyObject *match_args;
    PyObject *rename;
    PyObject *post_init;
    PyObject *struct_field_codecs;  /* tuple of per-field {type: dump} dicts, or NULL */
    Py_ssize_t hash_offset;  /* 0 for no caching, otherwise offset */
    int8_t frozen;
    int8_t order;
    int8_t eq;
    int8_t repr_omit_defaults;
    int8_t array_like;
    int8_t gc;
    int8_t omit_defaults;
    int8_t forbid_unknown_fields;
    int8_t validate_on_init;
} StructMetaObject;

typedef struct StructInfo {
    PyObject_VAR_HEAD
    StructMetaObject *class;
#ifdef Py_GIL_DISABLED
    _Atomic(uint8_t) initialized;
#endif
    TypeNode *types[];
} StructInfo;

typedef struct {
    PyObject_HEAD
    StructMetaObject *st_type;
} StructConfig;

static PyTypeObject LiteralInfo_Type;
static PyTypeObject TypedDictInfo_Type;
static PyTypeObject DataclassInfo_Type;
static PyTypeObject NamedTupleInfo_Type;
static PyTypeObject StructInfo_Type;
static PyTypeObject StructMetaType;
static TypeNode* TypeNode_Convert(PyObject *type);
static PyObject* StructInfo_Convert(PyObject*);
static PyObject* TypedDictInfo_Convert(PyObject*);
static PyObject* DataclassInfo_Convert(PyObject*);
static PyObject* NamedTupleInfo_Convert(PyObject*);

#define StructMeta_GET_FIELDS(s) (((StructMetaObject *)(s))->struct_fields)
#define StructMeta_GET_NFIELDS(s) (PyTuple_GET_SIZE((((StructMetaObject *)(s))->struct_fields)))
#define StructMeta_GET_DEFAULTS(s) (((StructMetaObject *)(s))->struct_defaults)
#define StructMeta_GET_OFFSETS(s) (((StructMetaObject *)(s))->struct_offsets)

#define OPT_UNSET -1
#define OPT_FALSE 0
#define OPT_TRUE 1
#define STRUCT_MERGE_OPTIONS(opt1, opt2) (((opt2) != OPT_UNSET) ? (opt2) : (opt1))

static MS_NOINLINE int
_ms_is_struct_meta_scan(PyTypeObject *mt) {
    /* Common path: scan mt->tp_mro for StructMeta without a function call.
     * This logic is adapted from the CPython implementation:
     * https://github.com/python/cpython/blob/26b7df2430cd5a9ee772bfa6ee03a73bd0b11619/Objects/typeobject.c#L2890-L2919 */
    PyObject *mro = mt->tp_mro;
    if (MS_LIKELY(mro != NULL)) {
        /* Skip index 0 since we already checked exact equality. */
        Py_ssize_t n = PyTuple_GET_SIZE(mro);
        for (Py_ssize_t i = 1; i < n; i++) {
            if (PyTuple_GET_ITEM(mro, i) == (PyObject *)&StructMetaType) {
                return 1;
            }
        }
        return 0;
    }

    /* Very rare during type construction: use CPython's base-chain/MRO logic. */
    return PyType_IsSubtype(mt, &StructMetaType);
}

static MS_INLINE int
ms_is_struct_meta(PyTypeObject *mt) {
    /* Checks whether `mt` is StructMeta or a subclass thereof; first tries an exact
     * metaclass pointer match, otherwise walks the metaclass inheritance chain
     * to determine if `mt` derives from StructMeta. */
    if (MS_LIKELY(mt == &StructMetaType)) {
        return 1;
    }

    return _ms_is_struct_meta_scan(mt);
}

static MS_INLINE int
ms_is_struct_type(PyTypeObject *t) {
    /* Checks whether the metaclass of type `t` is StructMeta or a subclass thereof. */
    return ms_is_struct_meta(Py_TYPE((PyObject *)t));
}

static MS_INLINE int
ms_is_struct_cls(PyObject *o) {
    /* Checks whether the metaclass of class object `o` is StructMeta or a subclass thereof.
     * Expects `o` to be a type/class object. */
    return ms_is_struct_meta(Py_TYPE(o));
}

static MS_INLINE int
ms_is_struct_inst(PyObject *o) {
    /* Checks whether the metaclass of `type(o)` is StructMeta or a subclass thereof. */
    return ms_is_struct_meta(Py_TYPE((PyObject *)Py_TYPE(o)));
}

static MS_INLINE StructInfo *
TypeNode_get_struct_info(TypeNode *type) {
    /* Struct types are always first */
    StructInfo *info = type->details[0].pointer;
#ifdef Py_GIL_DISABLED
    if (atomic_load(&info->initialized)) {
        return info;
    }
    Py_BEGIN_ALLOW_THREADS
    /* wait for the StructInfo to be fully initialized by other thread */
    while (!atomic_load(&info->initialized)) {
    }
    Py_END_ALLOW_THREADS
#endif
    return info;
}

static MS_INLINE Lookup *
TypeNode_get_struct_union(TypeNode *type) {
    /* Struct union types are always first */
    return type->details[0].pointer;
}

static MS_INLINE PyObject *
TypeNode_get_custom(TypeNode *type) {
    /* Custom types can't be mixed with anything */
    return type->details[0].pointer;
}

static MS_INLINE PyObject *
TypeNode_get_codec(TypeNode *type) {
    /* Codecs are only valid on custom types, which occupy the first detail
     * slot; the codec (a Field*) is therefore always the next one. */
    return type->details[1].pointer;
}

static MS_INLINE IntLookup *
TypeNode_get_int_enum_or_literal(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & SLOT_00);
    return type->details[i].pointer;
}

static MS_INLINE StrLookup *
TypeNode_get_str_enum_or_literal(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (SLOT_00 | SLOT_01));
    return type->details[i].pointer;
}

static MS_INLINE TypedDictInfo *
TypeNode_get_typeddict_info(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (SLOT_00 | SLOT_01 | SLOT_02));
    TypedDictInfo *info = type->details[i].pointer;
#ifdef Py_GIL_DISABLED
    if (atomic_load(&info->initialized)) {
        return info;
    }
    Py_BEGIN_ALLOW_THREADS
    /* wait for the TypedDictInfo to be fully initialized by other thread */
    while (!atomic_load(&info->initialized)) {
    }
    Py_END_ALLOW_THREADS
#endif
    return info;
}

static MS_INLINE DataclassInfo *
TypeNode_get_dataclass_info(TypeNode *type) {
    Py_ssize_t i = ms_popcount(type->types & (SLOT_00 | SLOT_01 | SLOT_02));
    DataclassInfo *info = type->details[i].pointer;
#ifdef Py_GIL_DISABLED
    if (atomic_load(&info->initialized)) {
        return info;
    }
    Py_BEGIN_ALLOW_THREADS
    /* wait for the DataclassInfo to be fully initialized by other thread */
    while (!atomic_load(&info->initialized)) {
    }
    Py_END_ALLOW_THREADS
#endif
    return info;
}

static MS_INLINE NamedTupleInfo *
TypeNode_get_namedtuple_info(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03
        )
    );
    NamedTupleInfo *info = type->details[i].pointer;
#ifdef Py_GIL_DISABLED
    if (atomic_load(&info->initialized)) {
        return info;
    }
    Py_BEGIN_ALLOW_THREADS
    /* wait for the NamedTupleInfo to be fully initialized by other thread */
    while (!atomic_load(&info->initialized)) {
    }
    Py_END_ALLOW_THREADS
#endif
    return info;
}

static MS_INLINE PyObject *
TypeNode_get_constr_str_regex(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04
        )
    );
    return type->details[i].pointer;
}

static MS_INLINE void
TypeNode_get_dict(TypeNode *type, TypeNode **key, TypeNode **val) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05
        )
    );
    *key = type->details[i].pointer;
    *val = type->details[i + 1].pointer;
}

static MS_INLINE TypeNode *
TypeNode_get_array(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06
        )
    );
    return type->details[i].pointer;
}

static MS_INLINE int64_t
TypeNode_get_constr_int_min(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07
        )
    );
    return type->details[i].i64;
}

static MS_INLINE int64_t
TypeNode_get_constr_int_max(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08
        )
    );
    return type->details[i].i64;
}

static MS_INLINE int64_t
TypeNode_get_constr_int_multiple_of(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09
        )
    );
    return type->details[i].i64;
}

static MS_INLINE double
TypeNode_get_constr_float_min(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10
        )
    );
    return type->details[i].f64;
}

static MS_INLINE double
TypeNode_get_constr_float_max(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11
        )
    );
    return type->details[i].f64;
}

static MS_INLINE double
TypeNode_get_constr_float_multiple_of(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12
        )
    );
    return type->details[i].f64;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_str_min_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_str_max_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_bytes_min_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_bytes_max_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_array_min_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_array_max_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17 | SLOT_18
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_map_min_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17 | SLOT_18 | SLOT_19
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE Py_ssize_t
TypeNode_get_constr_map_max_length(TypeNode *type) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17 | SLOT_18 | SLOT_19 | SLOT_20
        )
    );
    return type->details[i].py_ssize_t;
}

static MS_INLINE void
TypeNode_get_fixtuple(TypeNode *type, Py_ssize_t *offset, Py_ssize_t *size) {
    Py_ssize_t i = ms_popcount(
        type->types & (
            SLOT_00 | SLOT_01 | SLOT_02 | SLOT_03 | SLOT_04 | SLOT_05 | SLOT_06 | SLOT_07 |
            SLOT_08 | SLOT_09 | SLOT_10 | SLOT_11 | SLOT_12 | SLOT_13 | SLOT_14 | SLOT_15 |
            SLOT_16 | SLOT_17 | SLOT_18 | SLOT_19 | SLOT_20 | SLOT_21
        )
    );
    *size = type->details[i].py_ssize_t;
    *offset = i + 1;
}

static void
TypeNode_get_traverse_ranges(
    TypeNode *type, Py_ssize_t *n_objects, Py_ssize_t *n_typenode,
    Py_ssize_t *fixtuple_offset, Py_ssize_t *fixtuple_size
) {
    Py_ssize_t n_obj = 0, n_type = 0, ft_offset = 0, ft_size = 0;
    /* Custom types cannot share a union with anything except `None` */
    if (type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC)) {
        n_obj = 1;
        if (type->types & MS_CONSTR_CODEC) {
            n_obj += 1;
        }
    }
    else if (!(type->types & MS_TYPE_ANY)) {
        /* Number of pyobject details */
        n_obj = ms_popcount(
            type->types & (
                MS_TYPE_STRUCT | MS_TYPE_STRUCT_UNION |
                MS_TYPE_STRUCT_ARRAY | MS_TYPE_STRUCT_ARRAY_UNION |
                MS_TYPE_INTENUM | MS_TYPE_INTLITERAL |
                MS_TYPE_ENUM | MS_TYPE_STRLITERAL |
                MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS |
                MS_TYPE_NAMEDTUPLE
            )
        );
        /* Number of typenode details */
        n_type = ms_popcount(
            type->types & (
                MS_ANY_DICT |
                MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_FROZENSET | MS_TYPE_VARTUPLE
            )
        );
        if (type->types & MS_TYPE_FIXTUPLE) {
            TypeNode_get_fixtuple(type, &ft_offset, &ft_size);
        }
    }
    *n_objects = n_obj;
    *n_typenode = n_type;
    *fixtuple_offset = ft_offset;
    *fixtuple_size = ft_size;
}

static void
TypeNode_Free(TypeNode *self) {
    if (self == NULL) return;
    Py_ssize_t n_obj, n_typenode, fixtuple_offset, fixtuple_size, i;
    TypeNode_get_traverse_ranges(self, &n_obj, &n_typenode, &fixtuple_offset, &fixtuple_size);

    for (i = 0; i < n_obj; i++) {
        PyObject *obj = (PyObject *)(self->details[i].pointer);
        Py_XDECREF(obj);
    }
    for (i = n_obj; i < (n_obj + n_typenode); i++) {
        TypeNode *node = (TypeNode *)(self->details[i].pointer);
        TypeNode_Free(node);
    }
    for (i = 0; i < fixtuple_size; i++) {
        TypeNode *node = (TypeNode *)(self->details[i + fixtuple_offset].pointer);
        TypeNode_Free(node);
    }
    PyMem_Free(self);
}

static int
TypeNode_traverse(TypeNode *self, visitproc visit, void *arg) {
    if (self == NULL) return 0;
    Py_ssize_t n_obj, n_typenode, fixtuple_offset, fixtuple_size, i;
    TypeNode_get_traverse_ranges(self, &n_obj, &n_typenode, &fixtuple_offset, &fixtuple_size);

    for (i = 0; i < n_obj; i++) {
        PyObject *obj = (PyObject *)(self->details[i].pointer);
        Py_VISIT(obj);
    }
    for (i = n_obj; i < (n_obj + n_typenode); i++) {
        int out;
        TypeNode *node = (TypeNode *)(self->details[i].pointer);
        if ((out = TypeNode_traverse(node, visit, arg)) != 0) return out;
    }
    for (i = 0; i < fixtuple_size; i++) {
        int out;
        TypeNode *node = (TypeNode *)(self->details[i + fixtuple_offset].pointer);
        if ((out = TypeNode_traverse(node, visit, arg)) != 0) return out;
    }
    return 0;
}

static PyObject *
typenode_simple_repr(TypeNode *self) {
    strbuilder builder = {" | ", 3};

    if (self->types & (MS_TYPE_ANY | MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC) || self->types == 0) {
        return PyUnicode_FromString("any");
    }
    if (self->types & (MS_TYPE_BOOL | MS_TYPE_BOOLLITERAL_TRUE | MS_TYPE_BOOLLITERAL_FALSE)) {
        if (!strbuilder_extend_literal(&builder, "bool")) return NULL;
    }
    if (self->types & (MS_TYPE_INT | MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        if (!strbuilder_extend_literal(&builder, "int")) return NULL;
    }
    if (self->types & MS_TYPE_FLOAT) {
        if (!strbuilder_extend_literal(&builder, "float")) return NULL;
    }
    if (self->types & (MS_TYPE_STR | MS_TYPE_ENUM | MS_TYPE_STRLITERAL)) {
        if (!strbuilder_extend_literal(&builder, "str")) return NULL;
    }
    if (self->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY | MS_TYPE_MEMORYVIEW)) {
        if (!strbuilder_extend_literal(&builder, "bytes")) return NULL;
    }
    if (self->types & MS_TYPE_DATETIME) {
        if (!strbuilder_extend_literal(&builder, "datetime")) return NULL;
    }
    if (self->types & MS_TYPE_DATE) {
        if (!strbuilder_extend_literal(&builder, "date")) return NULL;
    }
    if (self->types & MS_TYPE_TIME) {
        if (!strbuilder_extend_literal(&builder, "time")) return NULL;
    }
    if (self->types & MS_TYPE_TIMEDELTA) {
        if (!strbuilder_extend_literal(&builder, "duration")) return NULL;
    }
    if (self->types & MS_TYPE_UUID) {
        if (!strbuilder_extend_literal(&builder, "uuid")) return NULL;
    }
    if (self->types & MS_TYPE_DECIMAL) {
        if (!strbuilder_extend_literal(&builder, "decimal")) return NULL;
    }
    if (self->types & (
            MS_TYPE_STRUCT | MS_TYPE_STRUCT_UNION |
            MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS |
            MS_ANY_DICT
        )
    ) {
        if (!strbuilder_extend_literal(&builder, "object")) return NULL;
    }
    if (
        self->types & (
            MS_TYPE_STRUCT_ARRAY | MS_TYPE_STRUCT_ARRAY_UNION |
            MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_FROZENSET |
            MS_TYPE_VARTUPLE | MS_TYPE_FIXTUPLE | MS_TYPE_NAMEDTUPLE
        )
    ) {
        if (!strbuilder_extend_literal(&builder, "array")) return NULL;
    }
    if (self->types & MS_TYPE_NONE) {
        if (!strbuilder_extend_literal(&builder, "null")) return NULL;
    }

    return strbuilder_build(&builder);
}

typedef struct {
    PyObject *gt;
    PyObject *ge;
    PyObject *lt;
    PyObject *le;
    PyObject *multiple_of;
    PyObject *regex;
    PyObject *min_length;
    PyObject *max_length;
    PyObject *tz;
    PyObject *codec;  /* the Field carrying dump/validate, or NULL */
} Constraints;

typedef struct {
    StructspecState *mod;
    PyObject *context;
    uint64_t types;
    PyObject *struct_obj;
    PyObject *struct_info;
    PyObject *structs_set;
    PyObject *structs_lookup;
    PyObject *intenum_obj;
    PyObject *enum_obj;
    PyObject *custom_obj;
    PyObject *array_el_obj;
    PyObject *dict_key_obj;
    PyObject *dict_val_obj;
    PyObject *typeddict_obj;
    PyObject *dataclass_obj;
    PyObject *namedtuple_obj;
    PyObject *literals;
    PyObject *literal_int_values;
    PyObject *literal_int_lookup;
    PyObject *literal_str_values;
    PyObject *literal_str_lookup;
    bool literal_none;
    bool literal_bool_true;
    bool literal_bool_false;
    /* Constraints */
    int64_t c_int_min;
    int64_t c_int_max;
    int64_t c_int_multiple_of;
    double c_float_min;
    double c_float_max;
    double c_float_multiple_of;
    PyObject *c_str_regex;
    PyObject *codec_obj;  /* Field* with dump/validate, or NULL */
    Py_ssize_t c_str_min_length;
    Py_ssize_t c_str_max_length;
    Py_ssize_t c_bytes_min_length;
    Py_ssize_t c_bytes_max_length;
    Py_ssize_t c_array_min_length;
    Py_ssize_t c_array_max_length;
    Py_ssize_t c_map_min_length;
    Py_ssize_t c_map_max_length;
} TypeNodeCollectState;

static MS_INLINE bool
constraints_is_empty(Constraints *self) {
    return (
        self->gt == NULL &&
        self->ge == NULL &&
        self->lt == NULL &&
        self->le == NULL &&
        self->multiple_of == NULL &&
        self->regex == NULL &&
        self->min_length == NULL &&
        self->max_length == NULL &&
        self->tz == NULL &&
        self->codec == NULL
    );
}

static int
_set_constraint(PyObject *source, PyObject **target, const char *name, PyObject *type) {
    if (source == NULL) return 0;
    if (*target == NULL) {
        *target = source;
        return 0;
    }
    PyErr_Format(
        PyExc_TypeError,
        "Multiple `Field` annotations setting `%s` found, "
        "type `%R` is invalid",
        name, type
    );
    return -1;
}

static int
constraints_update(Constraints *self, Field *meta, PyObject *type) {
#define set_constraint(field) do { \
    if (_set_constraint(meta->field, &(self->field), #field, type) < 0) return -1; \
} while (0)
    set_constraint(gt);
    set_constraint(ge);
    set_constraint(lt);
    set_constraint(le);
    set_constraint(multiple_of);
    set_constraint(regex);
    set_constraint(min_length);
    set_constraint(max_length);
    set_constraint(tz);
    if (meta->dump != NULL || meta->validate != NULL) {
        if (_set_constraint((PyObject *)meta, &(self->codec), "dump", type) < 0) return -1;
    }
    if (self->gt != NULL && self->ge != NULL) {
        PyErr_Format(
            PyExc_TypeError,
            "Cannot set both `gt` and `ge` on the same annotated type, "
            "type `%R` is invalid",
            type
        );
        return -1;
    }
    if (self->lt != NULL && self->le != NULL) {
        PyErr_Format(
            PyExc_TypeError,
            "Cannot set both `lt` and `le` on the same annotated type, "
            "type `%R` is invalid",
            type
        );
        return -1;
    }
    return 0;
#undef set_constraint
}

enum constraint_kind {
    CK_INT = 0,
    CK_FLOAT = 1,
    CK_STR = 2,
    CK_BYTES = 3,
    CK_TIME = 4,
    CK_ARRAY = 5,
    CK_MAP = 6,
    CK_OTHER = 7,
};

static int
err_invalid_constraint(const char *name, const char *kind, PyObject *obj) {
    PyErr_Format(
        PyExc_TypeError,
        "Can only set `%s` on a %s type - type `%R` is invalid",
        name, kind, obj
    );
    return -1;
}

static bool
_constr_as_i64(PyObject *obj, int64_t *target, int offset) {
    int overflow;
    int64_t x = PyLong_AsLongLongAndOverflow(obj, &overflow);
    if (overflow != 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "Integer bounds constraints (`ge`, `le`, ...) that don't fit in an "
            "int64 are currently not supported. If you need this feature, please "
            "open an issue on GitHub"
        );
        return false;
    }
    else if (x == -1 && PyErr_Occurred()) {
        return false;
    }
    /* Do offsets for lt/gt */
    if (offset == -1) {
        if (x == INT64_MIN) {
            PyErr_SetString(PyExc_ValueError, "lt <= -2**63 is not supported");
            return false;
        }
        x -= 1;
    }
    else if (offset == 1) {
        if (x == ((1ULL << 63) - 1)) {
            PyErr_SetString(PyExc_ValueError, "gt >= 2**63 - 1 is not supported");
            return false;
        }
        x += 1;
    }

    *target = x;
    return true;
}

static bool
_constr_as_f64(PyObject *obj, double *target, int offset) {
    /* Use PyFloat_AsDouble to also handle integers */
    double x = PyFloat_AsDouble(obj);
    /* Should never be hit, types already checked */
    if (x == -1.0 && PyErr_Occurred()) return false;
    if (offset == 1) {
        x = nextafter(x, DBL_MAX);
    }
    else if (offset == -1) {
        x = nextafter(x, -DBL_MAX);
    }
    *target = x;
    return true;
}

static bool
_constr_as_py_ssize_t(PyObject *obj, Py_ssize_t *target) {
    Py_ssize_t x = PyLong_AsSsize_t(obj);
    /* Should never be hit, types already checked */
    if (x == -1 && PyErr_Occurred()) return false;
    *target = x;
    return true;
}

static int
typenode_collect_constraints(
    TypeNodeCollectState *state,
    Constraints *constraints,
    enum constraint_kind kind,
    PyObject *obj
) {
    /* If no constraints, do nothing */
    if (constraints == NULL) return 0;
    if (constraints_is_empty(constraints)) return 0;

    /* Codecs are only supported on custom (non-native) types */
    if (constraints->codec != NULL) {
        bool is_custom = (
            state->custom_obj != NULL &&
            (state->types & ~(MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC | MS_TYPE_NONE)) == 0
        );
        if (!is_custom) {
            PyErr_Format(
                PyExc_TypeError,
                "`Field(dump=...)`/`Field(validate=...)` codecs can only be used "
                "on custom types - type `%R` is invalid",
                obj
            );
            return -1;
        }
        state->types |= MS_CONSTR_CODEC;
        Py_INCREF(constraints->codec);
        state->codec_obj = constraints->codec;
    }

    /* Check that the constraints are valid for the corresponding type */
    if (kind != CK_INT && kind != CK_FLOAT) {
        if (constraints->gt != NULL) return err_invalid_constraint("gt", "numeric", obj);
        if (constraints->ge != NULL) return err_invalid_constraint("ge", "numeric", obj);
        if (constraints->lt != NULL) return err_invalid_constraint("lt", "numeric", obj);
        if (constraints->le != NULL) return err_invalid_constraint("le", "numeric", obj);
        if (constraints->multiple_of != NULL) return err_invalid_constraint("multiple_of", "numeric", obj);
    }
    if (kind != CK_STR) {
        if (constraints->regex != NULL) return err_invalid_constraint("pattern", "str", obj);
    }
    if (kind != CK_STR && kind != CK_BYTES && kind != CK_ARRAY && kind != CK_MAP) {
        if (constraints->min_length != NULL) return err_invalid_constraint("min_length", "str, bytes, or collection", obj);
        if (constraints->max_length != NULL) return err_invalid_constraint("max_length", "str, bytes, or collection", obj);
    }
    if (kind != CK_TIME) {
        if (constraints->tz != NULL) return err_invalid_constraint("tz", "datetime or time", obj);
    }

    /* Next attempt to fill in the state. */
    if (kind == CK_INT) {
        if (constraints->gt != NULL) {
            state->types |= MS_CONSTR_INT_MIN;
            if (!_constr_as_i64(constraints->gt, &(state->c_int_min), 1)) return -1;
        }
        else if (constraints->ge != NULL) {
            state->types |= MS_CONSTR_INT_MIN;
            if (!_constr_as_i64(constraints->ge, &(state->c_int_min), 0)) return -1;
        }
        if (constraints->lt != NULL) {
            state->types |= MS_CONSTR_INT_MAX;
            if (!_constr_as_i64(constraints->lt, &(state->c_int_max), -1)) return -1;
        }
        else if (constraints->le != NULL) {
            state->types |= MS_CONSTR_INT_MAX;
            if (!_constr_as_i64(constraints->le, &(state->c_int_max), 0)) return -1;
        }
        if (constraints->multiple_of != NULL) {
            state->types |= MS_CONSTR_INT_MULTIPLE_OF;
            if (!_constr_as_i64(constraints->multiple_of, &(state->c_int_multiple_of), 0)) return -1;
        }
    }
    else if (kind == CK_FLOAT) {
        if (constraints->gt != NULL) {
            state->types |= MS_CONSTR_FLOAT_GT;
            if (!_constr_as_f64(constraints->gt, &(state->c_float_min), 1)) return -1;
        }
        else if (constraints->ge != NULL) {
            state->types |= MS_CONSTR_FLOAT_GE;
            if (!_constr_as_f64(constraints->ge, &(state->c_float_min), 0)) return -1;
        }
        if (constraints->lt != NULL) {
            state->types |= MS_CONSTR_FLOAT_LT;
            if (!_constr_as_f64(constraints->lt, &(state->c_float_max), -1)) return -1;
        }
        else if (constraints->le != NULL) {
            state->types |= MS_CONSTR_FLOAT_LE;
            if (!_constr_as_f64(constraints->le, &(state->c_float_max), 0)) return -1;
        }
        if (constraints->multiple_of != NULL) {
            state->types |= MS_CONSTR_FLOAT_MULTIPLE_OF;
            if (!_constr_as_f64(constraints->multiple_of, &(state->c_float_multiple_of), 0)) return -1;
        }
    }
    else if (kind == CK_STR) {
        if (constraints->regex != NULL) {
            state->types |= MS_CONSTR_STR_REGEX;
            Py_INCREF(constraints->regex);
            state->c_str_regex = constraints->regex;
        }
        if (constraints->min_length != NULL) {
            state->types |= MS_CONSTR_STR_MIN_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->min_length, &(state->c_str_min_length))) return -1;
        }
        if (constraints->max_length != NULL) {
            state->types |= MS_CONSTR_STR_MAX_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->max_length, &(state->c_str_max_length))) return -1;
        }
    }
    else if (kind == CK_BYTES) {
        if (constraints->min_length != NULL) {
            state->types |= MS_CONSTR_BYTES_MIN_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->min_length, &(state->c_bytes_min_length))) return -1;
        }
        if (constraints->max_length != NULL) {
            state->types |= MS_CONSTR_BYTES_MAX_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->max_length, &(state->c_bytes_max_length))) return -1;
        }
    }
    else if (kind == CK_TIME) {
        if (constraints->tz != NULL) {
            if (constraints->tz == Py_True) {
                state->types |= MS_CONSTR_TZ_AWARE;
            }
            else {
                state->types |= MS_CONSTR_TZ_NAIVE;
            }
        }
    }
    else if (kind == CK_ARRAY) {
        if (constraints->min_length != NULL) {
            state->types |= MS_CONSTR_ARRAY_MIN_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->min_length, &(state->c_array_min_length))) return -1;
        }
        if (constraints->max_length != NULL) {
            state->types |= MS_CONSTR_ARRAY_MAX_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->max_length, &(state->c_array_max_length))) return -1;
        }
    }
    else if (kind == CK_MAP) {
        if (constraints->min_length != NULL) {
            state->types |= MS_CONSTR_MAP_MIN_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->min_length, &(state->c_map_min_length))) return -1;
        }
        if (constraints->max_length != NULL) {
            state->types |= MS_CONSTR_MAP_MAX_LENGTH;
            if (!_constr_as_py_ssize_t(constraints->max_length, &(state->c_map_max_length))) return -1;
        }
    }
    return 0;
}

static int typenode_collect_type(TypeNodeCollectState*, PyObject*);

static TypeNode *
typenode_from_collect_state(TypeNodeCollectState *state) {
    Py_ssize_t e_ind, n_extra = 0, fixtuple_size = 0;
    bool has_fixtuple = false;

    n_extra = ms_popcount(
        state->types & (
            MS_TYPE_STRUCT | MS_TYPE_STRUCT_ARRAY |
            MS_TYPE_STRUCT_UNION | MS_TYPE_STRUCT_ARRAY_UNION |
            MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC |
            MS_TYPE_INTENUM | MS_TYPE_INTLITERAL |
            MS_TYPE_ENUM | MS_TYPE_STRLITERAL |
            MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS |
            MS_TYPE_NAMEDTUPLE |
            MS_CONSTR_STR_REGEX |
            MS_ANY_DICT |
            MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_FROZENSET | MS_TYPE_VARTUPLE |
            MS_CONSTR_INT_MIN |
            MS_CONSTR_INT_MAX |
            MS_CONSTR_INT_MULTIPLE_OF |
            MS_CONSTR_FLOAT_GT | MS_CONSTR_FLOAT_GE |
            MS_CONSTR_FLOAT_LT | MS_CONSTR_FLOAT_LE |
            MS_CONSTR_FLOAT_MULTIPLE_OF |
            MS_CONSTR_STR_MIN_LENGTH |
            MS_CONSTR_STR_MAX_LENGTH |
            MS_CONSTR_BYTES_MIN_LENGTH |
            MS_CONSTR_BYTES_MAX_LENGTH |
            MS_CONSTR_ARRAY_MIN_LENGTH |
            MS_CONSTR_ARRAY_MAX_LENGTH |
            MS_CONSTR_MAP_MIN_LENGTH |
            MS_CONSTR_MAP_MAX_LENGTH |
            MS_CONSTR_CODEC
        )
    );
    if (state->types & MS_TYPE_FIXTUPLE) {
        has_fixtuple = true;
        fixtuple_size = PyTuple_GET_SIZE(state->array_el_obj);
        n_extra += fixtuple_size + 1;
    }

    if (n_extra == 0) {
        TypeNode *out = (TypeNode *)PyMem_Malloc(sizeof(TypeNode));
        if (out == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        out->types = state->types;
        return out;
    }

    /* Use calloc so that `out->details` is initialized, easing cleanup on error */
    TypeNode *out = (TypeNode *)PyMem_Calloc(
        1, sizeof(TypeNode) + n_extra * sizeof(TypeDetail)
    );
    if (out == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    out->types = state->types;
    /* Populate `details` fields in order */
    e_ind = 0;
    if (state->custom_obj != NULL) {
        Py_INCREF(state->custom_obj);
        /* Add `Any` to the type node, so the individual decode functions can
         * check for `Any` alone, and only have to handle custom types in one
         * location  (e.g. `mpack_decode`). */
        out->types |= MS_TYPE_ANY;
        out->details[e_ind++].pointer = state->custom_obj;
    }
    if (state->struct_info != NULL) {
        Py_INCREF(state->struct_info);
        out->details[e_ind++].pointer = state->struct_info;
    }
    if (state->structs_lookup != NULL) {
        Py_INCREF(state->structs_lookup);
        out->details[e_ind++].pointer = state->structs_lookup;
    }
    if (state->intenum_obj != NULL) {
        PyObject *member_map = PyObject_GetAttr(state->intenum_obj, state->mod->str__value2member_map_);
        if (member_map == NULL) goto error;
        PyObject *lookup = IntLookup_New(member_map, NULL, state->intenum_obj, false);
        Py_DECREF(member_map);
        if (lookup == NULL) goto error;
        out->details[e_ind++].pointer = lookup;
    }
    if (state->literal_int_lookup != NULL) {
        Py_INCREF(state->literal_int_lookup);
        out->details[e_ind++].pointer = state->literal_int_lookup;
    }
    if (state->enum_obj != NULL) {
        PyObject *member_map = PyObject_GetAttr(state->enum_obj, state->mod->str__value2member_map_);
        if (member_map == NULL) goto error;
        PyObject *lookup = StrLookup_New(member_map, NULL, state->enum_obj, false);
        Py_DECREF(member_map);
        if (lookup == NULL) goto error;
        out->details[e_ind++].pointer = lookup;
    }
    if (state->literal_str_lookup != NULL) {
        Py_INCREF(state->literal_str_lookup);
        out->details[e_ind++].pointer = state->literal_str_lookup;
    }
    if (state->typeddict_obj != NULL) {
        PyObject *info = TypedDictInfo_Convert(state->typeddict_obj);
        if (info == NULL) goto error;
        out->details[e_ind++].pointer = info;
    }
    if (state->dataclass_obj != NULL) {
        PyObject *info = DataclassInfo_Convert(state->dataclass_obj);
        if (info == NULL) goto error;
        out->details[e_ind++].pointer = info;
    }
    if (state->namedtuple_obj != NULL) {
        PyObject *info = NamedTupleInfo_Convert(state->namedtuple_obj);
        if (info == NULL) goto error;
        out->details[e_ind++].pointer = info;
    }
    if (state->types & MS_CONSTR_CODEC) {
        Py_INCREF(state->codec_obj);
        out->details[e_ind++].pointer = state->codec_obj;
    }
    if (state->types & MS_CONSTR_STR_REGEX) {
        Py_INCREF(state->c_str_regex);
        out->details[e_ind++].pointer = state->c_str_regex;
    }
    if (state->dict_key_obj != NULL && state->dict_val_obj != NULL) {
        TypeNode *temp = TypeNode_Convert(state->dict_key_obj);
        if (temp == NULL) goto error;
        out->details[e_ind++].pointer = temp;
        temp = TypeNode_Convert(state->dict_val_obj);
        if (temp == NULL) goto error;
        out->details[e_ind++].pointer = temp;
    }
    if (state->array_el_obj != NULL) {
        if (has_fixtuple) {
            out->details[e_ind++].py_ssize_t = fixtuple_size;

            for (Py_ssize_t i = 0; i < fixtuple_size; i++) {
                TypeNode *temp = TypeNode_Convert(
                    PyTuple_GET_ITEM(state->array_el_obj, i)
                );
                if (temp == NULL) goto error;
                out->details[e_ind++].pointer = temp;
            }
        }
        else {
            TypeNode *temp = TypeNode_Convert(state->array_el_obj);
            if (temp == NULL) goto error;
            out->details[e_ind++].pointer = temp;
        }
    }
    if (state->types & MS_CONSTR_INT_MIN) {
        out->details[e_ind++].i64 = state->c_int_min;
    }
    if (state->types & MS_CONSTR_INT_MAX) {
        out->details[e_ind++].i64 = state->c_int_max;
    }
    if (state->types & MS_CONSTR_INT_MULTIPLE_OF) {
        out->details[e_ind++].i64 = state->c_int_multiple_of;
    }
    if (state->types & (MS_CONSTR_FLOAT_GT | MS_CONSTR_FLOAT_GE)) {
        out->details[e_ind++].f64 = state->c_float_min;
    }
    if (state->types & (MS_CONSTR_FLOAT_LT | MS_CONSTR_FLOAT_LE)) {
        out->details[e_ind++].f64 = state->c_float_max;
    }
    if (state->types & MS_CONSTR_FLOAT_MULTIPLE_OF) {
        out->details[e_ind++].f64 = state->c_float_multiple_of;
    }
    if (state->types & MS_CONSTR_STR_MIN_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_str_min_length;
    }
    if (state->types & MS_CONSTR_STR_MAX_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_str_max_length;
    }
    if (state->types & MS_CONSTR_BYTES_MIN_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_bytes_min_length;
    }
    if (state->types & MS_CONSTR_BYTES_MAX_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_bytes_max_length;
    }
    if (state->types & MS_CONSTR_ARRAY_MIN_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_array_min_length;
    }
    if (state->types & MS_CONSTR_ARRAY_MAX_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_array_max_length;
    }
    if (state->types & MS_CONSTR_MAP_MIN_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_map_min_length;
    }
    if (state->types & MS_CONSTR_MAP_MAX_LENGTH) {
        out->details[e_ind++].py_ssize_t = state->c_map_max_length;
    }
    return (TypeNode *)out;

error:
    TypeNode_Free((TypeNode *)out);
    return NULL;
}

static bool
get_structtype_cache(StructspecState *mod, PyObject *obj, PyTypeObject *type, PyObject **out) {
    PyObject *cached = PyObject_GenericGetAttr(obj, mod->str___structtype_cache__);
    if (cached != NULL) {
        if (Py_TYPE(cached) != type) {
            Py_DECREF(cached);
            PyErr_Format(
                PyExc_RuntimeError,
                "%R.__structtype_cache__ has been overwritten",
                obj
            );
        }
        else {
            *out = cached;
        }
        return true;
    }
    PyErr_Clear();
    return false;
}

static int
typenode_collect_err_unique(TypeNodeCollectState *state, const char *kind) {
    PyErr_Format(
        PyExc_TypeError,
        "Type unions may not contain more than one %s type - "
        "type `%R` is not supported",
        kind,
        state->context
    );
    return -1;
}

static int
typenode_collect_check_invariants(TypeNodeCollectState *state) {
    /* If a custom type is used, this node may only contain that type and `None */
    if (
        state->custom_obj != NULL &&
        state->types & ~(MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC | MS_TYPE_NONE | MS_CONSTR_CODEC)
    ) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions containing a custom type may not contain any "
            "additional types other than `None` - type `%R` is not supported",
            state->context
        );
        return -1;
    }

    /* Ensure at most one array-like type in the union */
    if (ms_popcount(
            state->types & (
                MS_TYPE_STRUCT_ARRAY | MS_TYPE_STRUCT_ARRAY_UNION |
                MS_TYPE_LIST | MS_TYPE_SET | MS_TYPE_FROZENSET |
                MS_TYPE_VARTUPLE | MS_TYPE_FIXTUPLE | MS_TYPE_NAMEDTUPLE
            )
        ) > 1
    ) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain more than one array-like type "
            "(`Struct(array_like=True)`, `list`, `set`, `frozenset`, `tuple`, "
            "`NamedTuple`) - type `%R` is not supported",
            state->context
        );
        return -1;
    }
    /* Ensure at most one dict-like type in the union */
    int ndictlike = ms_popcount(
        state->types & (
            MS_TYPE_STRUCT | MS_TYPE_STRUCT_UNION |
            MS_TYPE_TYPEDDICT | MS_TYPE_DATACLASS
        )
    );
    if (state->types & MS_ANY_DICT) {
        ndictlike++;
    }
    if (ndictlike > 1) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain more than one dict-like type "
            "(`Struct`, `dict`, `TypedDict`, `dataclass`) - type `%R` "
            "is not supported",
            state->context
        );
        return -1;
    }

    /* If int & int literals are both present, drop literals */
    if (state->types & MS_TYPE_INT && state->literal_int_lookup) {
        state->types &= ~MS_TYPE_INTLITERAL;
        Py_CLEAR(state->literal_int_lookup);
    }

    /* If str & str literals are both present, drop literals */
    if (state->types & MS_TYPE_STR && state->literal_str_lookup) {
        state->types &= ~MS_TYPE_STRLITERAL;
        Py_CLEAR(state->literal_str_lookup);
    }

    /* Ensure int-like types don't conflict */
    if (ms_popcount(state->types & (MS_TYPE_INT | MS_TYPE_INTLITERAL | MS_TYPE_INTENUM)) > 1) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain more than one int-like type (`int`, "
            "`Enum`, `Literal[int values]`) - type `%R` is not supported",
            state->context
        );
        return -1;
    }

    /* Ensure str-like types don't conflict */
    if (ms_popcount(
            state->types & (
                MS_TYPE_STR | MS_TYPE_STRLITERAL | MS_TYPE_ENUM |
                MS_TYPE_BYTES | MS_TYPE_BYTEARRAY | MS_TYPE_MEMORYVIEW |
                MS_TYPE_DATETIME | MS_TYPE_DATE | MS_TYPE_TIME |
                MS_TYPE_TIMEDELTA | MS_TYPE_UUID | MS_TYPE_DECIMAL
            )
        ) > 1
    ) {
        PyErr_Format(
            PyExc_TypeError,
            "Type unions may not contain more than one str-like type (`str`, "
            "`Enum`, `Literal[str values]`, `datetime`, `date`, `time`, `timedelta`, "
            "`uuid`, `decimal`, `bytes`, `bytearray`) - type `%R` is not supported",
            state->context
        );
        return -1;
    }
    return 0;
}

static int
typenode_collect_enum(TypeNodeCollectState *state, PyObject *obj) {
    bool is_intenum;

    if (PyType_IsSubtype((PyTypeObject *)obj, &PyLong_Type)) {
        is_intenum = true;
    }
    else if (PyType_IsSubtype((PyTypeObject *)obj, &PyUnicode_Type)) {
        is_intenum = false;
    }
    else {
        PyObject *members = PyObject_GetAttr(obj, state->mod->str__value2member_map_);
        if (members == NULL) return -1;
        if (!PyDict_Check(members)) {
            Py_DECREF(members);
            PyErr_SetString(
                PyExc_RuntimeError, "Expected _value2member_map_ to be a dict"
            );
            return -1;
        }
        /* Traverse _value2member_map_ to determine key type */
        Py_ssize_t pos = 0;
        PyObject *key;
        bool all_ints = true;
        bool all_strs = true;
        while (PyDict_Next(members, &pos, &key, NULL)) {
            all_ints &= PyLong_CheckExact(key);
            all_strs &= PyUnicode_CheckExact(key);
        }
        Py_CLEAR(members);

        if (all_ints) {
            is_intenum = true;
        }
        else if (all_strs) {
            is_intenum = false;
        }
        else {
            PyErr_Format(
                PyExc_TypeError,
                "Enums must contain either all str or all int values - "
                "type `%R` is not supported",
                state->context
            );
            return -1;
        }
    }

    if (is_intenum) {
        if (state->intenum_obj != NULL) {
            return typenode_collect_err_unique(state, "int enum");
        }
        state->types |= MS_TYPE_INTENUM;
        Py_INCREF(obj);
        state->intenum_obj = obj;
    }
    else {
        if (state->enum_obj != NULL) {
            return typenode_collect_err_unique(state, "str enum");
        }
        state->types |= MS_TYPE_ENUM;
        Py_INCREF(obj);
        state->enum_obj = obj;
    }
    return 0;
}

static int
typenode_collect_dict(TypeNodeCollectState *state, uint64_t type, PyObject *key, PyObject *val) {
    if (state->dict_key_obj != NULL || state->dict_val_obj != NULL) {
        return typenode_collect_err_unique(state, "dict");
    }
    state->types |= type;
    Py_INCREF(key);
    state->dict_key_obj = key;
    Py_INCREF(val);
    state->dict_val_obj = val;
    return 0;
}

static int
typenode_collect_array(TypeNodeCollectState *state, uint64_t type, PyObject *obj) {
    if (state->array_el_obj != NULL) {
        return typenode_collect_err_unique(
            state, "array-like (list, set, tuple)"
        );
    }
    state->types |= type;
    Py_INCREF(obj);
    state->array_el_obj = obj;
    return 0;
}

static int
typenode_collect_custom(TypeNodeCollectState *state, uint64_t type, PyObject *obj) {
    if (state->custom_obj != NULL) {
        return typenode_collect_err_unique(state, "custom");
    }
    state->types |= type;
    Py_INCREF(obj);
    state->custom_obj = obj;
    return 0;
}

static int
typenode_collect_typevar(TypeNodeCollectState *state, PyObject *obj) {
    int out;
    PyObject *bound = PyObject_GetAttr(obj, state->mod->str___bound__);
    if (bound == NULL) return -1;
    if (bound == Py_None) {
        Py_DECREF(bound);

        /* No `bound`, check for constraints */
        PyObject *constraints = PyObject_GetAttr(obj, state->mod->str___constraints__);
        if (constraints == NULL) return -1;
        if (
            !(
                (constraints == Py_None) ||
                (PyTuple_CheckExact(constraints) && (PyTuple_GET_SIZE(constraints) == 0))
            )
        ) {
            PyErr_Format(
                PyExc_TypeError,
                "Unbound TypeVar `%R` has constraints `%R` - constraints are "
                "currently unsupported. If possible, either explicitly bind "
                "the parameter, or use `bound` instead of constraints.",
                obj, constraints
            );
            Py_DECREF(constraints);
            return -1;
        }
        Py_DECREF(constraints);

        /* No constraints either, use `Any` */
        out = typenode_collect_type(state, state->mod->typing_any);
    }
    else {
        /* Bound, substitute in the bound type */
        out = typenode_collect_type(state, bound);
        Py_DECREF(bound);
    }
    return out;
}

static int
typenode_collect_struct(TypeNodeCollectState *state, PyObject *obj) {
    if (state->struct_obj == NULL && state->structs_set == NULL) {
        /* First struct found, store it directly */
        Py_INCREF(obj);
        state->struct_obj = obj;
    }
    else {
        if (state->structs_set == NULL) {
            /* Second struct found, create a set and move the existing struct there */
            state->structs_set = PyFrozenSet_New(NULL);
            if (state->structs_set == NULL) return -1;
            if (PySet_Add(state->structs_set, state->struct_obj) < 0) return -1;
            Py_CLEAR(state->struct_obj);
        }
        if (PySet_Add(state->structs_set, obj) < 0) return -1;
    }
    return 0;
}

static int
typenode_collect_typeddict(TypeNodeCollectState *state, PyObject *obj) {
    if (state->typeddict_obj != NULL) {
        return typenode_collect_err_unique(state, "TypedDict");
    }
    state->types |= MS_TYPE_TYPEDDICT;
    Py_INCREF(obj);
    state->typeddict_obj = obj;
    return 0;
}

static int
typenode_collect_dataclass(TypeNodeCollectState *state, PyObject *obj) {
    if (state->dataclass_obj != NULL) {
        return typenode_collect_err_unique(state, "dataclass or attrs");
    }
    state->types |= MS_TYPE_DATACLASS;
    Py_INCREF(obj);
    state->dataclass_obj = obj;
    return 0;
}

static int
typenode_collect_namedtuple(TypeNodeCollectState *state, PyObject *obj) {
    if (state->namedtuple_obj != NULL) {
        return typenode_collect_err_unique(state, "NamedTuple");
    }
    state->types |= MS_TYPE_NAMEDTUPLE;
    Py_INCREF(obj);
    state->namedtuple_obj = obj;
    return 0;
}

static int
typenode_collect_literal(TypeNodeCollectState *state, PyObject *literal) {
    PyObject *args = PyObject_GetAttr(literal, state->mod->str___args__);
    /* This should never happen, since we know this is a `Literal` object */
    if (args == NULL) return -1;

    Py_ssize_t size = PyTuple_GET_SIZE(args);
    if (size < 0) goto error;

    if (size == 0) {
        PyErr_Format(
            PyExc_TypeError,
            "Literal types must have at least one item, %R is invalid",
            literal
        );
        goto error;
    }

    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *obj = PyTuple_GET_ITEM(args, i);
        PyTypeObject *type = Py_TYPE(obj);

        if (obj == Py_None || obj == NONE_TYPE) {
            state->literal_none = true;
        }
        else if (type == &PyBool_Type) {
            if (obj == Py_True) {
                state->literal_bool_true = true;
            }
            else {
                state->literal_bool_false = true;
            }
        }
        else if (type == &PyLong_Type) {
            if (state->literal_int_values == NULL) {
                state->literal_int_values = PySet_New(NULL);
                if (state->literal_int_values == NULL) goto error;
            }
            if (PySet_Add(state->literal_int_values, obj) < 0) goto error;
        }
        else if (type == &PyUnicode_Type) {
            if (state->literal_str_values == NULL) {
                state->literal_str_values = PySet_New(NULL);
                if (state->literal_str_values == NULL) goto error;
            }
            if (PySet_Add(state->literal_str_values, obj) < 0) goto error;
        }
        else {
            /* Check for nested Literal */
            PyObject *origin = PyObject_GetAttr(obj, state->mod->str___origin__);
            if (origin == NULL) {
                PyErr_Clear();
                goto invalid;
            }
            else if (origin != state->mod->typing_literal) {
                Py_DECREF(origin);
                goto invalid;
            }
            Py_DECREF(origin);
            if (typenode_collect_literal(state, obj) < 0) goto error;
        }
    }

    Py_DECREF(args);
    return 0;

invalid:
    PyErr_Format(
        PyExc_TypeError,
        "Literal may only contain None/booleans/integers/strings - %R is not supported",
        literal
    );

error:
    Py_DECREF(args);
    return -1;
}

static int
typenode_collect_validate_literals(TypeNodeCollectState *state) {
    if (state->literals == NULL) {
        /* Nothing to do */
        return 0;
    }

    Py_ssize_t n = PyList_GET_SIZE(state->literals);

    /* Collect all values in all literals */
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *literal = PyList_GET_ITEM(state->literals, i);
        if (typenode_collect_literal(state, literal) < 0) return -1;
    }
    /* Convert values to lookup objects (if values exist for each type) */
    if (state->literal_int_values != NULL) {
        state->types |= MS_TYPE_INTLITERAL;
        state->literal_int_lookup = IntLookup_New(
            state->literal_int_values, NULL, NULL, false
        );
        if (state->literal_int_lookup == NULL) return -1;
    }
    if (state->literal_str_values != NULL) {
        state->types |= MS_TYPE_STRLITERAL;
        state->literal_str_lookup = StrLookup_New(
            state->literal_str_values, NULL, NULL, false
        );
        if (state->literal_str_lookup == NULL) return -1;
    }
    if (state->literal_none) {
        state->types |= MS_TYPE_NONE;
    }
    if (state->literal_bool_true) {
        state->types |= MS_TYPE_BOOLLITERAL_TRUE;
    }
    if (state->literal_bool_false) {
        state->types |= MS_TYPE_BOOLLITERAL_FALSE;
    }
    return 0;
}

static int
typenode_collect_validate_structs_lock_held(TypeNodeCollectState *state) {
    if (state->struct_obj == NULL && state->structs_set == NULL) {
        return 0;
    }
    else if (state->struct_obj != NULL) {
        /* Single struct */
        state->struct_info = StructInfo_Convert(state->struct_obj);
        if (state->struct_info == NULL) return -1;
        if (((StructInfo *)state->struct_info)->class->array_like == OPT_TRUE) {
            state->types |= MS_TYPE_STRUCT_ARRAY;
        }
        else {
            state->types |= MS_TYPE_STRUCT;
        }
        return 0;
    }

    /* Multiple structs.
     *
     * Try looking the structs_set up in the cache first, to avoid building a
     * new one below.
     */
    PyObject *lookup = NULL;
    if (PyDict_GetItemRef(state->mod->struct_lookup_cache, state->structs_set, &lookup) < 0) {
        return -1;
    }

    if (lookup != NULL) {
        /* Lookup was in the cache, update the state and return */
        state->structs_lookup = lookup;

        if (Lookup_array_like(lookup)) {
            state->types |= MS_TYPE_STRUCT_ARRAY_UNION;
        }
        else {
            state->types |= MS_TYPE_STRUCT_UNION;
        }
        return 0;
    }

    /* Here we check a number of restrictions before building a lookup table
     * from struct tags to their matching classes.
     *
     * Validation checks:
     * - All structs in the set are tagged.
     * - All structs in the set have the same `array_like` status
     * - All structs in the set have the same `tag_field`
     * - All structs in the set have a unique `tag_value`
     *
     * If any of these checks fails, an appropriate error is returned.
     */
    PyObject *tag_mapping = NULL, *tag_field = NULL, *set_iter = NULL, *set_item = NULL;
    PyObject *struct_info = NULL;
    bool array_like = false;
    bool tags_are_strings = true;
    int status = -1;

    tag_mapping = PyDict_New();
    if (tag_mapping == NULL) goto cleanup;

    set_iter = PyObject_GetIter(state->structs_set);
    while ((set_item = PyIter_Next(set_iter))) {
        struct_info = StructInfo_Convert(set_item);
        Py_DECREF(set_item);
        if (struct_info == NULL) goto cleanup;

        StructMetaObject *struct_type = ((StructInfo *)struct_info)->class;
        PyObject *item_tag_field = struct_type->struct_tag_field;
        PyObject *item_tag_value = struct_type->struct_tag_value;
        bool item_array_like = struct_type->array_like == OPT_TRUE;

        if (item_tag_value == NULL) {
            PyErr_Format(
                PyExc_TypeError,
                "If a type union contains multiple Struct types, all Struct "
                "types must be tagged (via `tag` or `tag_field` kwarg) - type "
                "`%R` is not supported",
                state->context
            );
            goto cleanup;
        }
        if (tag_field == NULL) {
            array_like = struct_type->array_like == OPT_TRUE;
            tag_field = struct_type->struct_tag_field;
            tags_are_strings = PyUnicode_CheckExact(item_tag_value);
        }
        else {
            if (array_like != item_array_like) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Type unions may not contain Struct types with `array_like=True` "
                    "and `array_like=False` - type `%R` is not supported",
                    state->context
                );
                goto cleanup;
            }
            if (tags_are_strings != PyUnicode_CheckExact(item_tag_value)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Type unions may not contain Struct types with both `int` "
                    "and `str` tags - type `%R` is not supported",
                    state->context
                );
                goto cleanup;
            }

            int compare = PyUnicode_Compare(item_tag_field, tag_field);
            if (compare == -1 && PyErr_Occurred()) goto cleanup;
            if (compare != 0) {
                PyErr_Format(
                    PyExc_TypeError,
                    "If a type union contains multiple Struct types, all Struct types "
                    "must have the same `tag_field` - type `%R` is not supported",
                    state->context
                );
                goto cleanup;
            }
        }
        if (PyDict_GetItem(tag_mapping, item_tag_value) != NULL) {
            PyErr_Format(
                PyExc_TypeError,
                "If a type union contains multiple Struct types, all Struct types "
                "must have unique `tag` values - type `%R` is not supported",
                state->context
            );
            goto cleanup;
        }

        int ok = PyDict_SetItem(tag_mapping, item_tag_value, struct_info) == 0;
        Py_CLEAR(struct_info);
        if (!ok) goto cleanup;
    }
    /* Build a lookup from tag_value -> struct_info */
    if (tags_are_strings) {
        lookup = StrLookup_New(tag_mapping, tag_field, NULL, array_like);
    }
    else {
        lookup = IntLookup_New(tag_mapping, tag_field, NULL, array_like);
    }
    if (lookup == NULL) goto cleanup;

    state->structs_lookup = lookup;

    /* Check if the cache is full, if so clear the oldest item */
    if (PyDict_GET_SIZE(state->mod->struct_lookup_cache) == 64) {
        PyObject *key;
        Py_ssize_t pos = 0;
        if (PyDict_Next(state->mod->struct_lookup_cache, &pos, &key, NULL)) {
            if (PyDict_DelItem(state->mod->struct_lookup_cache, key) < 0) {
                goto cleanup;
            }
        }
    }

    /* Add the new lookup to the cache */
    if (PyDict_SetItem(state->mod->struct_lookup_cache, state->structs_set, lookup) < 0) {
        goto cleanup;
    }

    /* Update the `types` */
    if (array_like) {
        state->types |= MS_TYPE_STRUCT_ARRAY_UNION;
    }
    else {
        state->types |= MS_TYPE_STRUCT_UNION;
    }

    status = 0;

cleanup:
    Py_XDECREF(set_iter);
    Py_XDECREF(tag_mapping);
    Py_XDECREF(struct_info);
    return status;
}

static int
typenode_collect_validate_structs(TypeNodeCollectState *state) {
    int status;
    Py_BEGIN_CRITICAL_SECTION(state->mod->struct_lookup_cache);
    status = typenode_collect_validate_structs_lock_held(state);
    Py_END_CRITICAL_SECTION();
    return status;
}


static void
typenode_collect_clear_state(TypeNodeCollectState *state) {
    Py_CLEAR(state->struct_obj);
    Py_CLEAR(state->struct_info);
    Py_CLEAR(state->structs_set);
    Py_CLEAR(state->structs_lookup);
    Py_CLEAR(state->intenum_obj);
    Py_CLEAR(state->enum_obj);
    Py_CLEAR(state->custom_obj);
    Py_CLEAR(state->array_el_obj);
    Py_CLEAR(state->dict_key_obj);
    Py_CLEAR(state->dict_val_obj);
    Py_CLEAR(state->typeddict_obj);
    Py_CLEAR(state->dataclass_obj);
    Py_CLEAR(state->namedtuple_obj);
    Py_CLEAR(state->literals);
    Py_CLEAR(state->literal_int_values);
    Py_CLEAR(state->literal_int_lookup);
    Py_CLEAR(state->literal_str_values);
    Py_CLEAR(state->literal_str_lookup);
    Py_CLEAR(state->c_str_regex);
    Py_CLEAR(state->codec_obj);
}

/* This decomposes an input type `obj`, stripping out any "wrapper" types
 * (Annotated/NewType). It returns the following components:
 *
 * - `t` (return value): the first "concrete" type found in the type tree.
 * - `origin`: `__origin__` on `t` (if present), with a few normalizations
 *   applied to work around differences in type spelling (List vs list) and
 *   python version.
 * - `args`: `__args__` on `t` (if present)
 * - `constraints`: Any constraints from `Field` objects annotated on the type
 */
static PyObject *
typenode_origin_args_metadata(
    TypeNodeCollectState *state, PyObject *obj,
    PyObject **out_origin, PyObject **out_args, Constraints *constraints
) {
    PyObject *origin = NULL, *args = NULL;
    PyObject *t = obj;
    Py_INCREF(t);

    /* First strip out meta "wrapper" types (Annotated, NewType, Final) */
    while (true) {
        assert(t != NULL && origin == NULL && args == NULL);

        /* Before inspecting attributes, try looking up the object in the
         * abstract -> concrete mapping. If present, this is an unparametrized
         * collection of some form. This helps avoid compatibility issues in
         * Python 3.8, where unparametrized collections still have __args__. */
        origin = PyDict_GetItemWithError(state->mod->concrete_types, t);
        if (origin != NULL) {
            Py_INCREF(origin);
            break;
        }
        else {
            /* Ignore all errors in this initial check */
            PyErr_Clear();
        }

        /* If `t` is a type instance, no need to inspect further */
        if (PyType_CheckExact(t)) {
            /* t is a concrete type object. */
            break;
        }

        origin = PyObject_GetAttr(t, state->mod->str___origin__);
        if (origin != NULL) {
            if (Py_TYPE(t) == (PyTypeObject *)(state->mod->typing_annotated_alias)) {
                /* Handle typing.Annotated[...] */
                PyObject *metadata = PyObject_GetAttr(t, state->mod->str___metadata__);
                if (metadata == NULL) goto error;
                for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(metadata); i++) {
                    PyObject *annot = PyTuple_GET_ITEM(metadata, i);
                    if (Py_TYPE(annot) == &Field_Type) {
                        if (constraints_update(constraints, (Field *)annot, obj) < 0) {
                            Py_DECREF(metadata);
                            goto error;
                        }
                    }
                }
                Py_DECREF(metadata);
                Py_DECREF(t);
                t = origin;
                origin = NULL;
                continue;
            }
            else {
                args = PyObject_GetAttr(t, state->mod->str___args__);
                if (args != NULL) {
                    if (!PyTuple_Check(args)) {
                        PyErr_SetString(PyExc_TypeError, "__args__ must be a tuple");
                        goto error;
                    }
                    if (origin == state->mod->typing_final) {
                        /* Handle typing.Final[...] */
                        PyObject *temp = PyTuple_GetItem(args, 0);
                        if (temp == NULL) goto error;
                        Py_CLEAR(args);
                        Py_CLEAR(origin);
                        Py_DECREF(t);
                        Py_INCREF(temp);
                        t = temp;
                        continue;
                    }
                    /* Check for parametrized TypeAliasType if Python 3.12+ */
                #if PY312_PLUS
                    if (Py_TYPE(origin) == (PyTypeObject *)(state->mod->typing_typealiastype)) {
                        PyObject *value = PyObject_GetAttr(origin, state->mod->str___value__);
                        if (value == NULL) goto error;
                        PyObject *temp = PyObject_GetItem(value, args);
                        Py_DECREF(value);
                        if (temp == NULL) goto error;
                        Py_CLEAR(args);
                        Py_CLEAR(origin);
                        Py_DECREF(t);
                        t = temp;
                        continue;
                    }
                #endif
                }
                else {
                    /* Custom non-parametrized generics won't have __args__
                     * set. Ignore __args__ error */
                    PyErr_Clear();
                }
                /* Lookup __origin__ in the mapping, in case it's a supported
                * abstract type. Equal to `origin = mapping.get(origin, origin)` */
                PyObject *temp = PyDict_GetItem(state->mod->concrete_types, origin);
                if (temp != NULL) {
                    Py_DECREF(origin);
                    Py_INCREF(temp);
                    origin = temp;
                }
                break;
            }
        }
        else {
            PyErr_Clear();

            /* Check for NewType */
            PyObject *supertype = PyObject_GetAttr(t, state->mod->str___supertype__);
            if (supertype != NULL) {
                /* It's a newtype, use the wrapped type and loop again */
                Py_DECREF(t);
                t = supertype;
                continue;
            }
            PyErr_Clear();

            /* Check for TypeAliasType if Python 3.12+ */
        #if PY312_PLUS
            if (Py_TYPE(t) == (PyTypeObject *)(state->mod->typing_typealiastype)) {
                PyObject *value = PyObject_GetAttr(t, state->mod->str___value__);
                if (value == NULL) goto error;
                Py_DECREF(t);
                t = value;
                continue;
            }
        #endif
            break;
        }
    }

    if (Py_TYPE(t) == (PyTypeObject *)(state->mod->types_uniontype)) {
        /* Handle types.UnionType unions (`int | float | ...`) */
        args = PyObject_GetAttr(t, state->mod->str___args__);
        if (args == NULL) goto error;
        origin = state->mod->typing_union;
        Py_INCREF(origin);
    }

    *out_origin = origin;
    *out_args = args;
    return t;

error:
    Py_XDECREF(t);
    Py_XDECREF(origin);
    Py_XDECREF(args);
    return NULL;
}

static bool
is_namedtuple_class(TypeNodeCollectState *state, PyObject *t) {
    return (
        PyType_Check(t)
        && PyType_IsSubtype((PyTypeObject *)t, &PyTuple_Type)
        && PyObject_HasAttr(t, state->mod->str__fields)
    );
}

static bool
is_typeddict_class(TypeNodeCollectState *state, PyObject *t) {
    return (
        PyType_Check(t)
        && PyType_IsSubtype((PyTypeObject *)t, &PyDict_Type)
        && PyObject_HasAttr(t, state->mod->str___total__)
    );
}

static bool
is_dataclass_or_attrs_class(TypeNodeCollectState *state, PyObject *t) {
    return (
        PyType_Check(t) && (
            PyObject_HasAttr(t, state->mod->str___dataclass_fields__) ||
            PyObject_HasAttr(t, state->mod->str___attrs_attrs__)
        )
    );
}

static MS_INLINE int
normalize_types_generic_alias(TypeNodeCollectState *state, PyObject **t, PyObject *origin, PyObject *args) {
    // if '*t' is a 'types.GenericAlias', replace it (in place) with an equivalent
    // 'typing._GenericAlias'. 'types.GenericAlias' has __slots__ and forwards
    // attribute access to its origin, so we can't cache type info (as
    // '__structtype_cache__') on it; a 'typing._GenericAlias' can.
    //
    // only the branches that cache on the alias (struct/dataclass/TypedDict/
    // NamedTuple/Literal) need to call this. a 'types.GenericAlias' here only arises
    // when  subclassing a builtin container generic (e.g. 'collections.abc.Mapping') or
    // from a manually constructed 'types.GenericAlias' (e.g. wrapping 'typing.Literal').
    //
    // replace '*t' with a new reference on success (dropping the original), or
    // return -1 with an exception set.
    if (MS_LIKELY(Py_TYPE(*t) != &Py_GenericAliasType)) return 0;
    PyObject *converted = PyObject_CallFunctionObjArgs(
        state->mod->convert_generic_alias, origin, args, NULL
    );
    if (converted == NULL) return -1;
    Py_SETREF(*t, converted);
    return 0;
}

static int
typenode_collect_type(TypeNodeCollectState *state, PyObject *obj) {
    int out = 0;
    PyObject *t = NULL, *origin = NULL, *args = NULL;
    Constraints constraints = {0};
    enum constraint_kind kind = CK_OTHER;

    /* If `Any` type already encountered, nothing to do */
    if (state->types & MS_TYPE_ANY) return 0;

    t = typenode_origin_args_metadata(state, obj, &origin, &args, &constraints);
    if (t == NULL) return -1;

    if (t == state->mod->typing_any) {
        /* Any takes precedence, drop all existing and update type flags */
        typenode_collect_clear_state(state);
        state->types = MS_TYPE_ANY;
    }
    else if (t == Py_None || t == NONE_TYPE) {
        state->types |= MS_TYPE_NONE;
    }
    else if (t == (PyObject *)(&PyBool_Type)) {
        state->types |= MS_TYPE_BOOL;
    }
    else if (t == (PyObject *)(&PyLong_Type)) {
        state->types |= MS_TYPE_INT;
        kind = CK_INT;
    }
    else if (t == (PyObject *)(&PyFloat_Type)) {
        state->types |= MS_TYPE_FLOAT;
        kind = CK_FLOAT;
    }
    else if (t == (PyObject *)(&PyUnicode_Type)) {
        state->types |= MS_TYPE_STR;
        kind = CK_STR;
    }
    else if (t == (PyObject *)(&PyBytes_Type)) {
        state->types |= MS_TYPE_BYTES;
        kind = CK_BYTES;
    }
    else if (t == (PyObject *)(&PyByteArray_Type)) {
        state->types |= MS_TYPE_BYTEARRAY;
        kind = CK_BYTES;
    }
    else if (t == (PyObject *)(&PyMemoryView_Type)) {
        state->types |= MS_TYPE_MEMORYVIEW;
        kind = CK_BYTES;
    }
    else if (t == (PyObject *)(PyDateTimeAPI->DateTimeType)) {
        state->types |= MS_TYPE_DATETIME;
        kind = CK_TIME;
    }
    else if (t == (PyObject *)(PyDateTimeAPI->DateType)) {
        state->types |= MS_TYPE_DATE;
    }
    else if (t == (PyObject *)(PyDateTimeAPI->TimeType)) {
        state->types |= MS_TYPE_TIME;
        kind = CK_TIME;
    }
    else if (t == (PyObject *)(PyDateTimeAPI->DeltaType)) {
        state->types |= MS_TYPE_TIMEDELTA;
    }
    else if (t == state->mod->UUIDType) {
        state->types |= MS_TYPE_UUID;
    }
    else if (t == state->mod->DecimalType) {
        state->types |= MS_TYPE_DECIMAL;
    }
    else if (t == (PyObject *)(&Raw_Type)) {
        /* Raw is marked with a typecode of 0, nothing to do */
    }
    else if (Py_TYPE(t) == (PyTypeObject *)(state->mod->typing_typevar)) {
        out = typenode_collect_typevar(state, t);
    }
    else if (
        ms_is_struct_cls(t) ||
        (origin != NULL && ms_is_struct_cls(origin))
    ) {
        if (normalize_types_generic_alias(state, &t, origin, args) < 0) {
            out = -1;
        }
        else {
            out = typenode_collect_struct(state, t);
        }
    }
    else if (PyType_IsSubtype(Py_TYPE(t), state->mod->EnumMetaType)) {
        out = typenode_collect_enum(state, t);
    }
    else if (origin == (PyObject*)(&PyDict_Type)) {
        kind = CK_MAP;
        if (args != NULL && PyTuple_GET_SIZE(args) != 2) goto invalid;
        out = typenode_collect_dict(
            state,
            MS_TYPE_DICT,
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 0),
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 1)
        );
    }
    else if (origin == (PyObject*)(&PyList_Type)) {
        kind = CK_ARRAY;
        if (args != NULL && PyTuple_GET_SIZE(args) != 1) goto invalid;
        out = typenode_collect_array(
            state,
            MS_TYPE_LIST,
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 0)
        );
    }
#if PY315_PLUS
    else if (origin == (PyObject*)(&PyFrozenDict_Type)) {
        kind = CK_MAP;
        if (args != NULL && PyTuple_GET_SIZE(args) != 2) goto invalid;
        out = typenode_collect_dict(
            state,
            MS_TYPE_FROZENDICT,
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 0),
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 1)
        );
    }
#endif
    else if (origin == (PyObject*)(&PySet_Type)) {
        kind = CK_ARRAY;
        if (args != NULL && PyTuple_GET_SIZE(args) != 1) goto invalid;
        out = typenode_collect_array(
            state,
            MS_TYPE_SET,
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 0)
        );
    }
    else if (origin == (PyObject*)(&PyFrozenSet_Type)) {
        kind = CK_ARRAY;
        if (args != NULL && PyTuple_GET_SIZE(args) != 1) goto invalid;
        out = typenode_collect_array(
            state,
            MS_TYPE_FROZENSET,
            (args == NULL) ? state->mod->typing_any : PyTuple_GET_ITEM(args, 0)
        );
    }
    else if (origin == (PyObject*)(&PyTuple_Type)) {
        if (args == NULL) {
            kind = CK_ARRAY;
            out = typenode_collect_array(
                state, MS_TYPE_VARTUPLE, state->mod->typing_any
            );
        }
        else if (PyTuple_GET_SIZE(args) == 2 && PyTuple_GET_ITEM(args, 1) == Py_Ellipsis) {
            kind = CK_ARRAY;
            out = typenode_collect_array(
                state, MS_TYPE_VARTUPLE, PyTuple_GET_ITEM(args, 0)
            );
        }
        else if (
            PyTuple_GET_SIZE(args) == 1 &&
            PyTuple_CheckExact(PyTuple_GET_ITEM(args, 0)) &&
            PyTuple_GET_SIZE(PyTuple_GET_ITEM(args, 0)) == 0
        ) {
            /* XXX: this case handles a weird compatibility issue:
             * - typing.Tuple[()].__args__ == ((),)
             * - tuple[()].__args__ == ()
             */
            out = typenode_collect_array(
                state, MS_TYPE_FIXTUPLE, PyTuple_GET_ITEM(args, 0)
            );
        }
        else {
            out = typenode_collect_array(state, MS_TYPE_FIXTUPLE, args);
        }
    }
    else if (origin == state->mod->typing_union) {
        if (Py_EnterRecursiveCall(" while analyzing a type")) {
            out = -1;
            goto done;
        }
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); i++) {
            PyObject *arg = PyTuple_GET_ITEM(args, i);
            /* Ignore UnsetType in unions */
            if (arg == (PyObject *)(&Unset_Type)) continue;
            out = typenode_collect_type(state, arg);
            if (out < 0) break;
        }
        Py_LeaveRecursiveCall();
    }
    else if (origin == state->mod->typing_literal) {
        if (state->literals == NULL) {
            state->literals = PyList_New(0);
            if (state->literals == NULL) goto done;
        }
        if (normalize_types_generic_alias(state, &t, origin, args) < 0) {
            out = -1;
        }
        else {
            out = PyList_Append(state->literals, t);
        }
    }
    else if (
        is_typeddict_class(state, t) ||
        (origin != NULL && is_typeddict_class(state, origin))
    ) {
        if (normalize_types_generic_alias(state, &t, origin, args) < 0) {
            out = -1;
        }
        else {
            out = typenode_collect_typeddict(state, t);
        }
    }
    else if (
        is_namedtuple_class(state, t) ||
        (origin != NULL && is_namedtuple_class(state, origin))
    ) {
        if (normalize_types_generic_alias(state, &t, origin, args) < 0) {
            out = -1;
        }
        else {
            out = typenode_collect_namedtuple(state, t);
        }
    }
    else if (
        is_dataclass_or_attrs_class(state, t) ||
        (origin != NULL && is_dataclass_or_attrs_class(state, origin))
    ) {
        if (normalize_types_generic_alias(state, &t, origin, args) < 0) {
            out = -1;
        }
        else {
            out = typenode_collect_dataclass(state, t);
        }
    }
    else {
        if (origin != NULL) {
            if (!PyType_Check(origin)) goto invalid;
        }
        else {
            if (!PyType_Check(t)) goto invalid;
        }
        out = typenode_collect_custom(
            state,
            (origin != NULL) ? MS_TYPE_CUSTOM_GENERIC : MS_TYPE_CUSTOM,
            t
        );
    }

done:
    Py_XDECREF(t);
    Py_XDECREF(origin);
    Py_XDECREF(args);
    if (out == 0) {
        out = typenode_collect_constraints(state, &constraints, kind, obj);
    }
    return out;

invalid:
    PyErr_Format(PyExc_TypeError, "Type '%R' is not supported", t);
    out = -1;
    goto done;
}

static TypeNode *
TypeNode_Convert(PyObject *obj) {
    TypeNode *out = NULL;
    TypeNodeCollectState state = {0};
    state.mod = structtype_get_global_state();
    state.context = obj;

    if (Py_EnterRecursiveCall(" while analyzing a type")) return NULL;

    /* Traverse `obj` to collect all type annotations at this level */
    if (typenode_collect_type(&state, obj) < 0) goto done;
    /* Handle structs in a second pass */
    if (typenode_collect_validate_structs(&state) < 0) goto done;
    /* Handle literals in a second pass */
    if (typenode_collect_validate_literals(&state) < 0) goto done;
    /* Check type invariants to ensure Union types are valid */
    if (typenode_collect_check_invariants(&state) < 0) goto done;
    /* Populate a new TypeNode, recursing into subtypes as needed */
    out = typenode_from_collect_state(&state);
done:
    typenode_collect_clear_state(&state);
    Py_LeaveRecursiveCall();
    return out;
}

#define ms_raise_validation_error(path, format, ...) \
    do { \
        StructspecState *st = structtype_get_global_state(); \
        PyObject *suffix = PathNode_ErrSuffix(path); \
        if (suffix != NULL) { \
            PyErr_Format(st->ValidationError, format, __VA_ARGS__, suffix); \
            Py_DECREF(suffix); \
        } \
    } while (0)

static MS_NOINLINE PyObject *
ms_validation_error(const char *got, TypeNode *type, PathNode *path) {
    PyObject *type_repr = typenode_simple_repr(type);
    if (type_repr != NULL) {
        ms_raise_validation_error(path, "Expected `%U`, got `%s`%U", type_repr, got);
        Py_DECREF(type_repr);
    }
    return NULL;
}

static void
ms_missing_required_field(PyObject *field, PathNode *path) {
    ms_raise_validation_error(
        path,
        "Object missing required field `%U`%U",
        field
    );
}

static PyObject *
ms_invalid_cstr_value(const char *cstr, Py_ssize_t size, PathNode *path) {
    PyObject *str = PyUnicode_DecodeUTF8(cstr, size, NULL);
    if (str == NULL) return NULL;
    ms_raise_validation_error(path, "Invalid value '%U'%U", str);
    Py_DECREF(str);
    return NULL;
}

static PyObject *
ms_invalid_cint_value(int64_t val, PathNode *path) {
    ms_raise_validation_error(path, "Invalid value %lld%U", val);
    return NULL;
}

static PyObject *
ms_invalid_cuint_value(uint64_t val, PathNode *path) {
    ms_raise_validation_error(path, "Invalid value %llu%U", val);
    return NULL;
}

static MS_NOINLINE PyObject *
ms_error_unknown_field(const char *key, Py_ssize_t key_size, PathNode *path) {
    PyObject *field = PyUnicode_FromStringAndSize(key, key_size);
    if (field == NULL) return NULL;
    ms_raise_validation_error(
        path, "Object contains unknown field `%U`%U", field
    );
    Py_DECREF(field);
    return NULL;
}

/* Same as ms_raise_validation_error, except doesn't require any format arguments. */
static PyObject *
ms_error_with_path(const char *msg, PathNode *path) {
    StructspecState *st = structtype_get_global_state();
    PyObject *suffix = PathNode_ErrSuffix(path);
    if (suffix != NULL) {
        PyErr_Format(st->ValidationError, msg, suffix);
        Py_DECREF(suffix);
    }
    return NULL;
}

static MS_NOINLINE void
ms_maybe_wrap_validation_error(PathNode *path) {
    PyObject *exc_type, *exc, *tb;

    /* Fetch the exception state */
    PyErr_Fetch(&exc_type, &exc, &tb);

    /* If null, some other c-extension has borked, just return */
    if (exc_type == NULL) return;

    StructspecState *mod = structtype_get_global_state();

    /* If it's a TypeError or ValueError, wrap it in a ValidationError.
     * Otherwise we reraise the original error below.
     * Since both errors subclass from ValueError, we need
     * to special case them below - we don't wrap in those cases. */
    if (
        !PyType_IsSubtype((PyTypeObject *)exc_type, (PyTypeObject *)mod->DecodeError)
        &&
        (
            PyType_IsSubtype(
                (PyTypeObject *)exc_type, (PyTypeObject *)PyExc_ValueError
            ) ||
            PyType_IsSubtype(
                (PyTypeObject *)exc_type, (PyTypeObject *)PyExc_TypeError
            )
        )
    ) {
        PyObject *exc_type2, *exc2, *tb2;

        /* Normalize the original exception */
        PyErr_NormalizeException(&exc_type, &exc, &tb);
        if (tb != NULL) {
            PyException_SetTraceback(exc, tb);
            Py_DECREF(tb);
        }
        Py_DECREF(exc_type);

        /* Raise a new validation error with context based on the
            * original exception */
        ms_raise_validation_error(path, "%S%U", exc);

        /* Fetch the new exception */
        PyErr_Fetch(&exc_type2, &exc2, &tb2);
        /* Normalize the new exception */
        PyErr_NormalizeException(&exc_type2, &exc2, &tb2);
        /* Set the original exception as the cause and context */
        Py_INCREF(exc);
        PyException_SetCause(exc2, exc);
        PyException_SetContext(exc2, exc);

        /* At this point the original exc_type/exc/tb are all dropped,
            * replace them with the new values */
        exc_type = exc_type2;
        exc = exc2;
        tb = tb2;
    }
    /* Restore the new exception state */
    PyErr_Restore(exc_type, exc, tb);
}

static PyTypeObject StructMixinType;


static PyObject *
Struct_alloc(PyTypeObject *type) {
    return type->tp_alloc(type, type->tp_itemsize);
}

/* Mirrored from cpython Objects/typeobject.c */
static void
clear_slots(PyTypeObject *type, PyObject *self)
{
    Py_ssize_t i, n;
    PyMemberDef *mp;

    n = Py_SIZE(type);
    mp = MS_PyHeapType_GET_MEMBERS((PyHeapTypeObject *)type);
    for (i = 0; i < n; i++, mp++) {
        if (mp->type == T_OBJECT_EX && !(mp->flags & READONLY)) {
            char *addr = (char *)self + mp->offset;
            PyObject *obj = *(PyObject **)addr;
            if (obj != NULL) {
                *(PyObject **)addr = NULL;
                Py_DECREF(obj);
            }
        }
    }
}

static void
Struct_dealloc_nogc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);

    /* Maybe call a finalizer */
    if (type->tp_finalize) {
        /* If resurrected, exit early */
        if (PyObject_CallFinalizerFromDealloc(self) < 0) return;
    }

    /* Maybe clear weakrefs */
    if (type->tp_weaklistoffset) {
        PyObject_ClearWeakRefs(self);
    }

    /* Clear all slots */
    PyTypeObject *base = type;
    while (base != NULL) {
        if (Py_SIZE(base)) {
            clear_slots(base, self);
        }
        base = base->tp_base;
    }

    type->tp_free(self);
    /* Decref the object type immediately */
    Py_DECREF(type);
}

static PyObject *
StructMeta_get_field_name(PyObject *self, Py_ssize_t field_index) {
    return PyTuple_GET_ITEM(
        ((StructMetaObject *)self)->struct_encode_fields, field_index
    );
}

static MS_INLINE Py_ssize_t
StructMeta_get_field_index(
    StructMetaObject *self, const char * key, Py_ssize_t key_size, Py_ssize_t *pos
) {
    const char *field;
    Py_ssize_t nfields, field_size, i, offset = *pos;
    nfields = PyTuple_GET_SIZE(self->struct_encode_fields);
    for (i = offset; i < nfields; i++) {
        field = unicode_str_and_size_nocheck(
            PyTuple_GET_ITEM(self->struct_encode_fields, i), &field_size
        );
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i < (nfields - 1) ? (i + 1) : 0;
            return i;
        }
    }
    for (i = 0; i < offset; i++) {
        field = unicode_str_and_size_nocheck(
            PyTuple_GET_ITEM(self->struct_encode_fields, i), &field_size
        );
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i + 1;
            return i;
        }
    }
    /* Not a field, check if it matches the tag field (if present) */
    if (MS_UNLIKELY(self->struct_tag_field != NULL)) {
        Py_ssize_t tag_field_size;
        const char *tag_field;
        tag_field = unicode_str_and_size_nocheck(self->struct_tag_field, &tag_field_size);
        if (key_size == tag_field_size && memcmp(key, tag_field, key_size) == 0) {
            return -2;
        }
    }
    return -1;
}

static int
dict_discard(PyObject *dict, PyObject *key) {
    int status = PyDict_Contains(dict, key);
    if (status < 0)
        return status;
    return (status == 1) ? PyDict_DelItem(dict, key) : 0;
}

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames);

/* setattr for frozen=True types */
static int
Struct_setattro_frozen(PyObject *self, PyObject *key, PyObject *value) {
    PyErr_Format(
        PyExc_AttributeError, "immutable type: '%s'", Py_TYPE(self)->tp_name
    );
    return -1;
}

/* setattr for frozen=False types */
static int
Struct_setattro_default(PyObject *self, PyObject *key, PyObject *value) {
    if (PyObject_GenericSetAttr(self, key, value) < 0) return -1;
    if (
        value != NULL &&
        MS_OBJECT_IS_GC(self) &&
        !MS_IS_TRACKED(self) &&
        MS_MAYBE_TRACKED(value)
    )
        PyObject_GC_Track(self);
    return 0;
}

static PyObject*
rename_lower(PyObject *rename, PyObject *field) {
    return PyObject_CallMethod(field, "lower", NULL);
}

static PyObject*
rename_upper(PyObject *rename, PyObject *field) {
    return PyObject_CallMethod(field, "upper", NULL);
}

static PyObject*
rename_kebab(PyObject *rename, PyObject *field) {
    PyObject *underscore = NULL, *dash = NULL, *temp = NULL, *out = NULL;
    underscore = PyUnicode_FromStringAndSize("_", 1);
    if (underscore == NULL) goto error;
    dash = PyUnicode_FromStringAndSize("-", 1);
    if (dash == NULL) goto error;
    temp = PyObject_CallMethod(field, "strip", "s", "_");
    if (temp == NULL) goto error;
    out = PyUnicode_Replace(temp, underscore, dash, -1);
error:
    Py_XDECREF(underscore);
    Py_XDECREF(dash);
    Py_XDECREF(temp);
    return out;
}

static PyObject*
rename_camel_inner(PyObject *field, bool cap_first) {
    PyObject *parts = NULL, *out = NULL, *empty = NULL;
    PyObject *underscore = PyUnicode_FromStringAndSize("_", 1);
    if (underscore == NULL) return NULL;

    parts = PyUnicode_Split(field, underscore, -1);
    if (parts == NULL) goto cleanup;

    if (PyList_GET_SIZE(parts) == 1 && !cap_first) {
        Py_INCREF(field);
        out = field;
        goto cleanup;
    }

    bool first = true;
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(parts); i++) {
        PyObject *part = PyList_GET_ITEM(parts, i);
        if (first && (PyUnicode_GET_LENGTH(part) == 0)) {
            /* Preserve leading underscores */
            Py_INCREF(underscore);
            Py_DECREF(part);
            PyList_SET_ITEM(parts, i, underscore);
        }
        else {
            if (!first || cap_first) {
                /* convert part to title case, inplace in the list */
                PyObject *part_title = PyObject_CallMethod(part, "title", NULL);
                if (part_title == NULL) goto cleanup;
                Py_DECREF(part);
                PyList_SET_ITEM(parts, i, part_title);
            }
            first = false;
        }
    }
    empty = PyUnicode_FromStringAndSize("", 0);
    if (empty == NULL) goto cleanup;
    out = PyUnicode_Join(empty, parts);

cleanup:
    Py_XDECREF(empty);
    Py_XDECREF(underscore);
    Py_XDECREF(parts);
    return out;
}

static PyObject*
rename_camel(PyObject *rename, PyObject *field) {
    return rename_camel_inner(field, false);
}

static PyObject*
rename_pascal(PyObject *rename, PyObject *field) {
    return rename_camel_inner(field, true);
}

static PyObject*
rename_callable(PyObject *rename, PyObject *field) {
    PyObject *temp = PyObject_CallOneArg(rename, field);
    if (temp == NULL) return NULL;
    if (PyUnicode_CheckExact(temp)) return temp;
    if (temp == Py_None) {
        Py_DECREF(temp);
        Py_INCREF(field);
        return field;
    }
    PyErr_Format(
        PyExc_TypeError,
        "Expected calling `rename` to return a `str` or `None`, got `%.200s`",
        Py_TYPE(temp)->tp_name
    );
    Py_DECREF(temp);
    return NULL;
}

static PyObject*
rename_mapping(PyObject *rename, PyObject *field) {
    PyObject *temp = PyObject_GetItem(rename, field);
    if (temp == NULL) {
        PyErr_Clear();
        Py_INCREF(field);
        return field;
    }
    else if (temp == Py_None) {
        Py_DECREF(temp);
        Py_INCREF(field);
        return field;
    }
    else if (PyUnicode_CheckExact(temp)) {
        return temp;
    }
    PyErr_Format(
        PyExc_TypeError,
        "Expected `rename[field]` to return a `str` or `None`, got `%.200s`",
        Py_TYPE(temp)->tp_name
    );
    Py_DECREF(temp);
    return NULL;
}

typedef struct {
    /* Temporary state. All owned references. */
    PyObject *defaults_lk;
    PyObject *offsets_lk;
    PyObject *kwonly_fields;
    PyObject *slots;
    PyObject *namespace;
    PyObject *renamed_fields;
    /* Output values. All owned references. */
    PyObject *fields;
    PyObject *encode_fields;
    PyObject *defaults;
    PyObject *match_args;
    PyObject *tag;
    PyObject *tag_field;
    PyObject *tag_value;
    Py_ssize_t *offsets;
    Py_ssize_t nkwonly;
    Py_ssize_t n_trailing_defaults;
    /* Configuration values. All borrowed references. */
    PyObject *name;
    PyObject *temp_tag_field;
    PyObject *temp_tag;
    PyObject *rename;
    int omit_defaults;
    int forbid_unknown_fields;
    int validate_on_init;
    int frozen;
    int eq;
    int order;
    int repr_omit_defaults;
    int array_like;
    int gc;
    int weakref;
    bool already_has_weakref;
    int dict;
    bool already_has_dict;
    int cache_hash;
    Py_ssize_t hash_offset;
    bool has_non_slots_bases;
    PyObject *resolved_annotations;
    PyObject *codec_maps;  /* tuple of per-field codec dicts, or NULL */
} StructMetaInfo;

static int
structmeta_check_namespace(PyObject *namespace) {
    static const char *attrs[] = {"__init__", "__new__", "__slots__"};
    Py_ssize_t nattrs = 3;

    for (Py_ssize_t i = 0; i < nattrs; i++) {
        if (PyDict_GetItemString(namespace, attrs[i]) != NULL) {
            PyErr_Format(PyExc_TypeError, "Struct types cannot define %s", attrs[i]);
            return -1;
        }
    }
    return 0;
}

static PyObject *
structmeta_get_module_ns(StructspecState *mod, StructMetaInfo *info) {
    PyObject *name = PyDict_GetItemString(info->namespace, "__module__");
    if (name == NULL) return NULL;
    PyObject *modules = PySys_GetObject("modules");
    if (modules == NULL) return NULL;
    PyObject *module = PyDict_GetItem(modules, name);
    if (module == NULL) return NULL;
    return PyObject_GetAttr(module, mod->str___dict__);
}

static int
structmeta_collect_base(StructMetaInfo *info, StructspecState *mod, PyObject *base) {
    if ((PyTypeObject *)base == &StructMixinType) return 0;

    if (((PyTypeObject *)base)->tp_weaklistoffset) {
        info->already_has_weakref = true;
    }

    if (((PyTypeObject *)base)->tp_dictoffset) {
        info->already_has_dict = true;
    }

    if (!PyType_Check(base)) {
        /* CPython's metaclass conflict check will catch this issue earlier on,
         * but it's still good to have this check in place in case that's ever
         * removed */
        PyErr_SetString(PyExc_TypeError, "All base classes must be types");
        return -1;
    }

    if (!ms_is_struct_cls(base)) {
        if (((PyTypeObject *)base)->tp_dictoffset) {
            info->has_non_slots_bases = true;
        }

        static const char *attrs[] = {"__init__", "__new__"};
        Py_ssize_t nattrs = 2;
        PyObject *tp_dict = MS_GET_TYPE_DICT((PyTypeObject *)base);
        for (Py_ssize_t i = 0; i < nattrs; i++) {
            if (PyDict_GetItemString(tp_dict, attrs[i]) != NULL) {
                PyErr_Format(PyExc_TypeError, "Struct base classes cannot define %s", attrs[i]);
                return -1;
            }
        }
        return 0;
    }

    StructMetaObject *st_type = (StructMetaObject *)base;

    /* Check if a hash_cache slot already exists */
    if (st_type->hash_offset != 0) {
        info->hash_offset = st_type->hash_offset;
    }

    /* Inherit config fields */
    if (st_type->struct_tag_field != NULL) {
        info->temp_tag_field = st_type->struct_tag_field;
    }
    if (st_type->struct_tag != NULL) {
        info->temp_tag = st_type->struct_tag;
    }
    if (st_type->rename != NULL) {
        info->rename = st_type->rename;
    }
    info->frozen = STRUCT_MERGE_OPTIONS(info->frozen, st_type->frozen);
    info->eq = STRUCT_MERGE_OPTIONS(info->eq, st_type->eq);
    info->order = STRUCT_MERGE_OPTIONS(info->order, st_type->order);
    info->array_like = STRUCT_MERGE_OPTIONS(info->array_like, st_type->array_like);
    info->gc = STRUCT_MERGE_OPTIONS(info->gc, st_type->gc);
    info->omit_defaults = STRUCT_MERGE_OPTIONS(info->omit_defaults, st_type->omit_defaults);
    info->repr_omit_defaults = STRUCT_MERGE_OPTIONS(
        info->repr_omit_defaults, st_type->repr_omit_defaults
    );
    info->forbid_unknown_fields = STRUCT_MERGE_OPTIONS(
        info->forbid_unknown_fields, st_type->forbid_unknown_fields
    );

    PyObject *fields = st_type->struct_fields;
    PyObject *encode_fields = st_type->struct_encode_fields;
    PyObject *defaults = st_type->struct_defaults;
    Py_ssize_t *offsets = st_type->struct_offsets;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    Py_ssize_t nkwonly = st_type->nkwonly;
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(defaults);
    Py_ssize_t defaults_offset = nfields - ndefaults;

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(fields, i);
        PyObject *encode_field = PyTuple_GET_ITEM(encode_fields, i);
        PyObject *default_val = NODEFAULT;
        if (i >= defaults_offset) {
            default_val = PyTuple_GET_ITEM(defaults, i - defaults_offset);
        }
        if (PyDict_SetItem(info->defaults_lk, field, default_val) < 0) return -1;

        /* Mark the field as kwonly or not */
        if (i >= (nfields - nkwonly)) {
            if (PySet_Add(info->kwonly_fields, field) < 0) return -1;
        }
        else {
            if (PySet_Discard(info->kwonly_fields, field) < 0) return -1;
        }

        /* Propagate any renamed fields */
        if (field != encode_field) {
            if (PyDict_SetItem(info->renamed_fields, field, encode_field) < 0) return -1;
        }

        PyObject *offset = PyLong_FromSsize_t(offsets[i]);
        if (offset == NULL) return -1;
        bool errored = PyDict_SetItem(info->offsets_lk, field, offset) < 0;
        Py_DECREF(offset);
        if (errored) return -1;
    }
    return 0;
}

/* Scan an annotation for __metadata__ containing a Field instance.
 * Returns borrowed pointer to Field (or NULL if none found).
 * Preserves any existing error indicator. */
static Field *
extract_field_from_annotated(PyObject *annotation, StructspecState *mod) {
    if (annotation == NULL) return NULL;
    /* Only handle typing.Annotated types */
    if (mod->typing_annotated_alias != NULL) {
        if (Py_TYPE(annotation) != (PyTypeObject *)(mod->typing_annotated_alias))
            return NULL;
    }
    /* Check for __metadata__ without disturbing existing errors */
    int has_error = PyErr_Occurred() != NULL;
    if (!has_error) {
        PyObject *metadata = PyObject_GetAttr(annotation, mod->str___metadata__);
        if (metadata == NULL) {
            PyErr_Clear();
            return NULL;
        }
        if (!PyTuple_Check(metadata)) { Py_DECREF(metadata); return NULL; }
        Field *result = NULL;
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(metadata); i++) {
            PyObject *item = PyTuple_GET_ITEM(metadata, i);
            if (Py_TYPE(item) == &Field_Type) { result = (Field *)item; break; }
        }
        Py_DECREF(metadata);
        return result;
    } else {
        /* Save existing error, try to get metadata */
        PyObject *exc_type, *exc_value, *exc_tb;
        PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
        PyObject *metadata = PyObject_GetAttr(annotation, mod->str___metadata__);
        if (metadata == NULL) {
            /* Restore original error */
            PyErr_Restore(exc_type, exc_value, exc_tb);
            return NULL;
        }
        /* Found metadata - discard saved error */
        Py_XDECREF(exc_type);
        Py_XDECREF(exc_value);
        Py_XDECREF(exc_tb);
        if (!PyTuple_Check(metadata)) { Py_DECREF(metadata); return NULL; }
        Field *result = NULL;
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(metadata); i++) {
            PyObject *item = PyTuple_GET_ITEM(metadata, i);
            if (Py_TYPE(item) == &Field_Type) { result = (Field *)item; break; }
        }
        Py_DECREF(metadata);
        return result;
    }
}

static int
structmeta_process_rename(
    StructMetaInfo *info, StructspecState *mod,
    PyObject *name, PyObject *default_value
) {
    if (
        default_value != NULL &&
        Py_TYPE(default_value) == &Field_Type &&
        ((Field *)default_value)->alias != NULL
    ) {
        Field *field = (Field *)default_value;
        if (PyUnicode_Compare(name, field->alias) == 0) return 0;
        return PyDict_SetItem(info->renamed_fields, name, field->alias);
    }

    /* Also check annotation for Field with alias */
    if (info->resolved_annotations != NULL) {
        PyObject *annotation = PyDict_GetItem(info->resolved_annotations, name);
        if (annotation != NULL) {
            Field *field = extract_field_from_annotated(annotation, mod);
            if (field != NULL && field->alias != NULL) {
                if (PyUnicode_Compare(name, field->alias) == 0) return 0;
                return PyDict_SetItem(info->renamed_fields, name, field->alias);
            }
        }
    }

    if (info->rename == NULL) return 0;

    PyObject* (*method)(PyObject *, PyObject *);
    if (PyUnicode_CheckExact(info->rename)) {
        if (PyUnicode_CompareWithASCIIString(info->rename, "lower") == 0) {
            method = &rename_lower;
        }
        else if (PyUnicode_CompareWithASCIIString(info->rename, "upper") == 0) {
            method = &rename_upper;
        }
        else if (PyUnicode_CompareWithASCIIString(info->rename, "camel") == 0) {
            method = &rename_camel;
        }
        else if (PyUnicode_CompareWithASCIIString(info->rename, "pascal") == 0) {
            method = &rename_pascal;
        }
        else if (PyUnicode_CompareWithASCIIString(info->rename, "kebab") == 0) {
            method = &rename_kebab;
        }
        else {
            PyErr_Format(PyExc_ValueError, "rename='%U' is unsupported", info->rename);
            return -1;
        }
    }
    else if (PyCallable_Check(info->rename)) {
        method = &rename_callable;
    }
    else if (PyMapping_Check(info->rename)) {
        method = &rename_mapping;
    }
    else {
        PyErr_SetString(PyExc_TypeError, "`rename` must be a str, callable, or mapping");
        return -1;
    }

    PyObject *temp = method(info->rename, name);
    if (temp == NULL) return -1;
    int out = 0;
    if (PyUnicode_Compare(name, temp) != 0) {
        out = PyDict_SetItem(info->renamed_fields, name, temp);
    }
    Py_DECREF(temp);
    return out;
}

static int
structmeta_process_default(StructMetaInfo *info, StructspecState *mod, PyObject *name) {
    PyObject *obj = PyDict_GetItem(info->namespace, name);
    PyObject *default_val = NULL;
    PyTypeObject *type;
    if (structmeta_process_rename(info, mod, name, obj) < 0) return -1;

    /* Error: Field used outside Annotated */
    if (obj != NULL && Py_TYPE(obj) == &Field_Type) {
        PyErr_SetString(PyExc_TypeError,
            "`Field` can only be used inside `typing.Annotated`");
        return -1;
    }

    if (obj == NULL) {
        return PyDict_SetItem(info->defaults_lk, name, NODEFAULT);
    }

    type = Py_TYPE(obj);

    if (type == &PyDict_Type) {
        if (PyDict_GET_SIZE(obj) != 0) goto error_nonempty;
        default_val = Factory_New((PyObject*)(&PyDict_Type));
        if (default_val == NULL) return -1;
    }
    else if (type == &PyList_Type) {
        if (PyList_GET_SIZE(obj) != 0) goto error_nonempty;
        default_val = Factory_New((PyObject*)(&PyList_Type));
        if (default_val == NULL) return -1;
    }
    else if (type == &PySet_Type) {
        if (PySet_GET_SIZE(obj) != 0) goto error_nonempty;
        default_val = Factory_New((PyObject*)(&PySet_Type));
        if (default_val == NULL) return -1;
    }
    else if (type == &PyByteArray_Type) {
        if (PyByteArray_GET_SIZE(obj) != 0) goto error_nonempty;
        default_val = Factory_New((PyObject*)(&PyByteArray_Type));
        if (default_val == NULL) return -1;
    }
    else if (
        ms_is_struct_type(type) &&
        ((StructMetaObject *)type)->frozen != OPT_TRUE
    ) {
        goto error_mutable_struct;
    }
    else {
        Py_INCREF(obj);
        default_val = obj;
    }

    if (dict_discard(info->namespace, name) < 0) {
        Py_DECREF(default_val);
        return -1;
    }
    int status = PyDict_SetItem(info->defaults_lk, name, default_val);
    Py_DECREF(default_val);
    return status;

error_nonempty:
    PyErr_Format(
        PyExc_TypeError,
        "Using a non-empty mutable collection (%R) as a default value is unsafe. "
        "Instead configure a `Factory` for this field.",
        obj
    );
    return -1;

error_mutable_struct:
    PyErr_Format(
        PyExc_TypeError,
        "Using a mutable struct object (%R) as a default value is unsafe. "
        "Either configure a `Factory` for this field, or set "
        "`frozen=True` on `%.200s`",
        obj, type->tp_name
    );
    return -1;
}

static int
structmeta_is_classvar(
    StructMetaInfo *info, StructspecState *mod, PyObject *ann, PyObject **module_ns
) {
    PyTypeObject *ann_type = Py_TYPE(ann);
    if (ann_type == &PyUnicode_Type) {
        Py_ssize_t ann_len;
        const char *ann_buf = unicode_str_and_size(ann, &ann_len);
        if (ann_len < 8) return 0;
        if (memcmp(ann_buf, "ClassVar", 8) == 0) {
            if (ann_len != 8 && ann_buf[8] != '[') return 0;
            if (*module_ns == NULL) {
                *module_ns = structmeta_get_module_ns(mod, info);
            }
            if (*module_ns == NULL) return 0;
            PyObject *temp = PyDict_GetItemString(*module_ns, "ClassVar");
            return temp == mod->typing_classvar;
        }
        if (ann_len < 15) return 0;
        if (memcmp(ann_buf, "typing.ClassVar", 15) == 0) {
            if (ann_len != 15 && ann_buf[15] != '[') return 0;
            if (*module_ns == NULL) {
                *module_ns = structmeta_get_module_ns(mod, info);
            }
            if (*module_ns == NULL) return 0;
            PyObject *temp = PyDict_GetItemString(*module_ns, "typing");
            if (temp == NULL) return 0;
            temp = PyObject_GetAttrString(temp, "ClassVar");
            if (temp == NULL) return -1;
            int status = temp == mod->typing_classvar;
            Py_DECREF(temp);
            return status;
        }
    }
    else {
        if (ann == mod->typing_classvar) {
            return 1;
        }
        else if ((PyObject *)ann_type == mod->typing_generic_alias) {
            PyObject *temp = PyObject_GetAttr(ann, mod->str___origin__);
            if (temp == NULL) return -1;
            int status = temp == mod->typing_classvar;
            Py_DECREF(temp);
            return status;
        }
        return 0;
    }
    return 0;
}

static bool
ms_is_single_custom_type(PyObject *t, StructspecState *mod) {
    TypeNodeCollectState state = {0};
    state.mod = mod;
    state.context = t;
    bool out = false;
    if (typenode_collect_type(&state, t) < 0) {
        PyErr_Clear();
        goto done;
    }
    out = (
        state.custom_obj != NULL &&
        (state.types & ~(MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC | MS_CONSTR_CODEC)) == 0
    );
done:
    typenode_collect_clear_state(&state);
    return out;
}

/* Add `dump` for `base_type` to a per-field codec map, erroring when the same
 * base type is registered with a different `dump` within one field. */
static int
codec_map_set(PyObject *codecs, PyObject *base_type, PyObject *dump, PyObject *ctx) {
    PyObject *existing = PyDict_GetItem(codecs, base_type);
    if (existing != NULL) {
        if (existing != dump) {
            PyErr_Format(
                PyExc_TypeError,
                "`Field(dump=...)` codecs for type `%R` conflict within a single "
                "field - type `%R` is invalid",
                base_type, ctx
            );
            return -1;
        }
        return 0;
    }
    return PyDict_SetItem(codecs, base_type, dump);
}

/* Recursively walk an annotation, collecting Field codecs into a per-field
 * codec map and validating that each codec is attached to a custom type. */
static int
codec_walk_annotation(PyObject *ann, PyObject *codecs, StructspecState *mod, PyObject *ctx) {
    PyObject *t = ann;
    Py_INCREF(t);
    int out = -1;

    /* Strip Annotated wrappers, collecting Field metadata at this level. */
    while (Py_TYPE(t) == (PyTypeObject *)(mod->typing_annotated_alias)) {
        PyObject *origin = PyObject_GetAttr(t, mod->str___origin__);
        if (origin == NULL) goto error;
        PyObject *metadata = PyObject_GetAttr(t, mod->str___metadata__);
        if (metadata == NULL) {
            Py_DECREF(origin);
            goto error;
        }
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(metadata); i++) {
            PyObject *item = PyTuple_GET_ITEM(metadata, i);
            if (Py_TYPE(item) != &Field_Type) continue;
            Field *field = (Field *)item;
            if (field->dump == NULL && field->validate == NULL) continue;
            if (!ms_is_single_custom_type(origin, mod)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "`Field(dump=...)`/`Field(validate=...)` codecs can only be "
                    "used on custom types - type `%R` is invalid",
                    origin
                );
                Py_DECREF(metadata);
                Py_DECREF(origin);
                goto error;
            }
            if (field->dump != NULL) {
                if (codec_map_set(codecs, origin, field->dump, ctx) < 0) {
                    Py_DECREF(metadata);
                    Py_DECREF(origin);
                    goto error;
                }
            }
        }
        Py_DECREF(metadata);
        Py_SETREF(t, origin);
    }

    /* Strip NewType / TypeAliasType / Final wrappers. */
    PyObject *supertype = PyObject_GetAttr(t, mod->str___supertype__);
    if (supertype != NULL) {
        Py_SETREF(t, supertype);
    }
    else {
        PyErr_Clear();
    }
#if PY312_PLUS
    if (Py_TYPE(t) == (PyTypeObject *)(mod->typing_typealiastype)) {
        PyObject *value = PyObject_GetAttr(t, mod->str___value__);
        if (value == NULL) goto error;
        Py_SETREF(t, value);
    }
#endif

    /* Recurse into container/union args. */
    PyObject *origin = PyObject_GetAttr(t, mod->str___origin__);
    if (origin != NULL) {
        PyObject *args = PyObject_GetAttr(t, mod->str___args__);
        if (args != NULL && PyTuple_Check(args)) {
            for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(args); i++) {
                PyObject *arg = PyTuple_GET_ITEM(args, i);
                if (arg == Py_Ellipsis) continue;
                if (codec_walk_annotation(arg, codecs, mod, ctx) < 0) {
                    Py_DECREF(args);
                    Py_DECREF(origin);
                    goto error;
                }
            }
            Py_DECREF(args);
        }
        else {
            Py_XDECREF(args);
            PyErr_Clear();
        }
        Py_DECREF(origin);
    }
    else {
        PyErr_Clear();
    }

    out = 0;
error:
    Py_DECREF(t);
    return out;
}

/* Build per-field codec maps ({base_type: dump}) from field annotations.
 * Validates at class creation that codecs are on custom types and that a
 * single field doesn't register two different dumps for one type. Fields
 * inherited from struct base classes keep their base codec maps. */
static int
structmeta_collect_codecs(StructMetaInfo *info, StructspecState *mod, PyObject *bases) {
    Py_ssize_t nfields = PyTuple_GET_SIZE(info->fields);
    PyObject *maps = NULL;
    int out = -1;

    if (nfields == 0) return 0;

    /* Build a {field: codec map} lookup from struct base classes so inherited
     * fields keep their codecs. Bases are merged in reverse order so the
     * primary base wins on conflicts, matching structmeta_collect_base. */
    PyObject *inherited = PyDict_New();
    if (inherited == NULL) goto done;
    for (Py_ssize_t b = PyTuple_GET_SIZE(bases) - 1; b >= 0; b--) {
        PyObject *base = PyTuple_GET_ITEM(bases, b);
        if (!ms_is_struct_cls(base)) continue;
        StructMetaObject *st_type = (StructMetaObject *)base;
        if (st_type->struct_field_codecs == NULL) continue;
        PyObject *base_fields = st_type->struct_fields;
        PyObject *base_codecs = st_type->struct_field_codecs;
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(base_fields); i++) {
            PyObject *map = PyTuple_GET_ITEM(base_codecs, i);
            if (map == NULL) continue;
            PyObject *field = PyTuple_GET_ITEM(base_fields, i);
            if (PyDict_SetItem(inherited, field, map) < 0) {
                Py_DECREF(inherited);
                goto done;
            }
        }
    }

    maps = PyTuple_New(nfields);
    if (maps == NULL) {
        Py_DECREF(inherited);
        goto done;
    }
    bool any = false;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(info->fields, i);
        PyObject *ann = (info->resolved_annotations != NULL)
            ? PyDict_GetItem(info->resolved_annotations, field)
            : NULL;
        PyObject *map = NULL;
        if (ann != NULL) {
            map = PyDict_New();
            if (map == NULL) {
                Py_DECREF(inherited);
                Py_DECREF(maps);
                goto done;
            }
            if (codec_walk_annotation(ann, map, mod, field) < 0) {
                Py_DECREF(map);
                Py_DECREF(inherited);
                Py_DECREF(maps);
                goto done;
            }
            if (PyDict_GET_SIZE(map) != 0) {
                any = true;
            }
            else {
                Py_DECREF(map);
                map = NULL;
            }
        }
        else {
            PyObject *inh = PyDict_GetItem(inherited, field);
            if (inh != NULL) {
                Py_INCREF(inh);
                map = inh;
                any = true;
            }
        }
        PyTuple_SET_ITEM(maps, i, map);
    }
    Py_DECREF(inherited);
    if (!any) {
        Py_DECREF(maps);
        maps = NULL;
    }
    Py_XSETREF(info->codec_maps, maps);
    out = 0;
done:
    return out;
}

static int
structmeta_collect_fields(StructMetaInfo *info, StructspecState *mod, bool kwonly) {
    PyObject *annotations = PyDict_GetItemString(  // borrowed reference
        info->namespace, "__annotations__"
    );
    if (annotations == NULL) {
        PyObject *annotate = ms_get_annotate_from_class_namespace(info->namespace);
        if (annotate == NULL) {
            return -1;
        }
        if (annotate == Py_None) {
            Py_DECREF(annotate);
            return 0;
        }
        PyObject *format = PyLong_FromLong(1);  /* annotationlib.Format.VALUE */
        if (format == NULL) {
            Py_DECREF(annotate);
            return -1;
        }
        annotations = PyObject_CallOneArg(
            annotate, format
        );
        Py_DECREF(annotate);
        Py_DECREF(format);
        if (annotations == NULL) {
            return -1;
        }
    } else {
        Py_INCREF(annotations);
    }

    if (!PyDict_Check(annotations)) {
        Py_DECREF(annotations);
        PyErr_SetString(PyExc_TypeError, "__annotations__ must be a dict");
        return -1;
    }

    /* Copy annotations for Field extraction */
    info->resolved_annotations = PyDict_Copy(annotations);

    PyObject *module_ns = NULL;
    PyObject *field, *value;
    Py_ssize_t i = 0;
    while (PyDict_Next(annotations, &i, &field, &value)) {
        if (!PyUnicode_CheckExact(field)) {
            PyErr_SetString(
                PyExc_TypeError, "__annotations__ keys must be strings"
            );
            goto error;
        }

        PyObject *invalid_field_names[] = {
            mod->str___weakref__, mod->str___dict__, mod->str___structtype_cached_hash__
        };
        for (int i = 0; i < 3; i++) {
            if (PyUnicode_Compare(field, invalid_field_names[i]) == 0) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Cannot have a struct field named %R",
                    field
                );
                goto error;
            }
        }

        int status = structmeta_is_classvar(info, mod, value, &module_ns);
        if (status == 1) continue;
        if (status == -1) goto error;

        /* If the field is new, add it to slots */
        if (PyDict_GetItem(info->defaults_lk, field) == NULL) {
            if (PyList_Append(info->slots, field) < 0) goto error;
        }

        if (kwonly) {
            if (PySet_Add(info->kwonly_fields, field) < 0) goto error;
        }
        else {
            if (PySet_Discard(info->kwonly_fields, field) < 0) goto error;
        }

        if (structmeta_process_default(info, mod, field) < 0) goto error;
    }
    Py_DECREF(annotations);
    return 0;
error:
    Py_DECREF(annotations);
    Py_XDECREF(module_ns);
    return -1;
}

static int
structmeta_construct_fields(StructMetaInfo *info, StructspecState *mod) {
    Py_ssize_t nfields = PyDict_GET_SIZE(info->defaults_lk);
    Py_ssize_t nkwonly = PySet_GET_SIZE(info->kwonly_fields);
    Py_ssize_t field_index = 0;

    info->fields = PyTuple_New(nfields);
    if (info->fields == NULL) return -1;
    info->defaults = PyList_New(0);

    /* First pass - handle all non-kwonly fields. */
    PyObject *field, *default_val;
    Py_ssize_t pos = 0;
    while (PyDict_Next(info->defaults_lk, &pos, &field, &default_val)) {
        int kwonly = PySet_Contains(info->kwonly_fields, field);
        if (kwonly < 0) return -1;
        if (kwonly) continue;

        Py_INCREF(field);
        PyTuple_SET_ITEM(info->fields, field_index, field);

        if (default_val == NODEFAULT) {
            if (PyList_GET_SIZE(info->defaults)) {
                PyErr_Format(
                    PyExc_TypeError,
                    "Required field '%U' cannot follow optional fields. Either "
                    "reorder the struct fields, or set `kw_only=True` in the "
                    "struct definition.",
                    field
                );
                return -1;
            }
        }
        else {
            if (PyList_Append(info->defaults, default_val) < 0) return -1;
        }
        field_index++;
    }
    /* Next handle any kw_only fields */
    if (nkwonly) {
        PyObject *field, *default_val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(info->defaults_lk, &pos, &field, &default_val)) {
            int kwonly = PySet_Contains(info->kwonly_fields, field);
            if (kwonly < 0) return -1;
            if (!kwonly) continue;

            Py_INCREF(field);
            PyTuple_SET_ITEM(info->fields, field_index, field);
            if (PyList_GET_SIZE(info->defaults) || default_val != NODEFAULT) {
                if (PyList_Append(info->defaults, default_val) < 0) return -1;
            }
            field_index++;
        }
    }

    /* Convert defaults list to tuple */
    PyObject *temp_defaults = PyList_AsTuple(info->defaults);
    Py_DECREF(info->defaults);
    info->defaults = temp_defaults;
    if (info->defaults == NULL) return -1;

    /* Compute n_trailing_defaults */
    info->nkwonly = nkwonly;
    info->n_trailing_defaults = 0;
    for (Py_ssize_t i = PyTuple_GET_SIZE(info->defaults) - 1; i >= 0; i--) {
        if (PyTuple_GET_ITEM(info->defaults, i) == NODEFAULT) break;
        info->n_trailing_defaults++;
    }

    /* Construct __match_args__ */
    info->match_args = PyTuple_GetSlice(info->fields, 0, nfields - nkwonly);
    if (info->match_args == NULL) return -1;

    /* Construct __slots__ */
    if (info->weakref == OPT_TRUE && !info->already_has_weakref) {
        if (PyList_Append(info->slots, mod->str___weakref__) < 0) return -1;
    }
    else if (info->weakref == OPT_FALSE && info->already_has_weakref) {
        PyErr_SetString(
            PyExc_ValueError,
            "Cannot set `weakref=False` if base class already has `weakref=True`"
        );
        return -1;
    }
    if (info->dict == OPT_TRUE && !info->already_has_dict) {
        if (PyList_Append(info->slots, mod->str___dict__) < 0) return -1;
    }
    else if (info->dict == OPT_FALSE && info->already_has_dict) {
        PyErr_SetString(
            PyExc_ValueError,
            "Cannot set `dict=False` if base class already has `dict=True`"
        );
        return -1;
    }
    if (info->cache_hash == OPT_TRUE && !info->hash_offset) {
        if (PyList_Append(info->slots, mod->str___structtype_cached_hash__) < 0) return -1;
    }
    else if (info->cache_hash == OPT_FALSE && info->hash_offset) {
        PyErr_SetString(
            PyExc_ValueError,
            "Cannot set `cache_hash=False` if base class already has `cache_hash=True`"
        );
        return -1;
    }

    if (PyList_Sort(info->slots) < 0) return -1;

    PyObject *slots = PyList_AsTuple(info->slots);
    if (slots == NULL) return -1;
    int out = PyDict_SetItemString(info->namespace, "__slots__", slots);
    Py_DECREF(slots);
    return out;
}


static int json_str_requires_escaping(PyObject *);

static int
structmeta_construct_encode_fields(StructMetaInfo *info)
{
    if (PyDict_GET_SIZE(info->renamed_fields) == 0) {
        /* Nothing to do, use original field tuple */
        Py_INCREF(info->fields);
        info->encode_fields = info->fields;
        return 0;
    }

    info->encode_fields = PyTuple_New(PyTuple_GET_SIZE(info->fields));
    if (info->encode_fields == NULL) return -1;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(info->fields); i++) {
        PyObject *name = PyTuple_GET_ITEM(info->fields, i);
        PyObject *temp = PyDict_GetItem(info->renamed_fields, name);
        if (temp == NULL) {
            temp = name;
        }
        Py_INCREF(temp);
        PyTuple_SET_ITEM(info->encode_fields, i, temp);
    }

    /* Ensure that renamed fields don't collide */
    PyObject *fields_set = PySet_New(info->encode_fields);
    if (fields_set == NULL) return -1;
    bool unique = PySet_GET_SIZE(fields_set) == PyTuple_GET_SIZE(info->encode_fields);
    Py_DECREF(fields_set);
    if (!unique) {
        PyErr_SetString(
            PyExc_ValueError,
            "Multiple fields rename to the same name, field names "
            "must be unique"
        );
        return -1;
    }

    /* Ensure all renamed fields contain characters that don't require quoting
     * in JSON. This isn't strictly required, but usage of such characters is
     * extremely unlikely, and forbidding this allows us to optimize encoding */
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(info->encode_fields); i++) {
        PyObject *field = PyTuple_GET_ITEM(info->encode_fields, i);
        Py_ssize_t status = json_str_requires_escaping(field);
        if (status == -1) return -1;
        if (status == 1) {
            PyErr_Format(
                PyExc_ValueError,
                "Renamed field names must not contain '\\', '\"', or control characters "
                "('\\u0000' to '\\u001F') - '%U' is invalid",
                field
            );
            return -1;
        }
    }
    return 0;
}

/* Precompute raw UTF-8 views of the struct's encode field names and tag field
 * so the JSON encoder can write keys without touching Python str objects at
 * encode time. Field names are guaranteed to be escape-free (identifiers, or
 * validated by structmeta_construct_encode_fields), so callers may write them
 * with the noescape path. The tag field may contain arbitrary characters and
 * must be written with the escaping path. */
static int
structmeta_construct_encode_key_views(StructMetaObject *cls)
{
    cls->struct_encode_keys = NULL;
    cls->struct_tag_field_view.buf = NULL;
    cls->struct_tag_field_view.size = 0;

    if (cls->struct_tag_field != NULL) {
        cls->struct_tag_field_view.buf = unicode_str_and_size(
            cls->struct_tag_field, &cls->struct_tag_field_view.size
        );
        if (cls->struct_tag_field_view.buf == NULL) return -1;
    }

    Py_ssize_t nfields = PyTuple_GET_SIZE(cls->struct_encode_fields);
    if (nfields == 0) return 0;

    cls->struct_encode_keys = PyMem_Malloc(nfields * sizeof(MS_StrView));
    if (cls->struct_encode_keys == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(cls->struct_encode_fields, i);
        /* Use the checked variant to force UTF-8 materialization for
         * non-ASCII names; the nocheck variant may return a NULL utf8
         * pointer before the cache is populated. */
        cls->struct_encode_keys[i].buf = unicode_str_and_size(field, &cls->struct_encode_keys[i].size);
        if (cls->struct_encode_keys[i].buf == NULL) return -1;
    }
    return 0;
}

/* Extracts the qualname for a class, and strips off any leading bits from a
 * function namespace. Examples:
 *
 * - `Foo` -> `Foo`
 * - `Foo.Bar` -> `Foo.Bar`
 * - `fizz.<locals>.Foo.Bar` -> `Foo.Bar`
 *
 * This means that (nested) classes dynamically defined within functions should
 * work the same as (nested) classes defined at the top level.
 */
static PyObject *simple_qualname(PyObject *cls) {
    PyObject *qualname = NULL, *dotlocalsdot = NULL, *rsplits = NULL, *out = NULL;

    qualname = PyObject_GetAttrString(cls, "__qualname__");
    if (qualname == NULL) goto cleanup;

    dotlocalsdot = PyUnicode_FromString(".<locals>.");
    if (dotlocalsdot == NULL) goto cleanup;

    rsplits = PyUnicode_RSplit(qualname, dotlocalsdot, 1);
    if (rsplits == NULL) goto cleanup;

    Py_ssize_t end = PyList_GET_SIZE(rsplits) - 1;
    out = PyList_GET_ITEM(rsplits, end);
    Py_INCREF(out);

cleanup:
    Py_XDECREF(qualname);
    Py_XDECREF(dotlocalsdot);
    Py_XDECREF(rsplits);
    return out;
}

static int
structmeta_construct_tag(StructMetaInfo *info, StructspecState *mod, PyObject *cls) {
    if (info->temp_tag == Py_False) return 0;
    if (info->temp_tag == NULL && info->temp_tag_field == NULL) return 0;

    Py_XINCREF(info->temp_tag);
    info->tag = info->temp_tag;

    /* Determine the tag value */
    if (info->temp_tag == NULL || info->temp_tag == Py_True) {
        info->tag_value = simple_qualname(cls);
        if (info->tag_value == NULL) return -1;
    }
    else {
        if (PyCallable_Check(info->temp_tag)) {
            PyObject *qualname = simple_qualname(cls);
            if (qualname == NULL) return -1;
            info->tag_value = PyObject_CallOneArg(info->temp_tag, qualname);
            Py_DECREF(qualname);
            if (info->tag_value == NULL) return -1;
        }
        else {
            Py_INCREF(info->temp_tag);
            info->tag_value = info->temp_tag;
        }

        if (PyLong_CheckExact(info->tag_value)) {
            int64_t val = PyLong_AsLongLong(info->tag_value);
            if (val == -1 && PyErr_Occurred()) {
                PyErr_SetString(
                    PyExc_ValueError,
                    "Integer `tag` values must be within [-2**63, 2**63 - 1]"
                );
                return -1;
            }
        }
        else if (!PyUnicode_CheckExact(info->tag_value)) {
            PyErr_SetString(PyExc_TypeError, "`tag` must be a `str` or an `int`");
            return -1;
        }
    }

    /* Next determine the tag_field to use. */
    if (info->temp_tag_field == NULL) {
        info->tag_field = mod->str_type;
        Py_INCREF(info->tag_field);
    }
    else if (PyUnicode_CheckExact(info->temp_tag_field)) {
        info->tag_field = info->temp_tag_field;
        Py_INCREF(info->tag_field);
    }
    else {
        PyErr_SetString(PyExc_TypeError, "`tag_field` must be a `str`");
        return -1;
    }
    int contains = PySequence_Contains(info->encode_fields, info->tag_field);
    if (contains < 0) return -1;
    if (contains) {
        PyErr_Format(
            PyExc_ValueError,
            "`tag_field='%U' conflicts with an existing field of that name",
            info->tag_field
        );
        return -1;
    }
    return 0;
}

static int
structmeta_construct_offsets(
    StructMetaInfo *info, StructspecState *mod, StructMetaObject *cls
) {
    PyMemberDef *mp = MS_PyHeapType_GET_MEMBERS(cls);
    for (Py_ssize_t i = 0; i < Py_SIZE(cls); i++, mp++) {
        PyObject *offset = PyLong_FromSsize_t(mp->offset);
        if (offset == NULL) return -1;
        bool errored = PyDict_SetItemString(info->offsets_lk, mp->name, offset) < 0;
        Py_DECREF(offset);
        if (errored) return -1;
    }

    info->offsets = PyMem_New(Py_ssize_t, PyTuple_GET_SIZE(info->fields));
    if (info->offsets == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(info->fields); i++) {
        PyObject *field = PyTuple_GET_ITEM(info->fields, i);
        PyObject *offset = PyDict_GetItem(info->offsets_lk, field);
        if (offset == NULL) {
            PyErr_Format(PyExc_RuntimeError, "Failed to get offset for %R", field);
            return -1;
        }
        info->offsets[i] = PyLong_AsSsize_t(offset);
    }

    if (info->cache_hash == OPT_TRUE && info->hash_offset == 0) {
        PyObject *offset = PyDict_GetItem(
            info->offsets_lk, mod->str___structtype_cached_hash__
        );
        if (offset == NULL) {
            PyErr_Format(
                PyExc_RuntimeError, "Failed to get offset for %R", mod->str___structtype_cached_hash__
            );
            return -1;
        }
        info->hash_offset = PyLong_AsSsize_t(offset);
    }
    return 0;
}


static PyObject *
StructMeta_new_inner(
    PyTypeObject *type, PyObject *name, PyObject *bases, PyObject *namespace,
    PyObject *arg_tag_field, PyObject *arg_tag, PyObject *arg_rename,
    int arg_omit_defaults, int arg_forbid_unknown_fields,
    int arg_frozen, int arg_eq, int arg_order, bool arg_kw_only,
    int arg_repr_omit_defaults, int arg_array_like,
    int arg_gc, int arg_weakref, int arg_dict, int arg_cache_hash,
    int arg_validate_on_init
) {
    StructMetaObject *cls = NULL;
    StructspecState *mod = structtype_get_global_state();
    bool ok = false;

    if (structmeta_check_namespace(namespace) < 0) return NULL;

    StructMetaInfo info = {
        .defaults_lk = NULL,
        .offsets_lk = NULL,
        .kwonly_fields = NULL,
        .slots = NULL,
        .namespace = NULL,
        .renamed_fields = NULL,
        .fields = NULL,
        .encode_fields = NULL,
        .defaults = NULL,
        .match_args = NULL,
        .tag = NULL,
        .tag_field = NULL,
        .tag_value = NULL,
        .offsets = NULL,
        .nkwonly = 0,
        .n_trailing_defaults = 0,
        .name = name,
        .temp_tag_field = NULL,
        .temp_tag = NULL,
        .rename = NULL,
        .omit_defaults = -1,
        .forbid_unknown_fields = -1,
        .validate_on_init = -1,
        .frozen = -1,
        .eq = -1,
        .order = -1,
        .repr_omit_defaults = -1,
        .array_like = -1,
        .gc = -1,
        .weakref = arg_weakref,
        .already_has_weakref = false,
        .dict = arg_dict,
        .already_has_dict = false,
        .cache_hash = arg_cache_hash,
        .hash_offset = 0,
        .has_non_slots_bases = false,
        .resolved_annotations = NULL,
    };

    info.defaults_lk = PyDict_New();
    if (info.defaults_lk == NULL) goto cleanup;
    info.offsets_lk = PyDict_New();
    if (info.offsets_lk == NULL) goto cleanup;
    info.kwonly_fields = PySet_New(NULL);
    if (info.kwonly_fields == NULL) goto cleanup;
    info.namespace = PyDict_Copy(namespace);
    if (info.namespace == NULL) goto cleanup;
    info.renamed_fields = PyDict_New();
    if (info.renamed_fields == NULL) goto cleanup;
    info.slots = PyList_New(0);
    if (info.slots == NULL) goto cleanup;

    /* Extract info from base classes in reverse MRO order */
    for (Py_ssize_t i = PyTuple_GET_SIZE(bases) - 1; i >= 0; i--) {
        PyObject *base = PyTuple_GET_ITEM(bases, i);
        if (structmeta_collect_base(&info, mod, base) < 0) goto cleanup;
    }

    /* Process configuration options */
    if (arg_tag != NULL && arg_tag != Py_None) {
        info.temp_tag = arg_tag;
    }
    if (arg_tag_field != NULL && arg_tag_field != Py_None) {
        info.temp_tag_field = arg_tag_field;
    }
    if (arg_rename != NULL) {
        info.rename = arg_rename == Py_None ? NULL : arg_rename;
    }
    info.frozen = STRUCT_MERGE_OPTIONS(info.frozen, arg_frozen);
    info.eq = STRUCT_MERGE_OPTIONS(info.eq, arg_eq);
    info.order = STRUCT_MERGE_OPTIONS(info.order, arg_order);
    info.repr_omit_defaults = STRUCT_MERGE_OPTIONS(info.repr_omit_defaults, arg_repr_omit_defaults);
    info.array_like = STRUCT_MERGE_OPTIONS(info.array_like, arg_array_like);
    info.gc = STRUCT_MERGE_OPTIONS(info.gc, arg_gc);
    info.omit_defaults = STRUCT_MERGE_OPTIONS(info.omit_defaults, arg_omit_defaults);
    info.forbid_unknown_fields = STRUCT_MERGE_OPTIONS(info.forbid_unknown_fields, arg_forbid_unknown_fields);
    info.validate_on_init = STRUCT_MERGE_OPTIONS(info.validate_on_init, arg_validate_on_init);

    if (info.eq == OPT_FALSE && info.order == OPT_TRUE) {
        PyErr_SetString(PyExc_ValueError, "Cannot set eq=False and order=True");
        goto cleanup;
    }

    if (info.cache_hash == OPT_TRUE && info.frozen != OPT_TRUE) {
        PyErr_SetString(PyExc_ValueError, "Cannot set cache_hash=True without frozen=True");
        goto cleanup;
    }

    if (info.gc == OPT_FALSE) {
        if (info.has_non_slots_bases) {
            PyErr_SetString(
                PyExc_ValueError,
                "Cannot set gc=False when inheriting from non-struct types with a __dict__"
            );
            goto cleanup;
        }
        else if (info.dict == OPT_TRUE || info.already_has_dict) {
            PyErr_SetString(PyExc_ValueError, "Cannot set gc=False and dict=True");
            goto cleanup;
        }
    }

    /* Collect new fields and defaults */
    if (structmeta_collect_fields(&info, mod, arg_kw_only) < 0) goto cleanup;

    /* Construct fields and defaults */
    if (structmeta_construct_fields(&info, mod) < 0) goto cleanup;

    /* Collect per-field codec maps from the resolved annotations */
    if (structmeta_collect_codecs(&info, mod, bases) < 0) goto cleanup;

    /* Construct encode_fields */
    if (structmeta_construct_encode_fields(&info) < 0) goto cleanup;

    /* Construct type */
    PyObject *args = Py_BuildValue("(OOO)", name, bases, info.namespace);
    if (args == NULL) goto cleanup;
    cls = (StructMetaObject *) PyType_Type.tp_new(type, args, NULL);
    Py_CLEAR(args);
    if (cls == NULL) goto cleanup;

    /* If the metaclass participates in abc.ABCMeta, initialise ABC
     * bookkeeping so issubclass/isinstance work correctly when the
     * metaclass is mixed with StructMeta. */
    if (mod != NULL && mod->ABCMetaType != NULL && mod->_abc_init != NULL) {
        int is_abc_meta = PyType_IsSubtype(type, mod->ABCMetaType);
        if (is_abc_meta < 0) goto cleanup;
        if (is_abc_meta) {
            PyObject *res = PyObject_CallOneArg(mod->_abc_init, (PyObject *)cls);
            if (res == NULL) goto cleanup;
            Py_DECREF(res);
        }
    }

    /* Fill in type methods */
    ((PyTypeObject *)cls)->tp_vectorcall = (vectorcallfunc)Struct_vectorcall;
    if (info.gc == OPT_FALSE) {
        ((PyTypeObject *)cls)->tp_flags &= ~Py_TPFLAGS_HAVE_GC;
        ((PyTypeObject *)cls)->tp_dealloc = &Struct_dealloc_nogc;
        ((PyTypeObject *)cls)->tp_free = &PyObject_Free;
    }
    else {
        ((PyTypeObject *)cls)->tp_flags |= Py_TPFLAGS_HAVE_GC;
        ((PyTypeObject *)cls)->tp_free = &PyObject_GC_Del;
    }
    if (info.frozen == OPT_TRUE) {
        /* Frozen structs always override __setattr__ */
        ((PyTypeObject *)cls)->tp_setattro = &Struct_setattro_frozen;
    }
    else if (
        ((PyTypeObject *)cls)->tp_setattro == Struct_setattro_frozen ||
        ((PyTypeObject *)cls)->tp_setattro == PyObject_GenericSetAttr
    ) {
        /* Only set non-frozen __setattr__ if it hasn't been defined by the
         * user. All structs will inherit `Struct_setattro_default` from the
         * base Struct class, so proper use of `super()` should still result in
         * this being called. */
        ((PyTypeObject *)cls)->tp_setattro = &Struct_setattro_default;
    }

    /* Construct tag, tag_field, & tag_value */
    if (structmeta_construct_tag(&info, mod, (PyObject *)cls) < 0) goto cleanup;

    /* Fill in struct offsets */
    if (structmeta_construct_offsets(&info, mod, cls) < 0) goto cleanup;

    /* Cache access to __post_init__ (if defined). */
    cls->post_init = PyObject_GetAttr((PyObject *)cls, mod->str___post_init__);
    if (cls->post_init == NULL) {
        PyErr_Clear();
    }

    cls->nkwonly = info.nkwonly;
    cls->n_trailing_defaults = info.n_trailing_defaults;
    cls->struct_offsets = info.offsets;
    Py_INCREF(info.fields);
    cls->struct_fields = info.fields;
    Py_XINCREF(info.codec_maps);
    cls->struct_field_codecs = info.codec_maps;
    Py_INCREF(info.defaults);
    cls->struct_defaults = info.defaults;
    Py_INCREF(info.encode_fields);
    cls->struct_encode_fields = info.encode_fields;
    Py_INCREF(info.match_args);
    cls->match_args = info.match_args;
    Py_XINCREF(info.tag);
    cls->struct_tag = info.tag;
    Py_XINCREF(info.tag_field);
    cls->struct_tag_field = info.tag_field;
    Py_XINCREF(info.tag_value);
    cls->struct_tag_value = info.tag_value;
    Py_XINCREF(info.rename);
    cls->rename = info.rename;

    if (structmeta_construct_encode_key_views(cls) < 0) goto cleanup;

    /* Cache the module on the type so methods like struct_dump_json /
     * struct_validate_json / struct_dump avoid a PyState_FindModule lookup
     * on every call. Types are per-interpreter heap objects, so this is
     * sub-interpreter safe. */
    cls->struct_module = PyState_FindModule(&structtypemodule);
    Py_XINCREF(cls->struct_module);

    cls->hash_offset = info.hash_offset;
    cls->frozen = info.frozen;
    cls->eq = info.eq;
    cls->order = info.order;
    cls->repr_omit_defaults = info.repr_omit_defaults;
    cls->array_like = info.array_like;
    cls->gc = info.gc;
    cls->omit_defaults = info.omit_defaults;
    cls->forbid_unknown_fields = info.forbid_unknown_fields;
    cls->validate_on_init = info.validate_on_init;

    

    ok = true;

cleanup:
    /* Temporary structures */
    Py_XDECREF(info.defaults_lk);
    Py_XDECREF(info.offsets_lk);
    Py_XDECREF(info.kwonly_fields);
    Py_XDECREF(info.slots);
    Py_XDECREF(info.namespace);
    Py_XDECREF(info.renamed_fields);
    Py_XDECREF(info.resolved_annotations);
    Py_XDECREF(info.codec_maps);
    /* Constructed outputs */
    Py_XDECREF(info.fields);
    Py_XDECREF(info.encode_fields);
    Py_XDECREF(info.defaults);
    Py_XDECREF(info.match_args);
    Py_XDECREF(info.tag);
    Py_XDECREF(info.tag_field);
    Py_XDECREF(info.tag_value);
    if (!ok) {
        if (info.offsets != NULL) {
            PyMem_Free(info.offsets);
            /* cls->struct_offsets aliases info.offsets; NULL it so dealloc
             * doesn't double-free if the error happened after assignment. */
            if (cls != NULL) cls->struct_offsets = NULL;
        }
        if (cls != NULL && cls->struct_encode_keys != NULL) {
            PyMem_Free(cls->struct_encode_keys);
            cls->struct_encode_keys = NULL;
        }
        Py_XDECREF(cls);
        return NULL;
    }
    return (PyObject *) cls;
}

static PyObject *
StructMeta_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyObject *name = NULL, *bases = NULL, *namespace = NULL;
    PyObject *arg_tag_field = NULL, *arg_tag = NULL, *arg_rename = NULL;
    int arg_omit_defaults = -1, arg_forbid_unknown_fields = -1;
    int arg_frozen = -1, arg_eq = -1, arg_order = -1, arg_repr_omit_defaults = -1;
    int arg_array_like = -1, arg_gc = -1, arg_weakref = -1, arg_dict = -1;
    int arg_kw_only = 0, arg_cache_hash = -1, arg_validate_on_init = -1;

    char *kwlist[] = {
        "name", "bases", "dict",
        "tag_field", "tag", "rename",
        "omit_defaults", "forbid_unknown_fields",
        "frozen", "eq", "order", "kw_only",
        "repr_omit_defaults", "array_like",
        "gc", "weakref", "dict", "cache_hash",
        "validate_on_init",
        NULL
    };

    /* Parse arguments: (name, bases, dict) */
    if (!PyArg_ParseTupleAndKeywords(
            args, kwargs, "UO!O!|$OOOppppppppppppp:StructMeta.__new__", kwlist,
            &name, &PyTuple_Type, &bases, &PyDict_Type, &namespace,
            &arg_tag_field, &arg_tag, &arg_rename,
            &arg_omit_defaults, &arg_forbid_unknown_fields,
            &arg_frozen, &arg_eq, &arg_order, &arg_kw_only,
            &arg_repr_omit_defaults, &arg_array_like,
            &arg_gc, &arg_weakref, &arg_dict, &arg_cache_hash,
            &arg_validate_on_init
        )
    )
        return NULL;

    return StructMeta_new_inner(
        type, name, bases, namespace,
        arg_tag_field, arg_tag, arg_rename,
        arg_omit_defaults, arg_forbid_unknown_fields,
        arg_frozen, arg_eq, arg_order, arg_kw_only,
        arg_repr_omit_defaults, arg_array_like,
        arg_gc, arg_weakref, arg_dict, arg_cache_hash,
        arg_validate_on_init
    );
}


static int
StructInfo_traverse(StructInfo *self, visitproc visit, void *arg)
{
    Py_VISIT(self->class);
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        int out = TypeNode_traverse(self->types[i], visit, arg);
        if (out != 0) return out;
    }
    return 0;
}

static int
StructInfo_clear(StructInfo *self)
{
    Py_CLEAR(self->class);
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        TypeNode_Free(self->types[i]);
        self->types[i] = NULL;
    }
    return 0;
}

static void
StructInfo_dealloc(StructInfo *self)
{
    PyObject_GC_UnTrack(self);
    StructInfo_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject StructInfo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.StructInfo",
    .tp_basicsize = sizeof(StructInfo),
    .tp_itemsize = sizeof(TypeNode *),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)StructInfo_clear,
    .tp_traverse = (traverseproc)StructInfo_traverse,
    .tp_dealloc = (destructor)StructInfo_dealloc,
};

static PyObject *
StructInfo_Convert_lock_held(PyObject *obj) {
    StructspecState *mod = structtype_get_global_state();
    StructMetaObject *class;
    PyObject *annotations = NULL;
    StructInfo *info = NULL;
    bool cache_set = false;
    bool is_struct = ms_is_struct_cls(obj);

    /* Check for a cached StructInfo, and return if one exists */
    if (MS_LIKELY(is_struct)) {
        class = (StructMetaObject *)obj;
        if (class->struct_info != NULL) {
            Py_INCREF(class->struct_info);
            return (PyObject *)(class->struct_info);
        }
        Py_INCREF(class);
    }
    else {
        PyObject *cached = NULL;
        if (get_structtype_cache(mod, obj, &StructInfo_Type, &cached)) {
            return cached;
        }
        PyObject *origin = PyObject_GetAttr(obj, mod->str___origin__);
        if (origin == NULL) return NULL;
        if (!ms_is_struct_cls(origin)) {
            Py_DECREF(origin);
            PyErr_SetString(
                PyExc_RuntimeError, "Expected __origin__ to be a Struct type"
            );
            return NULL;
        }
        class = (StructMetaObject *)origin;
    }

    /* At this point `class` is a StructMetaObject, and `obj` is a
     * StructMetaObject or Generic. `class` has already been incref'd */

    /* Ensure the StructMetaObject is fully initialized */
    if (MS_UNLIKELY(class->struct_fields == NULL)) {
        PyErr_Format(
            PyExc_ValueError,
            "Type `%R` isn't fully defined, and can't be used in any "
            "`Decoder`/`decode` operations. This commonly happens when "
            "trying to use the struct type within an `__init_subclass__` "
            "method. If you believe what you're trying to do should work, "
            "please raise an issue on GitHub.",
            (PyObject *)class
        );
        goto error;
    }

    /* Extract annotations from the original type object */
    annotations = PyObject_CallOneArg(mod->get_class_annotations, obj);
    if (annotations == NULL) goto error;

    /* Allocate and zero-out a new StructInfo */
    Py_ssize_t nfields = PyTuple_GET_SIZE(class->struct_fields);
    info = PyObject_GC_NewVar(StructInfo, &StructInfo_Type, nfields);
    if (info == NULL) goto error;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        info->types[i] = NULL;
    }
#ifdef Py_GIL_DISABLED
    atomic_store(&info->initialized, 0);
#endif
    Py_INCREF(class);
    info->class = class;

    /* Cache the new StuctInfo on the original type annotation */
    if (is_struct) {
        Py_INCREF(info);
        class->struct_info = info;
    }
    else {
        if (PyObject_SetAttr(obj, mod->str___structtype_cache__, (PyObject *)info) < 0) goto error;
    }
    cache_set = true;

    /* Process all the struct fields */
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(class->struct_fields, i);
        PyObject *field_type = PyDict_GetItem(annotations, field);
        if (field_type == NULL) goto error;
        TypeNode *type = TypeNode_Convert(field_type);
        if (type == NULL) goto error;
        info->types[i] = type;
    }

    Py_DECREF(class);
    Py_DECREF(annotations);
    PyObject_GC_Track(info);
#ifdef Py_GIL_DISABLED
    atomic_store(&info->initialized, 1);
#endif
    return (PyObject *)info;

error:
    if (cache_set) {
        /* An error occurred after the cache was created and set on the object.
         * We need to delete the cached value. */
        if (is_struct) {
            Py_CLEAR(class->struct_info);
        }
        else {
            /* Fetch and restore the original exception to avoid DelAttr
             * silently clearing it on rare occasions. */
            PyObject *err_type, *err_value, *err_tb;
            PyErr_Fetch(&err_type, &err_value, &err_tb);
            PyObject_DelAttr(obj, mod->str___structtype_cache__);
            PyErr_Restore(err_type, err_value, err_tb);
        }
    }
    Py_DECREF(class);
    Py_XDECREF(annotations);
    Py_XDECREF(info);
    return NULL;
}

static PyObject *
StructInfo_Convert(PyObject *obj) {
    PyObject *res = NULL;
    Py_BEGIN_CRITICAL_SECTION(obj);
    res = StructInfo_Convert_lock_held(obj);
    Py_END_CRITICAL_SECTION();
    return res;
}

static int
StructMeta_traverse(StructMetaObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->struct_fields);
    Py_VISIT(self->struct_defaults);
    Py_VISIT(self->struct_encode_fields);
    Py_VISIT(self->struct_module);
    Py_VISIT(self->struct_tag);  /* May be a function */
    Py_VISIT(self->rename);  /* May be a function */
    Py_VISIT(self->post_init);
    Py_VISIT(self->struct_field_codecs);
    Py_VISIT(self->struct_info);
    return PyType_Type.tp_traverse((PyObject *)self, visit, arg);
}

static int
StructMeta_clear(StructMetaObject *self)
{
    /* skip if clear already invoked */
    if (self->struct_fields == NULL) return 0;

    Py_CLEAR(self->struct_fields);
    Py_CLEAR(self->struct_defaults);
    Py_CLEAR(self->struct_encode_fields);
    Py_CLEAR(self->struct_module);
    Py_CLEAR(self->struct_tag_field);
    Py_CLEAR(self->struct_tag_value);
    Py_CLEAR(self->struct_tag);
    Py_CLEAR(self->rename);
    Py_CLEAR(self->post_init);
    Py_CLEAR(self->struct_field_codecs);
    Py_CLEAR(self->struct_info);
    Py_CLEAR(self->match_args);
    if (self->struct_offsets != NULL) {
        PyMem_Free(self->struct_offsets);
        self->struct_offsets = NULL;
    }
    if (self->struct_encode_keys != NULL) {
        PyMem_Free(self->struct_encode_keys);
        self->struct_encode_keys = NULL;
    }
    return PyType_Type.tp_clear((PyObject *)self);
}

static void
StructMeta_dealloc(StructMetaObject *self)
{
    /* The GC invariants require dealloc immediately untrack to avoid double
     * deallocation. However, PyType_Type.tp_dealloc assumes the type is
     * currently tracked. Hence the unfortunate untrack/retrack below. */
    PyObject_GC_UnTrack(self);
    StructMeta_clear(self);
    PyObject_GC_Track(self);
    PyType_Type.tp_dealloc((PyObject *)self);
}

static PyObject*
StructMeta_signature(StructMetaObject *self, void *closure)
{
    Py_ssize_t nfields, ndefaults, npos, nkwonly, i;
    StructspecState *st;
    PyObject *res = NULL;
    PyObject *inspect = NULL;
    PyObject *parameter_cls = NULL;
    PyObject *parameter_empty = NULL;
    PyObject *kind_positional = NULL;
    PyObject *kind_kw_only = NULL;
    PyObject *signature_cls = NULL;
    PyObject *annotations = NULL;
    PyObject *parameters = NULL;
    PyObject *temp_args = NULL, *temp_kwargs = NULL;
    PyObject *field, *kind, *default_val, *parameter, *annotation;

    st = structtype_get_global_state();

    nfields = PyTuple_GET_SIZE(self->struct_fields);
    ndefaults = PyTuple_GET_SIZE(self->struct_defaults);
    npos = nfields - ndefaults;
    nkwonly = self->nkwonly;

    inspect = PyImport_ImportModule("inspect");
    if (inspect == NULL) goto cleanup;
    parameter_cls = PyObject_GetAttrString(inspect, "Parameter");
    if (parameter_cls == NULL) goto cleanup;
    parameter_empty = PyObject_GetAttrString(parameter_cls, "empty");
    if (parameter_empty == NULL) goto cleanup;
    kind_positional = PyObject_GetAttrString(parameter_cls, "POSITIONAL_OR_KEYWORD");
    if (kind_positional == NULL) goto cleanup;
    kind_kw_only = PyObject_GetAttrString(parameter_cls, "KEYWORD_ONLY");
    if (kind_kw_only == NULL) goto cleanup;
    signature_cls = PyObject_GetAttrString(inspect, "Signature");
    if (signature_cls == NULL) goto cleanup;

    annotations = PyObject_CallOneArg(st->get_type_hints, (PyObject *)self);
    if (annotations == NULL) goto cleanup;

    parameters = PyList_New(nfields);
    if (parameters == NULL) return NULL;

    temp_args = PyTuple_New(0);
    if (temp_args == NULL) goto cleanup;
    temp_kwargs = PyDict_New();
    if (temp_kwargs == NULL) goto cleanup;

    for (i = 0; i < nfields; i++) {
        field = PyTuple_GET_ITEM(self->struct_fields, i);
        if (i < npos) {
            default_val = parameter_empty;
        } else {
            default_val = PyTuple_GET_ITEM(self->struct_defaults, i - npos);
            if (default_val == NODEFAULT) {
                default_val = parameter_empty;
            }
        }
        if (i < (nfields - nkwonly)) {
            kind = kind_positional;
        } else {
            kind = kind_kw_only;
        }
        annotation = PyDict_GetItem(annotations, field);
        if (annotation == NULL) {
            annotation = parameter_empty;
        }
        if (PyDict_SetItemString(temp_kwargs, "name", field) < 0) goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "kind", kind) < 0) goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "default", default_val) < 0) goto cleanup;
        if (PyDict_SetItemString(temp_kwargs, "annotation", annotation) < 0) goto cleanup;
        parameter = PyObject_Call(parameter_cls, temp_args, temp_kwargs);
        if (parameter == NULL) goto cleanup;
        PyList_SET_ITEM(parameters, i, parameter);
    }
    res = PyObject_CallOneArg(signature_cls, parameters);
cleanup:
    Py_XDECREF(inspect);
    Py_XDECREF(parameter_cls);
    Py_XDECREF(parameter_empty);
    Py_XDECREF(kind_positional);
    Py_XDECREF(kind_kw_only);
    Py_XDECREF(signature_cls);
    Py_XDECREF(annotations);
    Py_XDECREF(parameters);
    Py_XDECREF(temp_args);
    Py_XDECREF(temp_kwargs);
    return res;
}

static PyObject*
StructConfig_frozen(StructConfig *self, void *closure)
{
    if (self->st_type->frozen == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructConfig_eq(StructConfig *self, void *closure)
{
    if (self->st_type->eq == OPT_FALSE) { Py_RETURN_FALSE; }
    else { Py_RETURN_TRUE; }
}

static PyObject*
StructConfig_order(StructConfig *self, void *closure)
{
    if (self->st_type->order == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructConfig_array_like(StructConfig *self, void *closure)
{
    if (self->st_type->array_like == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructConfig_gc(StructConfig *self, void *closure)
{
    if (self->st_type->gc == OPT_FALSE) { Py_RETURN_FALSE; }
    else { Py_RETURN_TRUE; }
}

static PyObject*
StructConfig_weakref(StructConfig *self, void *closure)
{
    PyTypeObject *type = (PyTypeObject *)(self->st_type);
    if (type->tp_weaklistoffset) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject*
StructConfig_dict(StructConfig *self, void *closure)
{
    PyTypeObject *type = (PyTypeObject *)(self->st_type);
    if (type->tp_dictoffset) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject*
StructConfig_cache_hash(StructConfig *self, void *closure)
{
    StructMetaObject *type = (StructMetaObject *)(self->st_type);
    if (type->hash_offset != 0) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyObject*
StructConfig_repr_omit_defaults(StructConfig *self, void *closure)
{
    if (self->st_type->repr_omit_defaults == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructConfig_omit_defaults(StructConfig *self, void *closure)
{
    if (self->st_type->omit_defaults == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructConfig_forbid_unknown_fields(StructConfig *self, void *closure)
{
    if (self->st_type->forbid_unknown_fields == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructConfig_validate_on_init(StructConfig *self, void *closure)
{
    if (self->st_type->validate_on_init == OPT_TRUE) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject*
StructConfig_tag_field(StructConfig *self, void *closure)
{
    PyObject *out = self->st_type->struct_tag_field;
    if (out == NULL) Py_RETURN_NONE;
    Py_INCREF(out);
    return out;
}

static PyObject*
StructConfig_tag(StructConfig *self, void *closure)
{
    PyObject *out = self->st_type->struct_tag_value;
    if (out == NULL) Py_RETURN_NONE;
    Py_INCREF(out);
    return out;
}

static PyGetSetDef StructConfig_getset[] = {
    {"frozen", (getter) StructConfig_frozen, NULL, NULL, NULL},
    {"eq", (getter) StructConfig_eq, NULL, NULL, NULL},
    {"order", (getter) StructConfig_order, NULL, NULL, NULL},
    {"repr_omit_defaults", (getter) StructConfig_repr_omit_defaults, NULL, NULL, NULL},
    {"array_like", (getter) StructConfig_array_like, NULL, NULL, NULL},
    {"gc", (getter) StructConfig_gc, NULL, NULL, NULL},
    {"weakref", (getter) StructConfig_weakref, NULL, NULL, NULL},
    {"dict", (getter) StructConfig_dict, NULL, NULL, NULL},
    {"cache_hash", (getter) StructConfig_cache_hash, NULL, NULL, NULL},
    {"omit_defaults", (getter) StructConfig_omit_defaults, NULL, NULL, NULL},
    {"forbid_unknown_fields", (getter) StructConfig_forbid_unknown_fields, NULL, NULL, NULL},
    {"validate_on_init", (getter) StructConfig_validate_on_init, NULL, NULL, NULL},
    {"tag", (getter) StructConfig_tag, NULL, NULL, NULL},
    {"tag_field", (getter) StructConfig_tag_field, NULL, NULL, NULL},
    {NULL},
};

static int
StructConfig_traverse(StructConfig *self, visitproc visit, void *arg) {
    Py_VISIT(self->st_type);
    return 0;
}

static int
StructConfig_clear(StructConfig *self) {
    Py_CLEAR(self->st_type);
    return 0;
}

static void
StructConfig_dealloc(StructConfig *self) {
    PyObject_GC_UnTrack(self);
    StructConfig_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

PyDoc_STRVAR(StructConfig__doc__,
"StructConfig()\n"
"--\n"
"\n"
"Configuration settings for a given Struct type.\n"
"\n"
"This object is accessible through the ``__struct_config__`` field on a struct\n"
"type or instance. It exposes the following attributes, matching the Struct\n"
"configuration parameters of the same name. See the `Struct` docstring for\n"
"details.\n"
"\n"
"Note that the ``kw_only`` and ``rename`` struct configuration options are\n"
"consumed at class creation time and are not exposed as attributes on this\n"
"object.\n"
"\n"
"Configuration\n"
"-------------\n"
"frozen: bool\n"
"eq: bool\n"
"order: bool\n"
"array_like: bool\n"
"gc: bool\n"
"repr_omit_defaults: bool\n"
"omit_defaults: bool\n"
"forbid_unknown_fields: bool\n"
"validate_on_init: bool\n"
"weakref: bool\n"
"dict: bool\n"
"cache_hash: bool\n"
"tag_field: str | None\n"
"tag: str | int | None"
);

static PyTypeObject StructConfig_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype.StructConfig",
    .tp_doc = StructConfig__doc__,
    .tp_basicsize = sizeof(StructConfig),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = NULL,
    .tp_dealloc = (destructor) StructConfig_dealloc,
    .tp_clear = (inquiry) StructConfig_clear,
    .tp_traverse = (traverseproc) StructConfig_traverse,
    .tp_getset = StructConfig_getset,
};

static PyObject*
StructConfig_New(StructMetaObject *st_type)
{
    StructConfig *out = (StructConfig *)StructConfig_Type.tp_alloc(&StructConfig_Type, 0);
    if (out == NULL) return NULL;

    out->st_type = st_type;
    Py_INCREF(st_type);
    return (PyObject *)out;
}

static PyObject*
StructMeta_config(StructMetaObject *self, void *closure) {
    return StructConfig_New(self);
}

static PyMemberDef StructMeta_members[] = {
    {"__struct_fields__", T_OBJECT_EX, offsetof(StructMetaObject, struct_fields), READONLY, "Struct fields"},
    {"__struct_defaults__", T_OBJECT_EX, offsetof(StructMetaObject, struct_defaults), READONLY, "Struct defaults"},
    {"__struct_encode_fields__", T_OBJECT_EX, offsetof(StructMetaObject, struct_encode_fields), READONLY, "Struct encoded field names"},
    {"__match_args__", T_OBJECT_EX, offsetof(StructMetaObject, match_args), READONLY, "Positional match args"},
    {NULL},
};

static PyGetSetDef StructMeta_getset[] = {
    {"__signature__", (getter) StructMeta_signature, NULL, NULL, NULL},
    {"__struct_config__", (getter) StructMeta_config, NULL, "Struct configuration", NULL},
    {NULL},
};

PyDoc_STRVAR(StructMeta__doc__,
"StructMeta(name, bases, namespace, /, *, **struct_config)\n"
"--\n"
"\n"
"The metaclass for creating `Struct` types. See its documentation for the\n"
"available configuration options when subclassing.\n"
"\n"
"StructMeta can be subclassed, and may be combined with `abc.ABCMeta` to define\n"
"abstract base Structs. Other metaclass combinations are not supported; they\n"
"may work by accident but are not considered part of the public API.\n"
"\n"
"Examples\n"
"--------\n"
"Here we define a metaclass that modifies the default configuration and use\n"
"it to create a new `Struct` base class.\n"
"\n"
">>> from structtype import Struct, StructMeta\n"
">>> class KwOnlyStructMeta(StructMeta):\n"
"...     def __new__(mcls, name, bases, namespace, **struct_config):\n"
"...         struct_config.setdefault(\"kw_only\", True)\n"
"...         return super().__new__(mcls, name, bases, namespace, **struct_config)\n"
"...\n"
">>> class KwOnlyStruct(Struct, metaclass=KwOnlyStructMeta): ...\n"
"\n"
"Any subclass of ``KwOnlyStruct`` will have ``kw_only`` set to ``True`` by\n"
"default.\n"
"\n"
">>> class Example(KwOnlyStruct):\n"
"...     a: str = ""\n"
"...     b: int\n"
"...\n"
">>> Example(b=123)\n"
"Example(a='', b=123)\n"
);

static PyTypeObject StructMetaType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.StructMeta",
    .tp_doc = StructMeta__doc__,
    .tp_basicsize = sizeof(StructMetaObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_TYPE_SUBCLASS | Py_TPFLAGS_HAVE_GC | _Py_TPFLAGS_HAVE_VECTORCALL | Py_TPFLAGS_BASETYPE,
    .tp_new = StructMeta_new,
    .tp_dealloc = (destructor) StructMeta_dealloc,
    .tp_clear = (inquiry) StructMeta_clear,
    .tp_traverse = (traverseproc) StructMeta_traverse,
    .tp_members = StructMeta_members,
    .tp_getset = StructMeta_getset,
    .tp_call = PyVectorcall_Call,
    .tp_vectorcall_offset = offsetof(PyTypeObject, tp_vectorcall),
};


static PyObject *
get_default(PyObject *obj) {
    PyTypeObject *type = Py_TYPE(obj);
    if (type == &Factory_Type) {
        return Factory_Call(obj);
    }
    Py_INCREF(obj);
    return obj;
}

static MS_INLINE bool
is_default(PyObject *x, PyObject *d) {
    if (x == d) return true;
    if (Py_TYPE(d) == &Factory_Type) {
        PyTypeObject *factory = (PyTypeObject *)(((Factory *)d)->factory);
        if (Py_TYPE(x) != factory) return false;
        if (factory == &PyList_Type && PyList_GET_SIZE(x) == 0) return true;
        if (factory == &PyDict_Type && PyDict_GET_SIZE(x) == 0) return true;
        if (factory == &PySet_Type && PySet_GET_SIZE(x) == 0) return true;
        if (factory == &PyTuple_Type && PyTuple_GET_SIZE(x) == 0) return true;
        if (factory == &PyFrozenSet_Type && PySet_GET_SIZE(x) == 0) return true;
    }
    return false;
}

/* Set field #index on obj. Steals a reference to val */
static inline void
Struct_set_index(PyObject *obj, Py_ssize_t index, PyObject *val) {
    StructMetaObject *cls;
    char *addr;
    PyObject *old;

    cls = (StructMetaObject *)Py_TYPE(obj);
    addr = (char *)obj + cls->struct_offsets[index];
    old = *(PyObject **)addr;
    Py_XDECREF(old);
    *(PyObject **)addr = val;
}

/* Get field #index or NULL on obj. Returns a borrowed reference */
static inline PyObject*
Struct_get_index_noerror(PyObject *obj, Py_ssize_t index) {
    StructMetaObject *cls = (StructMetaObject *)Py_TYPE(obj);
    char *addr = (char *)obj + cls->struct_offsets[index];
    return *(PyObject **)addr;
}

/* Get field #index on obj. Returns a borrowed reference */
static inline PyObject*
Struct_get_index(PyObject *obj, Py_ssize_t index) {
    PyObject *val = Struct_get_index_noerror(obj, index);
    if (val == NULL) {
        StructMetaObject *cls = (StructMetaObject *)Py_TYPE(obj);
        PyErr_Format(PyExc_AttributeError,
                     "Struct field %R is unset",
                     PyTuple_GET_ITEM(cls->struct_fields, index));
    }
    return val;
}

static MS_INLINE int
Struct_post_init(StructMetaObject *st_type, PyObject *obj) {
    if (st_type->post_init != NULL) {
        PyObject *res = PyObject_CallOneArg(st_type->post_init, obj);
        if (res == NULL) return -1;
        Py_DECREF(res);
    }
    return 0;
}

static MS_INLINE int
Struct_decode_post_init(StructMetaObject *st_type, PyObject *obj, PathNode *path) {
    if (MS_UNLIKELY(Struct_post_init(st_type, obj) < 0)) {
        ms_maybe_wrap_validation_error(path);
        return -1;
    }
    return 0;
}

static int
Struct_fill_in_defaults(StructMetaObject *st_type, PyObject *obj, PathNode *path) {
    Py_ssize_t nfields, ndefaults, i;
    bool is_gc, should_untrack;

    nfields = PyTuple_GET_SIZE(st_type->struct_encode_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    is_gc = MS_TYPE_IS_GC(st_type);
    should_untrack = is_gc;

    for (i = 0; i < nfields; i++) {
        PyObject *val = Struct_get_index_noerror(obj, i);
        if (val == NULL) {
            if (MS_UNLIKELY(i < (nfields - ndefaults))) goto missing_required;
            val = PyTuple_GET_ITEM(
                st_type->struct_defaults, i - (nfields - ndefaults)
            );
            if (MS_UNLIKELY(val == NODEFAULT)) goto missing_required;
            val = get_default(val);
            if (MS_UNLIKELY(val == NULL)) return -1;
            Struct_set_index(obj, i, val);
        }
        if (should_untrack) {
            should_untrack = !MS_MAYBE_TRACKED(val);
        }
    }

    if (is_gc && should_untrack && MS_IS_TRACKED(obj))
        PyObject_GC_UnTrack(obj);

    if (Struct_decode_post_init(st_type, obj, path) < 0) return -1;

    return 0;

missing_required:
    ms_missing_required_field(
        PyTuple_GET_ITEM(st_type->struct_encode_fields, i), path
    );
    return -1;
}

static MS_NOINLINE void
Struct_build_abstract_error(PyTypeObject *cls) {
    /* This function is adapted from CPython's object machinery.
     * https://github.com/python/cpython/blob/a486d45/Objects/typeobject.c#L7147-L7193 */
    PyObject *abstract_methods = PyObject_GetAttrString((PyObject *)cls, "__abstractmethods__");
    if (abstract_methods == NULL) {
        return;
    }

    PyObject *sorted_methods = PySequence_List(abstract_methods);
    Py_DECREF(abstract_methods);
    if (sorted_methods == NULL) {
        return;
    }

    if (PyList_Sort(sorted_methods) < 0) {
        Py_DECREF(sorted_methods);
        return;
    }

    Py_ssize_t method_count = PyList_GET_SIZE(sorted_methods);
    if (method_count < 0) {
        Py_DECREF(sorted_methods);
        return;
    }

    PyObject *sep = PyUnicode_FromString("', '");
    if (sep == NULL) {
        Py_DECREF(sorted_methods);
        return;
    }

    PyObject *joined = PyUnicode_Join(sep, sorted_methods);
    Py_DECREF(sep);
    Py_DECREF(sorted_methods);
    if (joined == NULL) {
        return;
    }

    PyErr_Format(
        PyExc_TypeError,
        "Can't instantiate abstract class %s without "
        "an implementation for abstract method%s '%U'",
        cls->tp_name,
        method_count > 1 ? "s" : "",
        joined
    );
    Py_DECREF(joined);
}

static int struct_check_recursive(PyObject *, StructspecState *);

static PyObject *
Struct_vectorcall(PyTypeObject *cls, PyObject *const *args, size_t nargsf, PyObject *kwnames) {
    if (MS_UNLIKELY(cls->tp_flags & Py_TPFLAGS_IS_ABSTRACT)) {
        Struct_build_abstract_error(cls);
        return NULL;
    }

    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);

    StructMetaObject *st_type = (StructMetaObject *)cls;
    PyObject *fields = st_type->struct_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    PyObject *defaults = st_type->struct_defaults;
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(defaults);
    Py_ssize_t nkwonly = st_type->nkwonly;
    Py_ssize_t npos = nfields - ndefaults;

    if (MS_UNLIKELY(nargs > (nfields - nkwonly))) {
        PyErr_SetString(PyExc_TypeError, "Extra positional arguments provided");
        return NULL;
    }

    bool is_gc = MS_TYPE_IS_GC(cls);
    bool should_untrack = is_gc;

    PyObject *self = Struct_alloc(cls);
    if (self == NULL) return NULL;

    /* First, process all positional arguments */
    for (Py_ssize_t i = 0; i < nargs; i++) {
        PyObject *val = args[i];
        char *addr = (char *)self + st_type->struct_offsets[i];
        Py_INCREF(val);
        *(PyObject **)addr = val;
        if (should_untrack) {
            should_untrack = !MS_MAYBE_TRACKED(val);
        }
    }

    /* Next, process all kwargs */
    for (Py_ssize_t i = 0; i < nkwargs; i++) {
        char *addr;
        PyObject *val;
        Py_ssize_t field_index;
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);

        /* Since keyword names are interned, first loop with pointer
         * comparisons only. */
        for (field_index = nargs; field_index < nfields; field_index++) {
            PyObject *field = PyTuple_GET_ITEM(fields, field_index);
            if (MS_LIKELY(kwname == field)) goto kw_found;
        }

        /* Fast path failed. It's more likely that this is an invalid kwarg
         * than that the kwname wasn't interned. Loop from 0 this time to also
         * check for parameters passed both as arg and kwarg */
        for (field_index = 0; field_index < nfields; field_index++) {
            PyObject *field = PyTuple_GET_ITEM(fields, field_index);
            if (MS_UNICODE_EQ(kwname, field)) {
                if (MS_UNLIKELY(field_index < nargs)) {
                    PyErr_Format(
                        PyExc_TypeError,
                        "Argument '%U' given by name and position",
                        kwname
                    );
                    goto error;
                }
                goto kw_found;
            }
        }

        /* Unknown keyword */
        PyErr_Format(PyExc_TypeError, "Unexpected keyword argument '%U'", kwname);
        goto error;

kw_found:
        val = args[i + nargs];
        addr = (char *)self + st_type->struct_offsets[field_index];
        Py_INCREF(val);
        *(PyObject **)addr = val;
        if (should_untrack) {
            should_untrack = !MS_MAYBE_TRACKED(val);
        }
    }

    /* Finally, fill in missing defaults */
    if (nargs + nkwargs < nfields) {
        for (Py_ssize_t field_index = nargs; field_index < nfields; field_index++) {
            char *addr = (char *)self + st_type->struct_offsets[field_index];
            if (MS_LIKELY(*(PyObject **)addr == NULL)) {
                if (MS_LIKELY(field_index >= npos)) {
                    PyObject *val = PyTuple_GET_ITEM(defaults, field_index - npos);
                    if (MS_LIKELY(val != NODEFAULT)) {
                        val = get_default(val);
                        if (MS_UNLIKELY(val == NULL)) goto error;
                        *(PyObject **)addr = val;
                        if (should_untrack) {
                            should_untrack = !MS_MAYBE_TRACKED(val);
                        }
                        continue;
                    }
                }
                PyErr_Format(
                    PyExc_TypeError,
                    "Missing required argument '%U'",
                    PyTuple_GET_ITEM(fields, field_index)
                );
                goto error;
            }
        }
    }

    if (is_gc && should_untrack && MS_IS_TRACKED(self))
        PyObject_GC_UnTrack(self);

    if (st_type->validate_on_init == OPT_TRUE) {
        StructspecState *mod = structtype_get_global_state();
        if (mod == NULL) goto error;
        if (struct_check_recursive(self, mod) < 0) goto error;
    }

    if (Struct_post_init(st_type, self) < 0) goto error;
    return self;

error:
    Py_DECREF(self);
    return NULL;
}

static PyObject *
Struct_repr(PyObject *self) {
    StructMetaObject *st_type = (StructMetaObject *)(Py_TYPE(self));
    bool omit_defaults = st_type->repr_omit_defaults == OPT_TRUE;
    PyObject *fields = st_type->struct_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    PyObject *defaults = NULL;
    Py_ssize_t nunchecked = nfields;

    if (omit_defaults) {
        defaults = st_type->struct_defaults;
        nunchecked = nfields - PyTuple_GET_SIZE(defaults);
    }

    int recursive = Py_ReprEnter(self);
    if (recursive != 0) {
        return (recursive < 0) ? NULL : PyUnicode_FromString("...");  /* cpylint-ignore */
    }

    strbuilder builder = {0};
    bool first = true;
    const char *name = Py_TYPE(self)->tp_name;
    if (!strbuilder_extend(&builder, name, strlen(name))) goto error;
    if (!strbuilder_extend_literal(&builder, "(")) goto error;

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(fields, i);
        PyObject *val = Struct_get_index(self, i);
        if (val == NULL) goto error;

        if (i >= nunchecked) {
            PyObject *default_val = PyTuple_GET_ITEM(defaults, i - nunchecked);
            if (is_default(val, default_val)) continue;
        }

        if (first) {
            first = false;
        }
        else {
            if (!strbuilder_extend_literal(&builder, ", ")) goto error;
        }

        if (!strbuilder_extend_unicode(&builder, field)) goto error;
        if (!strbuilder_extend_literal(&builder, "=")) goto error;

        PyObject *repr = PyObject_Repr(val);
        if (repr == NULL) goto error;
        bool ok = strbuilder_extend_unicode(&builder, repr);
        Py_DECREF(repr);
        if (!ok) goto error;
    }

    if (!strbuilder_extend_literal(&builder, ")")) goto error;

    PyObject *out = strbuilder_build(&builder);
    Py_ReprLeave(self);
    return out;

error:
    strbuilder_reset(&builder);
    Py_ReprLeave(self);
    return NULL;
}

static Py_hash_t
Struct_hash(PyObject *self) {
    PyObject *val;
    Py_ssize_t i, nfields;
    Py_uhash_t acc = MS_HASH_XXPRIME_5;

    StructMetaObject *st_type = (StructMetaObject *)Py_TYPE(self);

    if (MS_UNLIKELY(st_type->eq == OPT_FALSE)) {
        /* If `__eq__` isn't implemented, then the default pointer-based
         * `__hash__` should be used */
        return PyBaseObject_Type.tp_hash(self);
    }

    if (MS_UNLIKELY(st_type->frozen != OPT_TRUE)) {
        /* If `__eq__` is implemented, only frozen types can be hashed */
        return PyObject_HashNotImplemented(self);
    }

    if (MS_UNLIKELY(st_type->hash_offset != 0)) {
        PyObject *cached_hash = *(PyObject **)((char *)self + st_type->hash_offset);
        if (cached_hash != NULL) {
            /* Use the cached hash */
            return PyLong_AsSsize_t(cached_hash);
        }
    }

    /* First hash the type by its pointer */
    size_t type_id = (size_t)((void *)st_type);
    /* The lower bits are likely to be 0; rotate by 4 */
    type_id = (type_id >> 4) | (type_id << (8 * sizeof(void *) - 4));
    acc += type_id * MS_HASH_XXPRIME_2;
    acc = MS_HASH_XXROTATE(acc);
    acc *= MS_HASH_XXPRIME_1;

    /* Then hash all the fields */
    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));
    for (i = 0; i < nfields; i++) {
        val = Struct_get_index(self, i);
        if (val == NULL) return -1;
        Py_uhash_t item_hash = PyObject_Hash(val);
        if (item_hash == (Py_uhash_t)-1) return -1;
        acc += item_hash * MS_HASH_XXPRIME_2;
        acc = MS_HASH_XXROTATE(acc);
        acc *= MS_HASH_XXPRIME_1;
    }
    acc += (1 + nfields) ^ (MS_HASH_XXPRIME_5 ^ 3527539UL);

    Py_uhash_t hash = (acc == (Py_uhash_t)-1) ?  1546275796 : acc;

    if (MS_UNLIKELY(st_type->hash_offset != 0)) {
        /* Cache the hash */
        char *addr = (char *)self + st_type->hash_offset;
        PyObject *cached_hash = PyLong_FromSsize_t(hash);
        if (cached_hash == NULL) return -1;
        *(PyObject **)addr = cached_hash;
    }

    return hash;
}

static PyObject *
Struct_richcompare(PyObject *self, PyObject *other, int op) {
    if (Py_TYPE(self) != Py_TYPE(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    StructMetaObject *st_type = (StructMetaObject *)(Py_TYPE(self));

    if (op == Py_EQ || op == Py_NE) {
        if (MS_UNLIKELY(st_type->eq == OPT_FALSE)) {
            Py_RETURN_NOTIMPLEMENTED;
        }
    }
    else if (st_type->order != OPT_TRUE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    if (
        MS_UNLIKELY(op == Py_NE && (Py_TYPE(self)->tp_richcompare != Struct_richcompare))
    ) {
        /* This case is hit when a subclass has manually defined `__eq__` but
         * not `__ne__`. In this case we want to dispatch to `__eq__` and invert
         * the result, rather than relying on the default `__ne__` implementation.
         */
        PyObject *out = Py_TYPE(self)->tp_richcompare(self, other, Py_EQ);
        if (out != NULL && out != Py_NotImplemented) {
            int is_true = PyObject_IsTrue(out);
            Py_DECREF(out);
            if (is_true < 0) {
                out = NULL;
            }
            else {
                out = is_true ? Py_False : Py_True;
                Py_INCREF(out);
            }
        }
        return out;
    }

    int equal = 1;
    PyObject *left = NULL, *right = NULL;

    /* Only need to loop if self is not other` */
    if (MS_LIKELY(self != other)) {
        Py_ssize_t nfields = StructMeta_GET_NFIELDS(st_type);
        for (Py_ssize_t i = 0; i < nfields; i++) {
            left = Struct_get_index(self, i);
            if (left == NULL) return NULL;

            right = Struct_get_index(other, i);
            if (right == NULL) return NULL;

            equal = PyObject_RichCompareBool(left, right, Py_EQ);

            if (equal < 0) return NULL;
            if (equal == 0) break;
        }
    }

    if (equal) {
        if (op == Py_EQ || op == Py_GE || op == Py_LE) {
            Py_RETURN_TRUE;
        }
        else if (op == Py_NE) {
            Py_RETURN_FALSE;
        }
        else if (left == NULL) {
            /* < or > on two 0-field or identical structs */
            Py_RETURN_FALSE;
        }
    }
    else if (op == Py_EQ) {
        Py_RETURN_FALSE;
    }
    else if (op == Py_NE) {
        Py_RETURN_TRUE;
    }
    /* Need to compare final element again to determine proper result */
    return PyObject_RichCompare(left, right, op);
}

static PyObject *
Struct_copy(PyObject *self, PyObject *args)
{
    Py_ssize_t i, nfields;
    PyObject *val, *res = NULL;

    res = Struct_alloc(Py_TYPE(self));
    if (res == NULL)
        return NULL;

    nfields = StructMeta_GET_NFIELDS(Py_TYPE(self));
    for (i = 0; i < nfields; i++) {
        val = Struct_get_index(self, i);
        if (val == NULL)
            goto error;
        Py_INCREF(val);
        Struct_set_index(res, i, val);
    }
    /* If self is tracked, then copy is tracked */
    if (MS_OBJECT_IS_GC(self) && !MS_IS_TRACKED(self))
        PyObject_GC_UnTrack(res);
    return res;
error:
    Py_DECREF(res);
    return NULL;
}

static PyObject *
Struct_replace(
    PyObject *self,
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *kwnames
) {
    Py_ssize_t nkwargs = (kwnames == NULL) ? 0 : PyTuple_GET_SIZE(kwnames);

    if (!check_positional_nargs(nargs, 0, 0)) return NULL;

    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(self);
    PyObject *fields = struct_type->struct_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    bool is_gc = MS_TYPE_IS_GC(struct_type);
    bool should_untrack = is_gc;

    PyObject *out = Struct_alloc((PyTypeObject *)struct_type);
    if (out == NULL) return NULL;

    for (Py_ssize_t i = 0; i < nkwargs; i++) {
        PyObject *val;
        Py_ssize_t field_index;
        PyObject *kwname = PyTuple_GET_ITEM(kwnames, i);

        /* Since keyword names are interned, first loop with pointer
         * comparisons only. */
        for (field_index = 0; field_index < nfields; field_index++) {
            PyObject *field = PyTuple_GET_ITEM(fields, field_index);
            if (MS_LIKELY(kwname == field)) goto kw_found;
        }
        for (field_index = 0; field_index < nfields; field_index++) {
            PyObject *field = PyTuple_GET_ITEM(fields, field_index);
            if (MS_UNICODE_EQ(kwname, field)) goto kw_found;
        }

        /* Unknown keyword */
        PyErr_Format(
            PyExc_TypeError, "`%.200s` has no field '%U'",
            ((PyTypeObject *)struct_type)->tp_name, kwname
        );
        goto error;

    kw_found:
        val = args[i];
        Py_INCREF(val);
        Struct_set_index(out, field_index, val);
        if (should_untrack) {
            should_untrack = !MS_MAYBE_TRACKED(val);
        }
    }

    for (Py_ssize_t i = 0; i < nfields; i++) {
        if (Struct_get_index_noerror(out, i) == NULL) {
            PyObject *val = Struct_get_index(self, i);
            if (val == NULL) goto error;
            if (should_untrack) {
                should_untrack = !MS_MAYBE_TRACKED(val);
            }
            Py_INCREF(val);
            Struct_set_index(out, i, val);
        }
    }

    if (Struct_post_init(struct_type, out) < 0) goto error;

    if (is_gc && should_untrack && MS_IS_TRACKED(out))
        PyObject_GC_UnTrack(out);
    return out;

error:
    Py_DECREF(out);
    return NULL;
}static PyObject *
Struct_reduce(PyObject *self, PyObject *args)
{
    PyObject *values = NULL, *out = NULL;
    StructMetaObject *st_type = (StructMetaObject *)(Py_TYPE(self));
    Py_ssize_t nfields = PyTuple_GET_SIZE(st_type->struct_fields);

    if (st_type->nkwonly) {
        StructspecState *mod = structtype_get_global_state();
        values = PyDict_New();
        if (values == NULL) return NULL;
        for (Py_ssize_t i = 0; i < nfields; i++) {
            PyObject *field = PyTuple_GET_ITEM(st_type->struct_fields, i);
            PyObject *val = Struct_get_index(self, i);
            if (val == NULL) goto cleanup;
            if (PyDict_SetItem(values, field, val) < 0) goto cleanup;
        }
        out = Py_BuildValue("O(OO)", mod->rebuild, Py_TYPE(self), values);
    }
    else {
        values = PyTuple_New(nfields);
        if (values == NULL) return NULL;
        for (Py_ssize_t i = 0; i < nfields; i++) {
            PyObject *val = Struct_get_index(self, i);
            if (val == NULL) goto cleanup;
            Py_INCREF(val);
            PyTuple_SET_ITEM(values, i, val);
        }
        out = PyTuple_Pack(2, Py_TYPE(self), values);
    }
cleanup:
    Py_DECREF(values);
    return out;
}

static PyObject *
Struct_rich_repr(PyObject *self, PyObject *args) {
    StructMetaObject *st_type = (StructMetaObject *)(Py_TYPE(self));
    bool omit_defaults = st_type->repr_omit_defaults == OPT_TRUE;
    PyObject *fields = st_type->struct_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    PyObject *defaults = NULL;
    Py_ssize_t nunchecked = nfields;

    if (omit_defaults) {
        defaults = st_type->struct_defaults;
        nunchecked = nfields - PyTuple_GET_SIZE(defaults);
    }

    PyObject *out = PyList_New(0);
    if (out == NULL) return NULL;

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(fields, i);
        PyObject *val = Struct_get_index(self, i);

        if (i >= nunchecked) {
            PyObject *default_val = PyTuple_GET_ITEM(defaults, i - nunchecked);
            if (is_default(val, default_val)) continue;
        }

        if (val == NULL) goto error;
        PyObject *part = PyTuple_Pack(2, field, val);
        if (part == NULL) goto error;
        int status = PyList_Append(out, part);
        Py_DECREF(part);
        if (status < 0) goto error;
    }
    return out;

error:
    Py_DECREF(out);
    return NULL;
}

static PyObject *
StructMixin_fields(PyObject *self, void *closure) {
    PyObject *out = ((StructMetaObject *)Py_TYPE(self))->struct_fields;
    Py_INCREF(out);
    return out;
}

static PyObject *
StructMixin_encode_fields(PyObject *self, void *closure) {
    PyObject *out = ((StructMetaObject *)Py_TYPE(self))->struct_encode_fields;
    Py_INCREF(out);
    return out;
}

static PyObject *
StructMixin_defaults(PyObject *self, void *closure) {
    PyObject *out = ((StructMetaObject *)Py_TYPE(self))->struct_defaults;
    Py_INCREF(out);
    return out;
}

static PyObject*
StructMixin_config(StructMetaObject *self, void *closure) {
    return StructConfig_New((StructMetaObject *)Py_TYPE(self));
}

static PyObject *
Struct_iter(PyObject *self) {
    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(self);
    PyObject *fields = struct_type->struct_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    PyObject *pairs = PyList_New(nfields);
    if (pairs == NULL) return NULL;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *key = PyTuple_GET_ITEM(fields, i);
        PyObject *val = Struct_get_index(self, i);
        if (val == NULL) { Py_DECREF(pairs); return NULL; }
        PyObject *pair = PyTuple_Pack(2, key, val);
        if (pair == NULL) { Py_DECREF(pairs); return NULL; }
        PyList_SET_ITEM(pairs, i, pair);
    }
    return PyObject_GetIter(pairs);
}

/* Forward declarations for struct methods */
static PyObject *Struct_dump_json(PyObject *, PyObject *const *, Py_ssize_t, PyObject *);
static PyObject *Struct_validate_json(PyObject *, PyObject *const *, Py_ssize_t, PyObject *);
static PyObject *Struct_dump(PyObject *, PyObject *const *, Py_ssize_t, PyObject *);
static PyObject *Struct_validate(PyObject *, PyObject *const *, Py_ssize_t, PyObject *);
static PyObject *Struct_check(PyObject *, PyObject *const *, Py_ssize_t, PyObject *);

PyDoc_STRVAR(Struct_copy__doc__,
"__copy__(self)\n"
"--\n"
"\n"
"Return a shallow copy of the struct.\n"
);

PyDoc_STRVAR(Struct_replace__doc__,
"__replace__(self, /, **changes: Any) -> Self\n"
"--\n"
"\n"
"Create a new struct with the specified fields replaced.\n"
"\n"
"Parameters\n"
"----------\n"
"**changes : Any\n"
"    Keyword arguments mapping field names to new values.\n"
"\n"
"Returns\n"
"-------\n"
"Self\n"
"    A new struct instance with the same type and the given fields replaced.\n"
);

PyDoc_STRVAR(Struct_reduce__doc__,
"__reduce__(self)\n"
"--\n"
"\n"
"Return state information for pickling.\n"
"\n"
"This enables ``pickle.dump`` and ``copy.deepcopy`` support for struct\n"
"instances.\n"
);

PyDoc_STRVAR(Struct_rich_repr__doc__,
"__rich_repr__(self)\n"
"--\n"
"\n"
"Return a list of ``(name, value)`` pairs for rich display.\n"
"\n"
"This is used by IPython and Jupyter to provide a structured representation\n"
"of the struct.\n"
);

PyDoc_STRVAR(Struct_dump_json__doc__,
"struct_dump_json(self, *, decimal_format=None, uuid_format=None, sort_keys=False)\n"
"--\n"
"\n"
"Serialize this struct to JSON bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"decimal_format : str or callable, optional\n"
"    Controls how ``decimal.Decimal`` values are encoded. If ``\"string\"``\n"
"    (default), encodes as JSON strings. If ``\"number\"``, encodes as JSON\n"
"    numbers. A callable is called with each decimal value and should return\n"
"    an encodable type.\n"
"uuid_format : str, optional\n"
"    Controls how ``uuid.UUID`` values are encoded. If ``\"canonical\"``\n"
"    (default), encodes in canonical form (``xxxxxxxx-xxxx-xxxx-xxxx-\n"
"    xxxxxxxxxxxx``). If ``\"hex\"``, encodes as hex (``xxxxxxxxxxxxxxxxxxxx\n"
"    xxxxxxxxxxxx``) .\n"
"sort_keys : bool, optional\n"
"    If ``True``, dict keys and set elements are sorted so that equal\n"
"    values produce identical output. Struct, dataclass, and object fields\n"
"    keep their declaration order. Default is ``False``.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized JSON.\n"
);

PyDoc_STRVAR(Struct_validate_json__doc__,
"struct_validate_json(cls, buf, *, strict=True)\n"
"--\n"
"\n"
"Deserialize JSON bytes to an instance of this struct type.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : str or bytes-like\n"
"    The JSON message to decode.\n"
"strict : bool, optional\n"
"    Whether type coercion rules should be strict. If ``False``, enables a\n"
"    wider set of coercion rules from string to non-string types for all\n"
"    values. Default is ``True``.\n"
"\n"
"Returns\n"
"-------\n"
"Self\n"
"    A new struct instance.\n"
);

PyDoc_STRVAR(Struct_dump__doc__,
"struct_dump(self, *, sort_keys=False, str_keys=False, builtin_types=None)\n"
"--\n"
"\n"
"Convert this struct to built-in Python types.\n"
"\n"
"This applies the same semantics as ``struct_dump_json`` but produces\n"
"Python objects instead of JSON bytes:\n"
"\n"
"- Struct-level settings are honored: ``rename``, ``omit_defaults``,\n"
"  ``array_like``, and ``tag`` for tagged unions.\n"
"- Fields containing ``UNSET`` are omitted from the output.\n"
"- Nested structs, dataclasses, attrs, TypedDict, and NamedTuple values\n"
"  are recursively expanded.\n"
"- Types without a direct builtin equivalent are converted to their\n"
"  serializable representations.\n"
"\n"
"Parameters\n"
"----------\n"
"sort_keys : bool, optional\n"
"    If ``True``, dict keys and set elements are sorted so that equal\n"
"    values produce identical output. Struct, dataclass, and object fields\n"
"    keep their declaration order. Default is ``False``.\n"
"str_keys : bool, optional\n"
"    Whether to convert all object keys to strings. Default is ``False``.\n"
"builtin_types : Iterable[type], optional\n"
"    An iterable of types to treat as additional builtin types. These\n"
"    types will be passed through unchanged. Currently supports\n"
"    ``bytes``, ``bytearray``, ``memoryview``, ``datetime.datetime``,\n"
"    ``datetime.time``, ``datetime.date``, ``datetime.timedelta``,\n"
"    ``uuid.UUID``, ``decimal.Decimal``, ``enum.Enum``, and custom types.\n"
"    Use ``ALL_BUILTIN_TYPES`` as a shortcut to pass through all\n"
"    supported types.\n"
"\n"
"Returns\n"
"-------\n"
"dict or list\n"
"    The converted object. For ``array_like=False`` structs returns a\n"
"    ``dict``. For ``array_like=True`` structs returns a ``list``.\n"
);

PyDoc_STRVAR(Struct_validate__doc__,
"struct_validate(cls, obj, *, strict=True, from_attributes=False)\n"
"--\n"
"\n"
"Convert built-in types to an instance of this struct type.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to convert.\n"
"strict : bool, optional\n"
"    Whether type coercion rules should be strict. If ``False``, enables a\n"
"    wider set of coercion rules from string to non-string types for all\n"
"    values. Setting ``strict=False`` implies ``str_keys=True`` and\n"
"    ``builtin_types=None``. Default is ``True``.\n"
"from_attributes : bool, optional\n"
"    If ``True``, input objects may be coerced to struct, dataclass, or\n"
"    attrs types by extracting attributes matching output field names.\n"
"    Default is ``False``.\n"
"\n"
"Returns\n"
"-------\n"
"Self\n"
"    A validated struct instance.\n"
);

PyDoc_STRVAR(Struct_validate_self__doc__,
"struct_validate_self(self)\n"
"--\n"
"\n"
"Validate this struct's fields against their declared types.\n"
"\n"
"Raises a ``ValidationError`` if any field's value does not match its\n"
"declared type.\n"
);

static PyMethodDef Struct_methods[] = {
    {"__copy__", Struct_copy, METH_NOARGS, Struct_copy__doc__},
    {"__replace__", (PyCFunction) Struct_replace, METH_FASTCALL | METH_KEYWORDS, Struct_replace__doc__},
    {"__reduce__", Struct_reduce, METH_NOARGS, Struct_reduce__doc__},
    {"__rich_repr__", Struct_rich_repr, METH_NOARGS, Struct_rich_repr__doc__},
    {"struct_dump_json", (PyCFunction) Struct_dump_json, METH_FASTCALL | METH_KEYWORDS, Struct_dump_json__doc__},
    {"struct_validate_json", (PyCFunction) Struct_validate_json, METH_FASTCALL | METH_KEYWORDS | METH_CLASS, Struct_validate_json__doc__},
    {"struct_dump", (PyCFunction) Struct_dump, METH_FASTCALL | METH_KEYWORDS, Struct_dump__doc__},
    {"struct_validate", (PyCFunction) Struct_validate, METH_FASTCALL | METH_KEYWORDS | METH_CLASS, Struct_validate__doc__},
    {"struct_validate_self", (PyCFunction) Struct_check, METH_FASTCALL | METH_KEYWORDS, Struct_validate_self__doc__},
    {NULL, NULL},
};

static PyGetSetDef StructMixin_getset[] = {
    {"__struct_fields__", (getter) StructMixin_fields, NULL,
        "A tuple of field names in declaration order",
        NULL},
    {"__struct_encode_fields__", (getter) StructMixin_encode_fields, NULL,
        "A tuple of field names used for serialization (after applying rename)",
        NULL},
    {"__struct_defaults__", (getter) StructMixin_defaults, NULL,
        "A tuple of default values for each field, in declaration order",
        NULL},
    {"__struct_config__", (getter) StructMixin_config, NULL,
        "The StructConfig for this struct type",
        NULL},
    {NULL},
};

static PyTypeObject StructMixinType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core._StructMixin",
    .tp_basicsize = 0,
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_setattro = Struct_setattro_default,
    .tp_repr = Struct_repr,
    .tp_richcompare = Struct_richcompare,
    .tp_hash = Struct_hash,
    .tp_iter = (getiterfunc)Struct_iter,
    .tp_methods = Struct_methods,
    .tp_getset = StructMixin_getset,
};

PyDoc_STRVAR(Struct__doc__,
"A base class for defining efficient serializable objects.\n"
"\n"
"Fields are defined using type annotations. Fields may optionally have\n"
"default values, which result in keyword parameters to the constructor.\n"
"\n"
"Structs automatically define ``__init__``, ``__eq__``, ``__repr__``, and\n"
"``__copy__`` methods. Additional methods can be defined on the class as\n"
"needed. Note that ``__init__``/``__new__`` cannot be overridden, but other\n"
"methods can. A tuple of the field names is available on the class via the\n"
"``__struct_fields__`` attribute if needed.\n"
"\n"
"Additional class options can be enabled by passing keywords to the class\n"
"definition (see example below). These configuration options may also be\n"
"inspected at runtime through the ``__struct_config__`` attribute.\n"
"\n"
"Configuration\n"
"-------------\n"
"frozen: bool, default False\n"
"   Whether instances of this type are pseudo-immutable. If true, attribute\n"
"   assignment is disabled and a corresponding ``__hash__`` is defined.\n"
"order: bool, default False\n"
"   If True, ``__lt__``, `__le__``, ``__gt__``, and ``__ge__`` methods\n"
"   will be generated for this type.\n"
"eq: bool, default True\n"
"   If True (the default), an ``__eq__`` method will be generated for this\n"
"   type. Set to False to compare based on instance identity alone.\n"
"kw_only: bool, default False\n"
"   If True, all fields will be treated as keyword-only arguments in the\n"
"   generated ``__init__`` method. Default is False.\n"
"omit_defaults: bool, default False\n"
"   Whether fields should be omitted from encoding if the corresponding value\n"
"   is the default for that field. Enabling this may reduce message size, and\n"
"   often also improve encoding & decoding performance.\n"
"forbid_unknown_fields: bool, default False\n"
"   If True, an error is raised if an unknown field is encountered while\n"
"   decoding structs of this type. If False (the default), no error is raised\n"
"   and the unknown field is skipped.\n"
"tag: str, int, bool, callable, or None, default None\n"
"   Used along with ``tag_field`` for configuring tagged union support. If\n"
"   either are non-None, then the struct is considered \"tagged\". In this case,\n"
"   an extra field (the ``tag_field``) and value (the ``tag``) are added to the\n"
"   encoded message, which can be used to differentiate message types during\n"
"   decoding.\n"
"\n"
"   Set ``tag=True`` to enable the default tagged configuration (``tag_field``\n"
"   is ``\"type\"``, ``tag`` is the class name). Alternatively, you can provide\n"
"   a string (or less commonly int) value directly to be used as the tag\n"
"   (e.g. ``tag=\"my-tag-value\"``).``tag`` can also be passed a callable that\n"
"   takes the class qualname and returns a valid tag value (e.g.\n"
"   ``tag=str.lower``). See the docs for more information.\n"
"tag_field: str or None, default None\n"
"   The field name to use for tagged union support. If ``tag`` is non-None,\n"
"   then this defaults to ``\"type\"``. See the ``tag`` docs above for more\n"
"   information.\n"
"rename: str, mapping, callable, or None, default None\n"
"   Controls renaming the field names used when encoding/decoding the struct.\n"
"   May be one of ``\"lower\"``, ``\"upper\"``, ``\"camel\"``, ``\"pascal\"``, or\n"
"   ``\"kebab\"`` to rename in lowercase, UPPERCASE, camelCase, PascalCase,\n"
"   or kebab-case respectively. May also be a mapping from field names to the\n"
"   renamed names (missing fields are not renamed). Alternatively, may be a\n"
"   callable that takes the field name and returns a new name or ``None`` to\n"
"   not rename that field. Default is ``None`` for no field renaming.\n"
"repr_omit_defaults: bool, default False\n"
"   Whether fields should be omitted from the generated repr if the\n"
"   corresponding value is the default for that field.\n"
"array_like: bool, default False\n"
"   If True, this struct type will be treated as an array-like type during\n"
"   encoding/decoding, rather than a dict-like type (the default). This may\n"
"   improve performance, at the cost of a more inscrutable message encoding.\n"
"gc: bool, default True\n"
"   Whether garbage collection is enabled for this type. Disabling this *may*\n"
"   help reduce GC pressure, but will prevent reference cycles composed of only\n"
"   ``gc=False`` from being collected. It is the user's responsibility to ensure\n"
"   that reference cycles don't occur when setting ``gc=False``.\n"
"weakref: bool, default False\n"
"   Whether instances of this type support weak references. Defaults to False.\n"
"dict: bool, default False\n"
"   Whether instances of this type will include a ``__dict__``. Setting this to\n"
"   True will allow adding additional undeclared attributes to a struct instance,\n"
"   which may be useful for holding private runtime state. Defaults to False.\n"
"cache_hash: bool, default False\n"
"   If enabled, the hash of a frozen struct instance will be computed at most\n"
"   once, and then cached on the instance for further reuse. For expensive\n"
"   hash values this can improve performance at the cost of a small amount of\n"
"   memory usage.\n"
"validate_on_init: bool, default False\n"
"   If True, the generated ``__init__`` validates the field values against\n"
"   their declared types and constraints when an instance is created, raising\n"
"   a ``ValidationError`` if any value does not match. Default is False.\n"
"\n"
"Examples\n"
"--------\n"
"Here we define a new `Struct` type for describing a dog. It has three fields;\n"
"two required and one optional.\n"
"\n"
">>> class Dog(Struct):\n"
"...     name: str\n"
"...     breed: str\n"
"...     is_good_boy: bool = True\n"
"...\n"
">>> Dog('snickers', breed='corgi')\n"
"Dog(name='snickers', breed='corgi', is_good_boy=True)\n"
"\n"
"Additional struct options can be set as part of the class definition. Here\n"
"we define a new `Struct` type for a frozen `Point` object.\n"
"\n"
">>> class Point(Struct, frozen=True):\n"
"...     x: float\n"
"...     y: float\n"
"...\n"
">>> {Point(1.5, 2.0): 1}  # frozen structs are hashable\n"
"{Point(x=1.5, y=2.0): 1}"
);

static int
LiteralInfo_traverse(LiteralInfo *self, visitproc visit, void *arg)
{
    Py_VISIT(self->str_lookup);
    Py_VISIT(self->int_lookup);
    return 0;
}

static int
LiteralInfo_clear(LiteralInfo *self)
{
    Py_CLEAR(self->str_lookup);
    Py_CLEAR(self->int_lookup);
    return 0;
}

static void
LiteralInfo_dealloc(LiteralInfo *self)
{
    PyObject_GC_UnTrack(self);
    LiteralInfo_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject LiteralInfo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.LiteralInfo",
    .tp_basicsize = sizeof(LiteralInfo),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)LiteralInfo_clear,
    .tp_traverse = (traverseproc)LiteralInfo_traverse,
    .tp_dealloc = (destructor)LiteralInfo_dealloc,
};

static PyObject *
TypedDictInfo_Convert_lock_held(PyObject *obj) {
    PyObject *annotations = NULL, *required = NULL;
    TypedDictInfo *info = NULL;
    StructspecState *mod = structtype_get_global_state();
    bool cache_set = false, succeeded = false;

    PyObject *cached = NULL;
    if (get_structtype_cache(mod, obj, &TypedDictInfo_Type, &cached)) {
        return cached;
    }

    /* Not cached, extract fields from TypedDict object */
    PyObject *temp = PyObject_CallOneArg(mod->get_typeddict_info, obj);
    if (temp == NULL) return NULL;
    annotations = PyTuple_GET_ITEM(temp, 0);
    Py_INCREF(annotations);
    required = PyTuple_GET_ITEM(temp, 1);
    Py_INCREF(required);
    Py_DECREF(temp);

    /* Allocate and zero-out a new TypedDictInfo object */
    Py_ssize_t nfields = PyDict_GET_SIZE(annotations);
    info = PyObject_GC_NewVar(TypedDictInfo, &TypedDictInfo_Type, nfields);
    if (info == NULL) goto cleanup;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        info->fields[i].key = NULL;
        info->fields[i].type = NULL;
    }
    /* Initialize nrequired to -1 as a flag in case of a recursive TypedDict
    * definition. */
    info->nrequired = -1;
#ifdef Py_GIL_DISABLED
    atomic_store(&info->initialized, 0);
#endif

    /* If not already cached, then cache on TypedDict object _before_
    * traversing fields. This is to ensure self-referential TypedDicts work. */
    if (PyObject_SetAttr(obj, mod->str___structtype_cache__, (PyObject *)info) < 0) {
        goto cleanup;
    }
    cache_set = true;

    /* Traverse fields and initialize TypedDictInfo */
    Py_ssize_t pos = 0, i = 0;
    PyObject *key, *val;
    while (PyDict_Next(annotations, &pos, &key, &val)) {
        TypeNode *type = TypeNode_Convert(val);
        if (type == NULL) goto cleanup;
        Py_INCREF(key);
        info->fields[i].key = key;
        info->fields[i].type = type;
        int contains = PySet_Contains(required, key);
        if (contains == -1) goto cleanup;
        if (contains) { type->types |= MS_EXTRA_FLAG; }
        i++;
    }
    info->nrequired = PySet_GET_SIZE(required);

    PyObject_GC_Track(info);
#ifdef Py_GIL_DISABLED
    atomic_store(&info->initialized, 1);
#endif
    succeeded = true;

cleanup:
    if (!succeeded) {
        Py_CLEAR(info);
        if (cache_set) {
            /* An error occurred after the cache was created and set on the
            * TypedDict. We need to delete the attribute. Fetch and restore the
            * original exception to avoid DelAttr silently clearing it on rare
            * occasions. */
            PyObject *err_type, *err_value, *err_tb;
            PyErr_Fetch(&err_type, &err_value, &err_tb);
            PyObject_DelAttr(obj, mod->str___structtype_cache__);
            PyErr_Restore(err_type, err_value, err_tb);
        }
    }
    Py_XDECREF(annotations);
    Py_XDECREF(required);
    return (PyObject *)info;
}

static PyObject *
TypedDictInfo_Convert(PyObject *obj) {
    PyObject *res = NULL;
    Py_BEGIN_CRITICAL_SECTION(obj);
    res = TypedDictInfo_Convert_lock_held(obj);
    Py_END_CRITICAL_SECTION();
    return res;
}

static MS_INLINE PyObject *
TypedDictInfo_lookup_key(
    TypedDictInfo *self, const char * key, Py_ssize_t key_size,
    TypeNode **type, Py_ssize_t *pos
) {
    const char *field;
    Py_ssize_t nfields, field_size, i, offset = *pos;
    nfields = Py_SIZE(self);
    for (i = offset; i < nfields; i++) {
        field = unicode_str_and_size_nocheck(self->fields[i].key, &field_size);
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i < (nfields - 1) ? (i + 1) : 0;
            *type = self->fields[i].type;
            return self->fields[i].key;
        }
    }
    for (i = 0; i < offset; i++) {
        field = unicode_str_and_size_nocheck(self->fields[i].key, &field_size);
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i + 1;
            *type = self->fields[i].type;
            return self->fields[i].key;
        }
    }
    return NULL;
}

static void
TypedDictInfo_error_missing(TypedDictInfo *self, PyObject *dict, PathNode *path) {
    Py_ssize_t nfields = Py_SIZE(self);
    for (Py_ssize_t i = 0; i < nfields; i++) {
        if (self->fields[i].type->types & MS_EXTRA_FLAG) {
            PyObject *field = self->fields[i].key;
            int contains = PyDict_Contains(dict, field);
            if (contains < 0) return;
            if (contains == 0) {
                ms_missing_required_field(field, path);
                return;
            }
        }
    }
    // Should be unreachable, but may happen if the TypedDict info
    // is inconsistent.
    assert(0);
}

static int
TypedDictInfo_traverse(TypedDictInfo *self, visitproc visit, void *arg)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        TypedDictField *field = &(self->fields[i]);
        if (field->key != NULL) {
            int out = TypeNode_traverse(field->type, visit, arg);
            if (out != 0) return out;
        }
    }
    return 0;
}

static int
TypedDictInfo_clear(TypedDictInfo *self)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        Py_CLEAR(self->fields[i].key);
        TypeNode_Free(self->fields[i].type);
        self->fields[i].type = NULL;
    }
    return 0;
}

static void
TypedDictInfo_dealloc(TypedDictInfo *self)
{
    PyObject_GC_UnTrack(self);
    TypedDictInfo_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject TypedDictInfo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.TypedDictInfo",
    .tp_basicsize = sizeof(TypedDictInfo),
    .tp_itemsize = sizeof(TypedDictField),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)TypedDictInfo_clear,
    .tp_traverse = (traverseproc)TypedDictInfo_traverse,
    .tp_dealloc = (destructor)TypedDictInfo_dealloc,
};

static PyObject *
DataclassInfo_Convert_lock_held(PyObject *obj) {
    PyObject *cls = NULL, *fields = NULL, *field_defaults = NULL;
    PyObject *pre_init = NULL, *post_init = NULL;
    DataclassInfo *info = NULL;
    StructspecState *mod = structtype_get_global_state();
    bool cache_set = false, succeeded = false;

    PyObject *cached = NULL;
    if (get_structtype_cache(mod, obj, &DataclassInfo_Type, &cached)) {
        return cached;
    }

    /* Not cached, extract fields from Dataclass object */
    PyObject *temp = PyObject_CallOneArg(mod->get_dataclass_info, obj);
    if (temp == NULL) return NULL;
    cls = PyTuple_GET_ITEM(temp, 0);
    Py_INCREF(cls);
    fields = PyTuple_GET_ITEM(temp, 1);
    Py_INCREF(fields);
    field_defaults = PyTuple_GET_ITEM(temp, 2);
    Py_INCREF(field_defaults);
    pre_init = PyTuple_GET_ITEM(temp, 3);
    Py_INCREF(pre_init);
    post_init = PyTuple_GET_ITEM(temp, 4);
    Py_INCREF(post_init);
    Py_DECREF(temp);

    /* Allocate and zero-out a new DataclassInfo object */
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    info = PyObject_GC_NewVar(DataclassInfo, &DataclassInfo_Type, nfields);
    if (info == NULL) goto cleanup;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        info->fields[i].key = NULL;
        info->fields[i].type = NULL;
    }
    Py_INCREF(field_defaults);
    info->defaults = field_defaults;
    Py_INCREF(cls);
    info->class = cls;
    if (pre_init == Py_None) {
        info->pre_init = NULL;
    }
    else {
        Py_INCREF(pre_init);
        info->pre_init = pre_init;
    }
    if (post_init == Py_None) {
        info->post_init = NULL;
    }
    else {
        Py_INCREF(post_init);
        info->post_init = post_init;
    }
#ifdef Py_GIL_DISABLED
    atomic_store(&info->initialized, 0);
#endif

    /* If not already cached, then cache on Dataclass object _before_
    * traversing fields. This is to ensure self-referential Dataclasses work. */
    if (PyObject_SetAttr(obj, mod->str___structtype_cache__, (PyObject *)info) < 0) {
        goto cleanup;
    }
    cache_set = true;

    /* Traverse fields and initialize DataclassInfo */
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(fields, i);
        TypeNode *type = TypeNode_Convert(PyTuple_GET_ITEM(field, 1));
        if (type == NULL) goto cleanup;
        /* If field has a default factory, set extra flag bit */
        if (PyObject_IsTrue(PyTuple_GET_ITEM(field, 2))) {
            type->types |= MS_EXTRA_FLAG;
        }
        info->fields[i].type = type;
        info->fields[i].key = PyTuple_GET_ITEM(field, 0);
        Py_INCREF(info->fields[i].key);
    }

    PyObject_GC_Track(info);
#ifdef Py_GIL_DISABLED
    atomic_store(&info->initialized, 1);
#endif
    succeeded = true;

cleanup:
    if (!succeeded) {
        Py_CLEAR(info);
        if (cache_set) {
            /* An error occurred after the cache was created and set on the
            * Dataclass. We need to delete the attribute. Fetch and restore the
            * original exception to avoid DelAttr silently clearing it on rare
            * occasions. */
            PyObject *err_type, *err_value, *err_tb;
            PyErr_Fetch(&err_type, &err_value, &err_tb);
            PyObject_DelAttr(obj, mod->str___structtype_cache__);
            PyErr_Restore(err_type, err_value, err_tb);
        }
    }
    Py_XDECREF(cls);
    Py_XDECREF(fields);
    Py_XDECREF(field_defaults);
    Py_XDECREF(pre_init);
    Py_XDECREF(post_init);
    return (PyObject *)info;
}

static PyObject *
DataclassInfo_Convert(PyObject *obj) {
    PyObject *res = NULL;
    Py_BEGIN_CRITICAL_SECTION(obj);
    res = DataclassInfo_Convert_lock_held(obj);
    Py_END_CRITICAL_SECTION();
    return res;
}

static MS_INLINE PyObject *
DataclassInfo_lookup_key(
    DataclassInfo *self, const char * key, Py_ssize_t key_size,
    TypeNode **type, Py_ssize_t *pos
) {
    const char *field;
    Py_ssize_t nfields, field_size, i, offset = *pos;
    nfields = Py_SIZE(self);
    for (i = offset; i < nfields; i++) {
        field = unicode_str_and_size_nocheck(self->fields[i].key, &field_size);
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i < (nfields - 1) ? (i + 1) : 0;
            *type = self->fields[i].type;
            return self->fields[i].key;
        }
    }
    for (i = 0; i < offset; i++) {
        field = unicode_str_and_size_nocheck(self->fields[i].key, &field_size);
        if (key_size == field_size && memcmp(key, field, key_size) == 0) {
            *pos = i + 1;
            *type = self->fields[i].type;
            return self->fields[i].key;
        }
    }
    return NULL;
}


static int
DataclassInfo_post_decode(DataclassInfo *self, PyObject *obj, PathNode *path) {
    Py_ssize_t nfields = Py_SIZE(self);
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(self->defaults);

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *name = self->fields[i].key;
        if (!PyObject_HasAttr(obj, name)) {
            if (i < (nfields - ndefaults)) {
                ms_missing_required_field(name, path);
                return -1;
            }
            else {
                PyObject *default_value = PyTuple_GET_ITEM(
                    self->defaults, i - (nfields - ndefaults)
                );
                bool is_factory = self->fields[i].type->types & MS_EXTRA_FLAG;
                if (is_factory) {
                    default_value = PyObject_CallNoArgs(default_value);
                    if (default_value == NULL) return -1;
                }
                int status = PyObject_GenericSetAttr(obj, name, default_value);
                if (is_factory) {
                    Py_DECREF(default_value);
                }
                if (status < 0) return -1;
            }
        }
    }
    if (self->post_init != NULL) {
        PyObject *res = PyObject_CallOneArg(self->post_init, obj);
        if (res == NULL) {
            ms_maybe_wrap_validation_error(path);
            return -1;
        }
        Py_DECREF(res);
    }
    return 0;
}

static int
DataclassInfo_traverse(DataclassInfo *self, visitproc visit, void *arg)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        DataclassField *field = &(self->fields[i]);
        if (field->key != NULL) {
            int out = TypeNode_traverse(field->type, visit, arg);
            if (out != 0) return out;
        }
    }
    Py_VISIT(self->defaults);
    Py_VISIT(self->class);
    Py_VISIT(self->pre_init);
    Py_VISIT(self->post_init);
    return 0;
}

static int
DataclassInfo_clear(DataclassInfo *self)
{
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        Py_CLEAR(self->fields[i].key);
        TypeNode_Free(self->fields[i].type);
        self->fields[i].type = NULL;
    }
    Py_CLEAR(self->defaults);
    Py_CLEAR(self->class);
    Py_CLEAR(self->pre_init);
    Py_CLEAR(self->post_init);
    return 0;
}

static void
DataclassInfo_dealloc(DataclassInfo *self)
{
    PyObject_GC_UnTrack(self);
    DataclassInfo_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject DataclassInfo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.DataclassInfo",
    .tp_basicsize = sizeof(DataclassInfo),
    .tp_itemsize = sizeof(DataclassField),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)DataclassInfo_clear,
    .tp_traverse = (traverseproc)DataclassInfo_traverse,
    .tp_dealloc = (destructor)DataclassInfo_dealloc,
};

static PyObject *
NamedTupleInfo_Convert_lock_held(PyObject *obj) {
    StructspecState *mod = structtype_get_global_state();
    NamedTupleInfo *info = NULL;
    PyObject *annotations = NULL, *cls = NULL, *fields = NULL;
    PyObject *defaults = NULL, *defaults_list = NULL;
    bool cache_set = false, succeeded = false;

    PyObject *cached = NULL;
    if (get_structtype_cache(mod, obj, &NamedTupleInfo_Type, &cached)) {
        return cached;
    }

    /* Not cached, extract fields from NamedTuple object */
    annotations = PyObject_CallOneArg(mod->get_class_annotations, obj);
    if (annotations == NULL) goto cleanup;

    if (PyType_Check(obj)) {
        Py_INCREF(obj);
        cls = obj;
    }
    else {
        cls = PyObject_GetAttr(obj, mod->str___origin__);
        if (cls == NULL) goto cleanup;
    }

    fields = PyObject_GetAttr(cls, mod->str__fields);
    if (fields == NULL) goto cleanup;
    defaults = PyObject_GetAttr(cls, mod->str__field_defaults);
    if (defaults == NULL) goto cleanup;

    /* Allocate and zero-out a new NamedTupleInfo object */
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    info = PyObject_GC_NewVar(NamedTupleInfo, &NamedTupleInfo_Type, nfields);
    if (info == NULL) goto cleanup;
    info->class = NULL;
    info->defaults = NULL;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        info->types[i] = NULL;
    }
#ifdef Py_GIL_DISABLED
    atomic_store(&info->initialized, 0);
#endif

    /* If not already cached, then cache on NamedTuple object _before_
    * traversing fields. This is to ensure self-referential NamedTuple work. */
    if (PyObject_SetAttr(obj, mod->str___structtype_cache__, (PyObject *)info) < 0) {
        goto cleanup;
    }
    cache_set = true;

    /* Traverse fields and initialize NamedTupleInfo */
    defaults_list = PyList_New(0);
    if (defaults_list == NULL) goto cleanup;
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = PyTuple_GET_ITEM(fields, i);
        /* Get the field type, defaulting to Any */
        PyObject *type_obj = PyDict_GetItem(annotations, field);
        if (type_obj == NULL) {
            type_obj = mod->typing_any;
        }
        /* Convert the type to a TypeNode */
        TypeNode *type = TypeNode_Convert(type_obj);
        if (type == NULL) goto cleanup;
        info->types[i] = type;
        /* Get the field default (if any), and append it to the list */
        PyObject *default_obj = PyDict_GetItem(defaults, field);
        if (default_obj != NULL) {
            if (PyList_Append(defaults_list, default_obj) < 0) goto cleanup;
        }
    }
    Py_INCREF(cls);
    info->class = cls;
    info->defaults = PyList_AsTuple(defaults_list);
    if (info->defaults == NULL) goto cleanup;
    PyObject_GC_Track(info);
#ifdef Py_GIL_DISABLED
    atomic_store(&info->initialized, 1);
#endif

    succeeded = true;

cleanup:
    if (!succeeded) {
        Py_CLEAR(info);
        if (cache_set) {
            /* An error occurred after the cache was created and set on the
            * NamedTuple. We need to delete the attribute. Fetch and restore
            * the original exception to avoid DelAttr silently clearing it
            * on rare occasions. */
            PyObject *err_type, *err_value, *err_tb;
            PyErr_Fetch(&err_type, &err_value, &err_tb);
            PyObject_DelAttr(obj, mod->str___structtype_cache__);
            PyErr_Restore(err_type, err_value, err_tb);
        }
    }
    Py_XDECREF(cls);
    Py_XDECREF(annotations);
    Py_XDECREF(fields);
    Py_XDECREF(defaults);
    Py_XDECREF(defaults_list);
    return (PyObject *)info;
}

static PyObject *
NamedTupleInfo_Convert(PyObject *obj) {
    PyObject *res = NULL;
    Py_BEGIN_CRITICAL_SECTION(obj);
    res = NamedTupleInfo_Convert_lock_held(obj);
    Py_END_CRITICAL_SECTION();
    return res;
}

static int
NamedTupleInfo_traverse(NamedTupleInfo *self, visitproc visit, void *arg)
{
    Py_VISIT(self->class);
    Py_VISIT(self->defaults);
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        int out = TypeNode_traverse(self->types[i], visit, arg);
        if (out != 0) return out;
    }
    return 0;
}

static int
NamedTupleInfo_clear(NamedTupleInfo *self)
{
    Py_CLEAR(self->class);
    Py_CLEAR(self->defaults);
    for (Py_ssize_t i = 0; i < Py_SIZE(self); i++) {
        TypeNode_Free(self->types[i]);
        self->types[i] = NULL;
    }
    return 0;
}

static void
NamedTupleInfo_dealloc(NamedTupleInfo *self)
{
    PyObject_GC_UnTrack(self);
    NamedTupleInfo_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyTypeObject NamedTupleInfo_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype._core.NamedTupleInfo",
    .tp_basicsize = sizeof(NamedTupleInfo),
    .tp_itemsize = sizeof(TypeNode *),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_clear = (inquiry)NamedTupleInfo_clear,
    .tp_traverse = (traverseproc)NamedTupleInfo_traverse,
    .tp_dealloc = (destructor)NamedTupleInfo_dealloc,
};



/*************************************************************************
 * Dataclass Utilities                                                   *
 *************************************************************************/

typedef struct {
    PyObject *obj;
    PyObject *fields;
    PyObject *dict;
    Py_ssize_t fields_pos;
    Py_ssize_t dict_pos;
    bool fastpath;
    bool standard_getattr;
} DataclassIter;

static bool
dataclass_iter_setup(DataclassIter *iter, PyObject *obj, PyObject *fields) {
    iter->dict = NULL;

    if (MS_UNLIKELY(!PyDict_CheckExact(fields))) {
        PyErr_Format(PyExc_RuntimeError, "%R.__dataclass_fields__ is not a dict", obj);
        return false;
    }

    iter->obj = obj;
    iter->fields = fields;
    iter->fields_pos = 0;
    iter->dict_pos = 0;
    iter->fastpath = false;
    iter->standard_getattr = (
        Py_TYPE(obj)->tp_getattro == PyObject_GenericGetAttr
    );
    if (iter->standard_getattr) {
        iter->dict = PyObject_GenericGetDict(obj, NULL);
        if (iter->dict == NULL) {
            PyErr_Clear();
        }
        else if (PyDict_GET_SIZE(fields) == PyDict_GET_SIZE(iter->dict)) {
            iter->fastpath = true;
        }
    }
    return true;
}

static void
dataclass_iter_cleanup(DataclassIter *iter) {
    Py_XDECREF(iter->dict);
}

static MS_INLINE bool
dataclass_iter_next(
    DataclassIter *iter,
    PyObject **field_name,
    PyObject **field_val
) {
    PyObject *name, *key, *val;

next_field:
    if (!PyDict_Next(iter->fields, &(iter->fields_pos), &name, NULL)) {
        return false;
    }
    if (MS_UNLIKELY(!PyUnicode_CheckExact(name))) goto next_field;

    if (MS_LIKELY(iter->fastpath)) {
        if (
            PyDict_Next(iter->dict, &(iter->dict_pos), &key, &val) &&
            (key == name)
        ) {
            Py_INCREF(val);
            goto found_val;
        }
        else {
            iter->fastpath = false;
        }
    }

    PyTypeObject *type = Py_TYPE(iter->obj);

    if (MS_LIKELY(iter->standard_getattr)) {
        if (iter->dict != NULL) {
            /* We know it's already hashed, it came from a dict */
            Py_hash_t hash = ((PyASCIIObject *)name)->hash;
            val = _PyDict_GetItem_KnownHash(iter->dict, name, hash);
            if (val != NULL) {
                Py_INCREF(val);
                goto found_val;
            }
        }

        PyObject *descr = _PyType_Lookup(type, name);
        if (descr != NULL) {
            descrgetfunc get = Py_TYPE(descr)->tp_descr_get;
            descrsetfunc set = Py_TYPE(descr)->tp_descr_set;
            if (get != NULL && set != NULL) {
                Py_INCREF(descr);
                val = get(descr, iter->obj, (PyObject *)type);
                Py_DECREF(descr);
                if (val != NULL) goto found_val;
                PyErr_Clear();
            }
        }
        goto next_field;
    }
    else {
        val = type->tp_getattro(iter->obj, name);
        if (val == NULL) {
            PyErr_Clear();
            goto next_field;
        }
        goto found_val;
    }

found_val:
    if (MS_UNLIKELY(val == UNSET)) {
        Py_DECREF(val);
        goto next_field;
    }
    *field_name = name;
    *field_val = val;
    return true;
}/*************************************************************************
 * Object Utilities                                                      *
 *************************************************************************//*************************************************************************
 * Shared Encoder structs/methods                                        *
 *************************************************************************/

#define ENC_INIT_BUFSIZE 32
#define ENC_LINES_INIT_BUFSIZE 1024

enum decimal_format {
    DECIMAL_FORMAT_STRING = 0,
    DECIMAL_FORMAT_NUMBER = 1,
    DECIMAL_FORMAT_CALLABLE = 2,
};

enum uuid_format {
    UUID_FORMAT_CANONICAL = 0,
    UUID_FORMAT_HEX = 1,
};

typedef struct EncoderState {
    StructspecState *mod;          /* module reference */
    PyObject *codecs;           /* current per-field {type: dump} map, or NULL */
    PyObject *decimal_callable; /* `decimal_format` callable */
    enum decimal_format decimal_format;
    enum uuid_format uuid_format;
    bool sort_keys;
    char* (*resize_buffer)(PyObject**, Py_ssize_t);  /* callback for resizing buffer */
    bool in_decimal_callable;

    char *output_buffer_raw;    /* raw pointer to output_buffer internal buffer */
    Py_ssize_t output_len;      /* Length of output_buffer */
    Py_ssize_t max_output_len;  /* Allocation size of output_buffer */
    PyObject *output_buffer;    /* bytes or bytearray storing the output */
} EncoderState;

typedef struct Encoder {
    PyObject_HEAD
    PyObject *decimal_callable;
    StructspecState *mod;
    enum decimal_format decimal_format;
    enum uuid_format uuid_format;
    bool sort_keys;
} Encoder;

static char*
ms_resize_bytes(PyObject** output_buffer, Py_ssize_t size)
{
    int status = _PyBytes_Resize(output_buffer, size);
    if (status < 0) return NULL;
    return PyBytes_AS_STRING(*output_buffer);
}

static MS_NOINLINE int
ms_resize(EncoderState *self, Py_ssize_t size)
{
    /* Calculate growth: size + size/2, with overflow check */
    Py_ssize_t growth = size / 2;
    if (size > PY_SSIZE_T_MAX - growth) {
        PyErr_SetString(PyExc_OverflowError, "encoded output is too large");
        return -1;
    }
    Py_ssize_t new_size = size + growth;
    if (new_size < 8) new_size = 8;
    self->max_output_len = new_size;
    char *new_buf = self->resize_buffer(&self->output_buffer, self->max_output_len);
    if (new_buf == NULL) return -1;
    self->output_buffer_raw = new_buf;
    return 0;
}

static MS_INLINE int
ms_ensure_space(EncoderState *self, Py_ssize_t size) {
    if (size < 0 || self->output_len > PY_SSIZE_T_MAX - size) {
        PyErr_SetString(PyExc_OverflowError, "encoded output is too large");
        return -1;
    }
    Py_ssize_t required = self->output_len + size;
    if (MS_UNLIKELY(required > self->max_output_len)) {
        return ms_resize(self, required);
    }
    return 0;
}

static MS_INLINE int
ms_write(EncoderState *self, const char *s, Py_ssize_t n)
{
    if (n < 0 || self->output_len > PY_SSIZE_T_MAX - n) {
        PyErr_SetString(PyExc_OverflowError, "encoded output is too large");
        return -1;
    }
    Py_ssize_t required = self->output_len + n;
    if (MS_UNLIKELY(required > self->max_output_len)) {
        if (ms_resize(self, required) < 0) return -1;
    }
    memcpy(self->output_buffer_raw + self->output_len, s, n);
    self->output_len += n;
    return 0;
}

static int
Encoder_init(Encoder *self, PyObject *args, PyObject *kwds)
{
    char *kwlist[] = {"decimal_format", "uuid_format", "sort_keys", NULL};
    PyObject *decimal_format = NULL, *uuid_format = NULL, *sort_keys = NULL;

    if (
        !PyArg_ParseTupleAndKeywords(
            args, kwds, "|$OOO", kwlist,
            &decimal_format, &uuid_format, &sort_keys
        )
    ) {
        return -1;
    }

    /* Process decimal format */
    self->decimal_callable = NULL;
    if (decimal_format == NULL) {
        self->decimal_format = DECIMAL_FORMAT_STRING;
    }
    else if (PyCallable_Check(decimal_format)) {
        self->decimal_format = DECIMAL_FORMAT_CALLABLE;
        Py_INCREF(decimal_format);
        self->decimal_callable = decimal_format;
    }
    else {
        bool ok = false;
        if (PyUnicode_CheckExact(decimal_format)) {
            if (PyUnicode_CompareWithASCIIString(decimal_format, "string") == 0) {
                self->decimal_format = DECIMAL_FORMAT_STRING;
                ok = true;
            }
            else if (PyUnicode_CompareWithASCIIString(decimal_format, "number") == 0) {
                self->decimal_format = DECIMAL_FORMAT_NUMBER;
                ok = true;
            }
        }
        if (!ok) {
            PyErr_Format(
                PyExc_ValueError,
                "`decimal_format` must be 'string', 'number', or a callable, got %R",
                decimal_format
            );
            return -1;
        }
    }

    /* Process uuid format */
    if (uuid_format == NULL) {
        self->uuid_format = UUID_FORMAT_CANONICAL;
    }
    else {
        bool ok = false;
        if (PyUnicode_CheckExact(uuid_format)) {
            if (PyUnicode_CompareWithASCIIString(uuid_format, "canonical") == 0) {
                self->uuid_format = UUID_FORMAT_CANONICAL;
                ok = true;
            }
            else if (PyUnicode_CompareWithASCIIString(uuid_format, "hex") == 0) {
                self->uuid_format = UUID_FORMAT_HEX;
                ok = true;
            }
        }
        if (!ok) {
            PyErr_Format(
                PyExc_ValueError,
                "`uuid_format` must be 'canonical' or 'hex', got %R",
                uuid_format
            );
            return -1;
        }
    }

    /* Process sort_keys */
    if (parse_sort_keys_arg(sort_keys, &self->sort_keys) < 0) return -1;

    self->mod = structtype_get_global_state();
    return 0;
}

static int
Encoder_traverse(Encoder *self, visitproc visit, void *arg)
{
    Py_VISIT(self->decimal_callable);
    return 0;
}

static int
Encoder_clear(Encoder *self)
{
    Py_CLEAR(self->decimal_callable);
    return 0;
}

static void
Encoder_dealloc(Encoder *self)
{
    PyObject_GC_UnTrack(self);
    Encoder_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

PyDoc_STRVAR(Encoder_encode__doc__,
"encode(self, obj)\n"
"--\n"
"\n"
"Serialize an object to bytes.\n"
"\n"
"Parameters\n"
"----------\n"
"obj : Any\n"
"    The object to serialize.\n"
"\n"
"Returns\n"
"-------\n"
"data : bytes\n"
"    The serialized object.\n"
);
static PyObject*
encoder_encode_common(
    Encoder *self,
    PyObject *const *args,
    Py_ssize_t nargs,
    int(*encode)(EncoderState*, PyObject*)
)
{
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;

    EncoderState state = {
        .mod = self->mod,
        .codecs = NULL,
        .decimal_format = self->decimal_format,
        .decimal_callable = self->decimal_callable,
        .in_decimal_callable = false,
        .uuid_format = self->uuid_format,
        .sort_keys = self->sort_keys,
        .output_len = 0,
        .max_output_len = ENC_INIT_BUFSIZE,
        .resize_buffer = &ms_resize_bytes
    };
    state.output_buffer = PyBytes_FromStringAndSize(NULL, state.max_output_len);
    if (state.output_buffer == NULL) return NULL;
    state.output_buffer_raw = PyBytes_AS_STRING(state.output_buffer);

    if (encode(&state, args[0]) < 0) {
        Py_DECREF(state.output_buffer);
        return NULL;
    }
    FAST_BYTES_SHRINK(state.output_buffer, state.output_len);
    return state.output_buffer;
}

static PyObject*
encode_common(
    PyObject *module,
    PyObject *const *args,
    Py_ssize_t nargs,
    PyObject *kwnames,
    int(*encode)(EncoderState*, PyObject*)
)
{
    PyObject *sort_keys = NULL, *decimal_format = NULL, *uuid_format = NULL;
    StructspecState *mod = structtype_get_state(module);

    /* Parse arguments */
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        if ((sort_keys = find_keyword(kwnames, args + nargs, mod->str_sort_keys)) != NULL) nkwargs--;
        if ((decimal_format = find_keyword(kwnames, args + nargs, mod->str_decimal_format)) != NULL) nkwargs--;
        if ((uuid_format = find_keyword(kwnames, args + nargs, mod->str_uuid_format)) != NULL) nkwargs--;
        if (nkwargs > 0) {
            PyErr_SetString(
                PyExc_TypeError,
                "Extra keyword arguments provided"
            );
            return NULL;
        }
    }

    /* Process decimal format */
    int dec_fmt = DECIMAL_FORMAT_STRING;
    PyObject *dec_callable = NULL;
    if (decimal_format == Py_None) { decimal_format = NULL; }
    if (decimal_format != NULL) {
        if (PyCallable_Check(decimal_format)) {
            dec_fmt = DECIMAL_FORMAT_CALLABLE;
            dec_callable = decimal_format;
        }
        else if (PyUnicode_CheckExact(decimal_format)) {
            if (PyUnicode_CompareWithASCIIString(decimal_format, "string") == 0) {
                dec_fmt = DECIMAL_FORMAT_STRING;
            }
            else if (PyUnicode_CompareWithASCIIString(decimal_format, "number") == 0) {
                dec_fmt = DECIMAL_FORMAT_NUMBER;
            }
            else {
                PyErr_Format(PyExc_ValueError,
                    "`decimal_format` must be 'string', 'number', or a callable, got %R",
                    decimal_format
                );
                return NULL;
            }
        }
        else {
            PyErr_Format(PyExc_ValueError,
                "`decimal_format` must be 'string', 'number', or a callable, got %R",
                decimal_format
            );
            return NULL;
        }
    }

    /* Process uuid format */
    int uuid_fmt = UUID_FORMAT_CANONICAL;
    if (uuid_format == Py_None) { uuid_format = NULL; }
    if (uuid_format != NULL) {
        if (PyUnicode_CheckExact(uuid_format)) {
            if (PyUnicode_CompareWithASCIIString(uuid_format, "canonical") == 0) {
                uuid_fmt = UUID_FORMAT_CANONICAL;
            }
            else if (PyUnicode_CompareWithASCIIString(uuid_format, "hex") == 0) {
                uuid_fmt = UUID_FORMAT_HEX;
            }
            else {
                PyErr_Format(PyExc_ValueError,
                    "`uuid_format` must be 'canonical' or 'hex', got %R",
                    uuid_format
                );
                return NULL;
            }
        }
        else {
            PyErr_Format(PyExc_ValueError,
                "`uuid_format` must be 'canonical' or 'hex', got %R",
                uuid_format
            );
            return NULL;
        }
    }

    EncoderState state = {
        .mod = mod,
        .codecs = NULL,
        .decimal_format = dec_fmt,
        .decimal_callable = dec_callable,
        .in_decimal_callable = false,
        .uuid_format = uuid_fmt,
        .output_len = 0,
        .max_output_len = ENC_INIT_BUFSIZE,
        .resize_buffer = &ms_resize_bytes
    };

    if (parse_sort_keys_arg(sort_keys, &state.sort_keys) < 0) return NULL;

    state.output_buffer = PyBytes_FromStringAndSize(NULL, state.max_output_len);
    if (state.output_buffer == NULL) return NULL;
    state.output_buffer_raw = PyBytes_AS_STRING(state.output_buffer);

    if (encode(&state, args[0]) < 0) {
        Py_DECREF(state.output_buffer);
        return NULL;
    }
    FAST_BYTES_SHRINK(state.output_buffer, state.output_len);
    return state.output_buffer;
}

static PyMemberDef Encoder_members[] = {
    {NULL},
};

static PyObject*
Encoder_decimal_format(Encoder *self, void *closure) {
    if (self->decimal_format == DECIMAL_FORMAT_STRING) {
        return PyUnicode_InternFromString("string");
    }
    else if (self->decimal_format == DECIMAL_FORMAT_NUMBER) {
        return PyUnicode_InternFromString("number");
    }
    else {
        Py_INCREF(self->decimal_callable);
        return self->decimal_callable;
    }
}

static PyObject*
Encoder_uuid_format(Encoder *self, void *closure) {
    if (self->uuid_format == UUID_FORMAT_CANONICAL) {
        return PyUnicode_InternFromString("canonical");
    }
    else {
        return PyUnicode_InternFromString("hex");
    }
}

static PyObject*
Encoder_sort_keys(Encoder *self, void *closure) {
    if (self->sort_keys) {
        Py_RETURN_TRUE;
    }
    else {
        Py_RETURN_FALSE;
    }
}

static PyGetSetDef Encoder_getset[] = {
    {"decimal_format", (getter) Encoder_decimal_format, NULL, NULL, NULL},
    {"uuid_format", (getter) Encoder_uuid_format, NULL, NULL, NULL},
    {"sort_keys", (getter) Encoder_sort_keys, NULL, NULL, NULL},
    {NULL},
};

/*************************************************************************
 * Shared Decoding Utilities                                             *
 *************************************************************************/

static PyObject *
ms_maybe_decode_bool_from_uint64(uint64_t x) {
    if (x == 0) {
        Py_RETURN_FALSE;
    }
    else if (x == 1) {
        Py_RETURN_TRUE;
    }
    return NULL;
}

static PyObject *
ms_maybe_decode_bool_from_int64(int64_t x) {
    if (x == 0) {
        Py_RETURN_FALSE;
    }
    else if (x == 1) {
        Py_RETURN_TRUE;
    }
    return NULL;
}

static PyObject *
ms_decode_str_enum_or_literal(const char *name, Py_ssize_t size, TypeNode *type, PathNode *path) {
    StrLookup *lookup = TypeNode_get_str_enum_or_literal(type);
    return StrLookup_GetOrError(lookup, name, size, path);
}

static PyObject *
ms_decode_int_enum_or_literal_int64(int64_t val, TypeNode *type, PathNode *path) {
    IntLookup *lookup = TypeNode_get_int_enum_or_literal(type);
    return IntLookup_GetInt64OrError(lookup, val, path);
}

static PyObject *
ms_decode_int_enum_or_literal_uint64(uint64_t val, TypeNode *type, PathNode *path) {
    IntLookup *lookup = TypeNode_get_int_enum_or_literal(type);
    return IntLookup_GetUInt64OrError(lookup, val, path);
}

static PyObject *
ms_decode_int_enum_or_literal_pyint(PyObject *val, TypeNode *type, PathNode *path) {
    IntLookup *lookup = TypeNode_get_int_enum_or_literal(type);
    return IntLookup_GetPyIntOrError(lookup, val, path);
}

static PyObject *
ms_decode_custom_struct(PyObject *cls, PyObject *dict, PathNode *path) {
    /* Construct an external struct (e.g. msgspec.Struct) from a Python dict.
     * Uses keyword construction cls(**kwargs) which works for both
     * kw_only and non-kw_only structs. */
    PyObject *fields = PyObject_GetAttrString(cls, "__struct_fields__");
    if (fields == NULL) return NULL;

    PyObject *defaults = PyObject_GetAttrString(cls, "__struct_defaults__");
    if (defaults == NULL) { Py_DECREF(fields); return NULL; }

    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(defaults);
    Py_ssize_t npos = nfields - ndefaults;

    PyObject *kwargs = PyDict_New();
    if (kwargs == NULL) { Py_DECREF(fields); Py_DECREF(defaults); return NULL; }

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *key = PyTuple_GET_ITEM(fields, i);
        PyObject *val = PyDict_GetItemWithError(dict, key);
        if (val == NULL) {
            if (PyErr_Occurred()) { Py_DECREF(kwargs); Py_DECREF(fields); Py_DECREF(defaults); return NULL; }
            if (i >= npos) {
                val = PyTuple_GET_ITEM(defaults, i - npos);
            }
            else {
                ms_raise_validation_error(path, "Field `%U` is required%U", key);
                Py_DECREF(kwargs); Py_DECREF(fields); Py_DECREF(defaults);
                return NULL;
            }
        }
        if (PyDict_SetItem(kwargs, key, val) < 0) {
            Py_DECREF(kwargs); Py_DECREF(fields); Py_DECREF(defaults);
            return NULL;
        }
    }

    PyObject *empty = PyTuple_New(0);
    if (empty == NULL) { Py_DECREF(kwargs); Py_DECREF(fields); Py_DECREF(defaults); return NULL; }

    PyObject *result = PyObject_Call(cls, empty, kwargs);
    Py_DECREF(empty);
    Py_DECREF(kwargs);
    Py_DECREF(fields);
    Py_DECREF(defaults);
    return result;
}

static MS_NOINLINE PyObject *
ms_decode_custom(PyObject *obj, TypeNode* type, PathNode *path) {
    PyObject *custom_cls = NULL, *custom_obj, *out = NULL;
    int status;
    bool generic = type->types & MS_TYPE_CUSTOM_GENERIC;

    if (obj == NULL) return NULL;

    if (obj == Py_None && type->types & MS_TYPE_NONE) return obj;

    custom_obj = TypeNode_get_custom(type);

    out = obj;

    /* Generic classes must be checked based on __origin__ */
    if (generic) {
        StructspecState *st = structtype_get_global_state();
        custom_cls = PyObject_GetAttr(custom_obj, st->str___origin__);
        if (custom_cls == NULL) {
            Py_DECREF(out);
            return NULL;
        }
    }
    else {
        custom_cls = custom_obj;
    }

    /* Field codec (Annotated[X, Field(validate=...)]) takes precedence over the
     * protocol fallback chain, but only when the value isn't already the type. */
    if (type->types & MS_CONSTR_CODEC) {
        Field *codec = (Field *)TypeNode_get_codec(type);
        if (codec->validate != NULL) {
            int is_inst = PyObject_IsInstance(out, custom_cls);
            if (is_inst < 0) {
                Py_DECREF(out);
                if (generic) Py_DECREF(custom_cls);
                return NULL;
            }
            if (!is_inst) {
                PyObject *temp = PyObject_CallOneArg(codec->validate, out);
                Py_DECREF(out);
                if (temp == NULL) {
                    ms_maybe_wrap_validation_error(path);
                    if (generic) Py_DECREF(custom_cls);
                    return NULL;
                }
                out = temp;
                goto done;
            }
        }
    }

    /* Custom type — struct_validate(dict) */
    if (PyObject_HasAttrString(custom_cls, "struct_validate") && out != Py_None) {
        int is_inst = PyObject_IsInstance(out, custom_cls);
        if (is_inst < 0) {
            Py_DECREF(out);
            return NULL;
        }
        if (!is_inst) {
            PyObject *temp = PyObject_CallMethod(custom_cls, "struct_validate", "O", out);
            if (temp == NULL) {
                Py_DECREF(out);
                ms_maybe_wrap_validation_error(path);
                return NULL;
            }
            Py_DECREF(out);
            out = temp;
        }
    }

    /* Pydantic BaseModel — model_validate(dict) */
    if (PyObject_HasAttrString(custom_cls, "model_validate") && out != Py_None) {
        int is_inst = PyObject_IsInstance(out, custom_cls);
        if (is_inst < 0) {
            Py_DECREF(out);
            return NULL;
        }
        if (!is_inst) {
            PyObject *temp = PyObject_CallMethod(custom_cls, "model_validate", "O", out);
            if (temp == NULL) {
                Py_DECREF(out);
                ms_maybe_wrap_validation_error(path);
                return NULL;
            }
            Py_DECREF(out);
            out = temp;
        }
    }

    /* External struct type (e.g. msgspec) — construct from dict */
    else if (PyDict_CheckExact(out) && PyObject_HasAttrString(custom_cls, "__struct_fields__")) {
        PyObject *temp = ms_decode_custom_struct(custom_cls, out, path);
        if (temp == NULL) { Py_DECREF(out); return NULL; }
        Py_DECREF(out);
        out = temp;
    }

    /* Auto-coerce subclasses of built-in types.
     * e.g. OrderedDict(dict), Counter(dict), deque(list), IPv4Address(str) */
    status = PyObject_IsInstance(out, custom_cls);
    if (status == 0 && PyType_IsSubtype((PyTypeObject *)custom_cls, Py_TYPE(out))) {
        PyObject *temp = PyObject_CallOneArg(custom_cls, out);
        if (temp == NULL) {
            Py_DECREF(out);
            ms_maybe_wrap_validation_error(path);
            if (generic) { Py_DECREF(custom_cls); }
            return NULL;
        }
        Py_DECREF(out);
        out = temp;
        status = 1;
    }
    else if (status < 0) {
        Py_DECREF(out);
        if (generic) { Py_DECREF(custom_cls); }
        return NULL;
    }

    /* Check that the decoded value matches the expected type */
    if (status == 0) {
        ms_raise_validation_error(
            path,
            "Expected `%s`, got `%s`%U",
            ((PyTypeObject *)custom_cls)->tp_name,
            Py_TYPE(out)->tp_name
        );
        Py_CLEAR(out);
    }

done:
    if (generic) {
        Py_DECREF(custom_cls);
    }
    return out;
}

static MS_NOINLINE PyObject *
_err_int_constraint(const char *msg, int64_t c, PathNode *path) {
    ms_raise_validation_error(path, msg, c);
    return NULL;
}

static MS_NOINLINE PyObject *
ms_decode_constr_int(int64_t x, TypeNode *type, PathNode *path) {
    if (type->types & MS_CONSTR_INT_MIN) {
        int64_t c = TypeNode_get_constr_int_min(type);
        bool ok = x >= c;
        if (MS_UNLIKELY(!ok)) {
            return _err_int_constraint("Expected `int` >= %lld%U", c, path);
        }
    }
    if (type->types & MS_CONSTR_INT_MAX) {
        int64_t c = TypeNode_get_constr_int_max(type);
        bool ok = x <= c;
        if (MS_UNLIKELY(!ok)) {
            return _err_int_constraint("Expected `int` <= %lld%U", c, path);
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_INT_MULTIPLE_OF)) {
        int64_t c = TypeNode_get_constr_int_multiple_of(type);
        bool ok = (x % c) == 0;
        if (MS_UNLIKELY(!ok)) {
            return _err_int_constraint(
                "Expected `int` that's a multiple of %lld%U", c, path
            );
        }
    }
    return PyLong_FromLongLong(x);
}

static MS_INLINE PyObject *
ms_decode_int(int64_t x, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_INT_CONSTRS)) {
        return ms_decode_constr_int(x, type, path);
    }
    return PyLong_FromLongLong(x);
}

static MS_NOINLINE PyObject *
ms_decode_constr_uint(uint64_t x, TypeNode *type, PathNode *path) {
    if (type->types & MS_CONSTR_INT_MAX) {
        int64_t c = TypeNode_get_constr_int_max(type);
        return _err_int_constraint("Expected `int` <= %lld%U", c, path);
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_INT_MULTIPLE_OF)) {
        int64_t c = TypeNode_get_constr_int_multiple_of(type);
        bool ok = (x % c) == 0;
        if (MS_UNLIKELY(!ok)) {
            return _err_int_constraint(
                "Expected `int` that's a multiple of %lld%U", c, path
            );
        }
    }
    return PyLong_FromUnsignedLongLong(x);
}

static MS_INLINE PyObject *
ms_decode_uint(uint64_t x, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_INT_CONSTRS)) {
        if (MS_LIKELY(x <= LLONG_MAX)) {
            return ms_decode_int(x, type, path);
        }
        return ms_decode_constr_uint(x, type, path);
    }
    return PyLong_FromUnsignedLongLong(x);
}

static MS_NOINLINE bool
ms_passes_int_constraints(uint64_t ux, bool neg, TypeNode *type, PathNode *path) {
    if (type->types & MS_CONSTR_INT_MIN) {
        int64_t c = TypeNode_get_constr_int_min(type);
        bool ok = (
            neg ? ((-(int64_t)ux) >= c) :
            ((c < 0) || (ux >= (uint64_t)c))
        );
        if (MS_UNLIKELY(!ok)) {
            _err_int_constraint("Expected `int` >= %lld%U", c, path);
            return false;
        }
    }
    if (type->types & MS_CONSTR_INT_MAX) {
        int64_t c = TypeNode_get_constr_int_max(type);
        bool ok = (
            neg ? ((-(int64_t)ux) <= c) :
            ((c >= 0) && (ux <= (uint64_t)c))
        );
        if (MS_UNLIKELY(!ok)) {
            _err_int_constraint("Expected `int` <= %lld%U", c, path);
            return false;
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_INT_MULTIPLE_OF)) {
        int64_t c = TypeNode_get_constr_int_multiple_of(type);
        bool ok = (ux % c) == 0;
        if (MS_UNLIKELY(!ok)) {
            _err_int_constraint(
                "Expected `int` that's a multiple of %lld%U", c, path
            );
            return false;
        }
    }
    return true;
}

/* Constraint checks for a PyLong that is known not to fit into a uint64/int64 */
static bool
ms_passes_big_int_constraints(PyObject *obj, TypeNode *type, PathNode *path) {
#if PY314_PLUS
    /* obj is always a PyLong here, so PyLong_GetSign can't fail */
    int sign = 0;
    (void)PyLong_GetSign(obj, &sign);
    bool neg = sign < 0;
#else
    bool neg = _PyLong_Sign(obj) < 0;
#endif

    if (type->types & MS_CONSTR_INT_MIN) {
        if (neg) {
            int64_t c = TypeNode_get_constr_int_min(type);
            _err_int_constraint("Expected `int` >= %lld%U", c, path);
            return false;
        }
    }
    if (type->types & MS_CONSTR_INT_MAX) {
        if (!neg) {
            int64_t c = TypeNode_get_constr_int_max(type);
            _err_int_constraint("Expected `int` <= %lld%U", c, path);
            return false;
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_INT_MULTIPLE_OF)) {
        int64_t c = TypeNode_get_constr_int_multiple_of(type);
        PyObject *base = PyLong_FromLongLong(c);
        if (base == NULL) return false;
        PyObject *remainder = PyNumber_Remainder(obj, base);
        Py_DECREF(base);
        if (remainder == NULL) return false;
        long iremainder = PyLong_AsLong(remainder);
        Py_DECREF(remainder);
        if (iremainder != 0) {
            _err_int_constraint(
                "Expected `int` that's a multiple of %lld%U", c, path
            );
            return false;
        }
    }
    return true;
}

static MS_NOINLINE PyObject *
ms_decode_big_pyint(PyObject *obj, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_INT_CONSTRS)) {
        if (!ms_passes_big_int_constraints(obj, type, path)) return NULL;
    }
    if (MS_LIKELY(PyLong_CheckExact(obj))) {
        Py_INCREF(obj);
        return obj;
    }
    else {
        return PyNumber_Long(obj);
    }
}

static MS_INLINE PyObject *
ms_decode_pyint(PyObject *obj, TypeNode *type, PathNode *path) {
    uint64_t ux;
    bool neg, overflow;
    overflow = fast_long_extract_parts(obj, &neg, &ux);
    if (MS_UNLIKELY(overflow)) {
        return ms_decode_big_pyint(obj, type, path);
    }
    if (MS_UNLIKELY(type->types & MS_INT_CONSTRS)) {
        if (!ms_passes_int_constraints(ux, neg, type, path)) return NULL;
    }
    if (MS_LIKELY(PyLong_CheckExact(obj))) {
        Py_INCREF(obj);
        return obj;
    }
    if (!neg) {
        return PyLong_FromUnsignedLongLong(ux);
    }
    return PyLong_FromLongLong(-(int64_t)ux);
}

static MS_NOINLINE PyObject *
ms_decode_bigint(const char *buf, Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (size > 4300) goto out_of_range;
    /* CPython int parsing routine requires NULL terminated buffer */
    char *temp = (char *)PyMem_Malloc(size + 1);
    if (temp == NULL) {
        return PyErr_NoMemory();
    }
    memcpy(temp, buf, size);
    temp[size] = '\0';
    PyObject *out = PyLong_FromString(temp, NULL, 10);
    PyMem_Free(temp);

    /* We already know the int is a valid integer string. An error here is
     * either a ValueError due to the int being too big (to prevent DDOS
     * issues), or some issue in the VM. We convert the former to out-of-range
     * errors for uniformity, and raise the latter directly. */
    if (MS_UNLIKELY(out == NULL)) {
        PyObject *exc_type, *exc, *tb;

        /* Fetch the exception state. Take a reference to each if not NULL */
        PyErr_Fetch(&exc_type, &exc, &tb);

        if (exc_type == NULL) {
            /* Some other c-extension has borked, just return.
            * References might still have been taken, so decred just in case */
            Py_XDECREF(exc_type);
            Py_XDECREF(exc);
            Py_XDECREF(tb);
            return NULL;
        }
        else if (exc_type == PyExc_ValueError) {
            Py_XDECREF(exc_type);
            Py_XDECREF(exc);
            Py_XDECREF(tb);
            goto out_of_range;
        }
        else {
            /* Restore the exception state.
            * No need to decref here as PyErr_Restore steals references to the args */
            PyErr_Restore(exc_type, exc, tb);
        }
    }

    if (MS_UNLIKELY(type->types & MS_INT_CONSTRS)) {
        if (!ms_passes_big_int_constraints(out, type, path)) {
            Py_CLEAR(out);
        }
    }
    return out;

out_of_range:
    return ms_error_with_path("Integer value out of range%U", path);
}

static MS_NOINLINE PyObject *
_err_float_constraint(
    const char *msg, int offset, double c, PathNode *path
) {
    if (offset == 1) {
        c = nextafter(c, DBL_MAX);
    }
    else if (offset == -1) {
        c = nextafter(c, -DBL_MAX);
    }
    PyObject *py_c = PyFloat_FromDouble(c);
    if (py_c != NULL) {
        ms_raise_validation_error(path, "Expected `float` %s %R%U", msg, py_c);
        Py_DECREF(py_c);
    }
    return NULL;
}

static MS_INLINE bool
ms_passes_float_constraints_inline(double x, TypeNode *type, PathNode *path) {
    if (type->types & (MS_CONSTR_FLOAT_GE | MS_CONSTR_FLOAT_GT)) {
        double c = TypeNode_get_constr_float_min(type);
        bool ok = x >= c;
        if (MS_UNLIKELY(!ok)) {
            bool eq = type->types & MS_CONSTR_FLOAT_GE;
            _err_float_constraint(
                eq ? ">=" : ">",
                eq ? 0 : -1,
                c,
                path
            );
            return false;
        }
    }
    if (type->types & (MS_CONSTR_FLOAT_LE | MS_CONSTR_FLOAT_LT)) {
        double c = TypeNode_get_constr_float_max(type);
        bool ok = x <= c;
        if (MS_UNLIKELY(!ok)) {
            bool eq = type->types & MS_CONSTR_FLOAT_LE;
            _err_float_constraint(
                eq ? "<=" : "<",
                eq ? 0 : 1,
                c,
                path
            );
            return false;
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_FLOAT_MULTIPLE_OF)) {
        double c = TypeNode_get_constr_float_multiple_of(type);
        bool ok = x == 0 || fmod(x, c) == 0.0;
        if (MS_UNLIKELY(!ok)) {
            _err_float_constraint(
                "that's a multiple of", 0, c, path
            );
            return false;
        }
    }
    return true;
}

static MS_NOINLINE PyObject *
ms_decode_constr_float(double x, TypeNode *type, PathNode *path) {
    if (!ms_passes_float_constraints_inline(x, type, path)) return NULL;
    return PyFloat_FromDouble(x);
}

static MS_INLINE PyObject *
ms_decode_float(double x, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_FLOAT_CONSTRS)) {
        return ms_decode_constr_float(x, type, path);
    }
    return PyFloat_FromDouble(x);
}

static MS_NOINLINE PyObject *
_ms_check_float_constraints(PyObject *obj, TypeNode *type, PathNode *path) {
    double x = PyFloat_AS_DOUBLE(obj);
    if (ms_passes_float_constraints_inline(x, type, path)) return obj;
    Py_DECREF(obj);
    return NULL;
}

static MS_INLINE PyObject *
ms_check_float_constraints(PyObject *obj, TypeNode *type, PathNode *path) {
    if (MS_LIKELY(!(type->types & MS_FLOAT_CONSTRS))) return obj;
    return _ms_check_float_constraints(obj, type, path);
}

static MS_NOINLINE bool
_err_py_ssize_t_constraint(const char *msg, Py_ssize_t c, PathNode *path) {
    ms_raise_validation_error(path, msg, c);
    return false;
}

static MS_NOINLINE PyObject *
_ms_check_str_constraints(PyObject *obj, TypeNode *type, PathNode *path) {
    if (obj == NULL) return NULL;

    Py_ssize_t len = PyUnicode_GET_LENGTH(obj);

    if (type->types & MS_CONSTR_STR_MIN_LENGTH) {
        Py_ssize_t c = TypeNode_get_constr_str_min_length(type);
        if (len < c) {
            _err_py_ssize_t_constraint(
                "Expected `str` of length >= %zd%U", c, path
            );
            goto error;
        }
    }
    if (type->types & MS_CONSTR_STR_MAX_LENGTH) {
        Py_ssize_t c = TypeNode_get_constr_str_max_length(type);
        if (len > c) {
            _err_py_ssize_t_constraint(
                "Expected `str` of length <= %zd%U", c, path
            );
            goto error;
        }
    }
    if (type->types & MS_CONSTR_STR_REGEX) {
        PyObject *regex = TypeNode_get_constr_str_regex(type);
        PyObject *res = PyObject_CallMethod(regex, "search", "O", obj);
        if (res == NULL) goto error;
        bool ok = (res != Py_None);
        Py_DECREF(res);
        if (!ok) {
            PyObject *pattern = PyObject_GetAttrString(regex, "pattern");
            if (pattern == NULL) goto error;
            ms_raise_validation_error(
                path, "Expected `str` matching regex %R%U", pattern
            );
            Py_DECREF(pattern);
            goto error;
        }
    }
    return obj;

error:
    Py_DECREF(obj);
    return NULL;
}

static MS_INLINE PyObject *
ms_check_str_constraints(PyObject *obj, TypeNode *type, PathNode *path) {
    if (MS_LIKELY(!(type->types & MS_STR_CONSTRS))) return obj;
    return _ms_check_str_constraints(obj, type, path);
}

static bool
ms_passes_bytes_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_CONSTR_BYTES_MIN_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_bytes_min_length(type);
        if (size < c) {
            return _err_py_ssize_t_constraint(
                "Expected `bytes` of length >= %zd%U", c, path
            );
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_BYTES_MAX_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_bytes_max_length(type);
        if (size > c) {
            return _err_py_ssize_t_constraint(
                "Expected `bytes` of length <= %zd%U", c, path
            );
        }
    }
    return true;
}

static MS_NOINLINE bool
_ms_passes_array_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_CONSTR_ARRAY_MIN_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_array_min_length(type);
        if (size < c) {
            return _err_py_ssize_t_constraint(
                "Expected `array` of length >= %zd%U", c, path
            );
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_ARRAY_MAX_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_array_max_length(type);
        if (size > c) {
            return _err_py_ssize_t_constraint(
                "Expected `array` of length <= %zd%U", c, path
            );
        }
    }
    return true;
}

static MS_INLINE bool
ms_passes_array_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_ARRAY_CONSTRS)) {
        return _ms_passes_array_constraints(size, type, path);
    }
    return true;
}

static MS_NOINLINE bool
_ms_passes_map_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_CONSTR_MAP_MIN_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_map_min_length(type);
        if (size < c) {
            return _err_py_ssize_t_constraint(
                "Expected `object` of length >= %zd%U", c, path
            );
        }
    }
    if (MS_UNLIKELY(type->types & MS_CONSTR_MAP_MAX_LENGTH)) {
        Py_ssize_t c = TypeNode_get_constr_map_max_length(type);
        if (size > c) {
           return _err_py_ssize_t_constraint(
                "Expected `object` of length <= %zd%U", c, path
            );
        }
    }
    return true;
}

static MS_INLINE bool
ms_passes_map_constraints(Py_ssize_t size, TypeNode *type, PathNode *path) {
    if (MS_UNLIKELY(type->types & MS_MAP_CONSTRS)) {
        return _ms_passes_map_constraints(size, type, path);
    }
    return true;
}

static bool
ms_passes_tz_constraint(
    PyObject *tz, TypeNode *type, PathNode *path
) {
    char *err, *type_str;
    if (tz == Py_None) {
        if (type->types & MS_CONSTR_TZ_AWARE) {
            err = "Expected `%s` with a timezone component%U";
            goto error;
        }
    }
    else if (type->types & MS_CONSTR_TZ_NAIVE) {
        err = "Expected `%s` with no timezone component%U";
        goto error;
    }
    return true;

error:
    if (type->types & MS_TYPE_TIME) {
        type_str = "time";
    }
    else {
        type_str = "datetime";
    }

    ms_raise_validation_error(path, err, type_str);
    return false;
}

static int
ms_encode_err_type_unsupported(PyTypeObject *type) {
    PyErr_Format(
        PyExc_TypeError,
        "Encoding objects of type %.200s is unsupported",
        type->tp_name
    );
    return -1;
}

/*************************************************************************
 * Datetime utilities                                                    *
 *************************************************************************/

#define MS_HAS_TZINFO(o)  (((_PyDateTime_BaseTZInfo *)(o))->hastzinfo)
#define MS_DATE_GET_TZINFO(o) PyDateTime_DATE_GET_TZINFO(o)
#define MS_TIME_GET_TZINFO(o) PyDateTime_TIME_GET_TZINFO(o)

/* Returns a new reference */
static PyObject*
timezone_from_offset(int32_t offset) {
    PyObject *delta = PyDelta_FromDSU(0, offset * 60, 0);
    if (delta == NULL) return NULL;
    PyObject *tz = PyTimeZone_FromOffset(delta);
    Py_DECREF(delta);
    return tz;
}

static bool
is_leap_year(int year)
{
    unsigned int y = (unsigned int)year;
    return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

static int
days_in_month(int year, int month) {
    static const uint8_t ndays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year))
        return 29;
    else
        return ndays[month - 1];
}

static MS_NOINLINE int
datetime_round_up_micros(
    int *year, int *month, int *day, int *hour,
    int *minute, int *second, int *microsecond
) {
    ++*microsecond;
    if (*microsecond == 1000000) {
        *microsecond = 0;
        ++*second;
        if (*second == 60) {
            *second = 0;
            ++*minute;
            if (*minute == 60) {
                *minute = 0;
                ++*hour;
                if (*hour == 24) {
                    *hour = 0;
                    ++*day;
                    if (*day == days_in_month(*year, *month) + 1) {
                        ++*month;
                        *day = 1;
                        if (*month > 12) {
                            *month = 1;
                            ++*year;
                            if (*year > 9999) return -1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

static MS_NOINLINE void
time_round_up_micros(
    int *hour, int *minute, int *second, int *microsecond
) {
    ++*microsecond;
    if (*microsecond == 1000000) {
        *microsecond = 0;
        ++*second;
        if (*second == 60) {
            *second = 0;
            ++*minute;
            if (*minute == 60) {
                *minute = 0;
                ++*hour;
                if (*hour == 24) {
                    *hour = 0;
                }
            }
        }
    }
}

/* Python datetimes bounded between (inclusive)
 * [0001-01-01T00:00:00.000000, 9999-12-31T23:59:59.999999] UTC */
#define MS_EPOCH_SECS_MAX 253402300800
#define MS_EPOCH_SECS_MIN -62135596800
#define MS_DAYS_PER_400Y (365*400 + 97)
#define MS_DAYS_PER_100Y (365*100 + 24)
#define MS_DAYS_PER_4Y   (365*4 + 1)

/* Epoch -> datetime conversion borrowed and modified from the implementation
 * in musl, found at
 * http://git.musl-libc.org/cgit/musl/tree/src/time/__secs_to_tm.c. musl is
 * copyright Rich Felker et. al, and is licensed under the standard MIT
 * license.  */
static PyObject *
datetime_from_epoch(
    int64_t epoch_secs, int64_t epoch_nanos, TypeNode *type, PathNode *path
) {
    /* Error on out-of-bounds datetimes. This leaves ample space in an int, so
     * no need to check for overflow later. */
    if (epoch_secs < MS_EPOCH_SECS_MIN || epoch_secs > MS_EPOCH_SECS_MAX) {
        return ms_error_with_path("Timestamp is out of range %U", path);
    }

    int64_t years, days, secs;
    int micros, months, remdays, remsecs, remyears;
    int qc_cycles, c_cycles, q_cycles;

    /* Round nanos to nearest microsecond, adjusting seconds as needed */
    micros = (int)DIV_ROUND_CLOSEST(epoch_nanos, 1000);
    if (micros == 1000000) {
        micros = 0;
        epoch_secs++;
    }

    /* Start in Mar not Jan, so leap day is on end */
    static const char days_in_month[] = {31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 31, 29};

    /* Offset to 2000-03-01, a mod 400 year, immediately after feb 29 */
    secs = epoch_secs - (946684800LL + 86400 * (31 + 29));
    days = secs / 86400;
    remsecs = secs % 86400;
    if (remsecs < 0) {
        remsecs += 86400;
        days--;
    }

    qc_cycles = (int)(days / MS_DAYS_PER_400Y);
    remdays = days % MS_DAYS_PER_400Y;
    if (remdays < 0) {
        remdays += MS_DAYS_PER_400Y;
        qc_cycles--;
    }

    c_cycles = remdays / MS_DAYS_PER_100Y;
    if (c_cycles == 4) c_cycles--;
    remdays -= c_cycles * MS_DAYS_PER_100Y;

    q_cycles = remdays / MS_DAYS_PER_4Y;
    if (q_cycles == 25) q_cycles--;
    remdays -= q_cycles * MS_DAYS_PER_4Y;

    remyears = remdays / 365;
    if (remyears == 4) remyears--;
    remdays -= remyears * 365;

    years = remyears + 4*q_cycles + 100*c_cycles + 400LL*qc_cycles;

    for (months = 0; days_in_month[months] <= remdays; months++)
        remdays -= days_in_month[months];

    if (months >= 10) {
        months -= 12;
        years++;
    }

    if (!ms_passes_tz_constraint(PyDateTime_TimeZone_UTC, type, path)) return NULL;
    return PyDateTimeAPI->DateTime_FromDateAndTime(
        (int)(years + 2000),
        months + 3,
        remdays + 1,
        remsecs / 3600,
        remsecs / 60 % 60,
        remsecs % 60,
        micros,
        PyDateTime_TimeZone_UTC,
        PyDateTimeAPI->DateTimeType
    );
}

static inline const char *
ms_read_fixint(const char *buf, int width, int *out) {
    int x = 0;
    for (int i = 0; i < width; i++) {
        char c = *buf++;
        if (!is_digit(c)) return NULL;
        x = x * 10 + (c - '0');
    }
    *out = x;
    return buf;
}

/* Requires 10 bytes of scratch space */
static void
ms_encode_date(PyObject *obj, char *out)
{
    uint32_t year = PyDateTime_GET_YEAR(obj);
    uint8_t month = PyDateTime_GET_MONTH(obj);
    uint8_t day = PyDateTime_GET_DAY(obj);

    write_u32_4_digits(year, out);
    *(out + 4) = '-';
    write_u32_2_digits(month, out + 5);
    *(out + 7) = '-';
    write_u32_2_digits(day, out + 8);
}

/* Requires 21 bytes of scratch space */
static int
ms_encode_time_parts(
    StructspecState *mod, PyObject *datetime_or_none,
    uint8_t hour, uint8_t minute, uint8_t second, uint32_t microsecond,
    PyObject *tzinfo, char *out, int out_offset
) {
    char *p = out + out_offset;
    write_u32_2_digits(hour, p);
    p += 2;
    *p++ = ':';
    write_u32_2_digits(minute, p);
    p += 2;
    *p++ = ':';
    write_u32_2_digits(second, p);
    p += 2;
    if (microsecond) {
        *p++ = '.';
        write_u32_6_digits(microsecond, p);
        p += 6;
    }

    if (tzinfo != Py_None) {
        int32_t offset_days = 0, offset_secs = 0;

        if (tzinfo != PyDateTime_TimeZone_UTC) {
            PyObject *offset = PyObject_CallMethodOneArg(
                tzinfo, mod->str_utcoffset, datetime_or_none
            );
            if (offset == NULL) {
                return -1;
            }
            else if (PyDelta_Check(offset)) {
                offset_days = PyDateTime_DELTA_GET_DAYS(offset);
                offset_secs = PyDateTime_DELTA_GET_SECONDS(offset);
                Py_DECREF(offset);
            }
            else if (offset == Py_None) {
                Py_DECREF(offset);
                goto done;
            }
            else if (offset != Py_None) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "tzinfo.utcoffset returned a non-timedelta object"
                );
                Py_DECREF(offset);
                return -1;
            }
        }
        if (MS_LIKELY(offset_secs == 0)) {
            *p++ = 'Z';
        }
        else {
            char sign = '+';
            if (offset_days == -1) {
                sign = '-';
                offset_secs = 86400 - offset_secs;
            }
            uint8_t offset_hour = offset_secs / 3600;
            uint8_t offset_min = (offset_secs / 60) % 60;
            /* If the offset isn't an even number of minutes, RFC 3339
            * indicates that the offset should be rounded to the nearest
            * possible hour:min pair */
            bool round_up = (offset_secs - (offset_hour * 3600 + offset_min * 60)) > 30;
            if (MS_UNLIKELY(round_up)) {
                offset_min++;
                if (offset_min == 60) {
                    offset_min = 0;
                    offset_hour++;
                    if (offset_hour == 24) {
                        offset_hour = 0;
                    }
                }
            }
            if (offset_hour == 0 && offset_min == 0) {
                *p++ = 'Z';
            }
            else {
                *p++ = sign;
                write_u32_2_digits(offset_hour, p);
                p += 2;
                *p++ = ':';
                write_u32_2_digits(offset_min, p);
                p += 2;
            }
        }
    }

done:
    return (int)(p - out);
}

/* Requires 21 bytes of scratch space max.
 *
 * Returns +nbytes if successful, -1 on failure */
static int
ms_encode_time(StructspecState *mod, PyObject *obj, char *out)
{
    uint8_t hour = PyDateTime_TIME_GET_HOUR(obj);
    uint8_t minute = PyDateTime_TIME_GET_MINUTE(obj);
    uint8_t second = PyDateTime_TIME_GET_SECOND(obj);
    uint32_t microsecond = PyDateTime_TIME_GET_MICROSECOND(obj);
    PyObject *tzinfo = MS_TIME_GET_TZINFO(obj);
    return ms_encode_time_parts(
        mod, Py_None, hour, minute, second, microsecond, tzinfo, out, 0
    );
}

/* Requires 32 bytes of scratch space max.
 *
 * Returns +nbytes if successful, -1 on failure */
static int
ms_encode_datetime(StructspecState *mod, PyObject *obj, char *out)
{
    uint8_t hour = PyDateTime_DATE_GET_HOUR(obj);
    uint8_t minute = PyDateTime_DATE_GET_MINUTE(obj);
    uint8_t second = PyDateTime_DATE_GET_SECOND(obj);
    uint32_t microsecond = PyDateTime_DATE_GET_MICROSECOND(obj);
    PyObject *tzinfo = MS_DATE_GET_TZINFO(obj);
    ms_encode_date(obj, out);
    out[10] = 'T';
    return ms_encode_time_parts(
        mod, obj, hour, minute, second, microsecond, tzinfo, out, 11
    );
}

static PyObject *
ms_decode_date(const char *buf, Py_ssize_t size, PathNode *path) {
    int year, month, day;

    /* A valid date is 10 characters in length */
    if (size != 10) goto invalid;

    /* Parse date */
    if ((buf = ms_read_fixint(buf, 4, &year)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &month)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &day)) == NULL) goto invalid;

    /* Ensure all numbers are valid */
    if (year == 0) goto invalid;
    if (month == 0 || month > 12) goto invalid;
    if (day == 0 || day > days_in_month(year, month)) goto invalid;

    return PyDateTimeAPI->Date_FromDate(year, month, day, PyDateTimeAPI->DateType);

invalid:
    return ms_error_with_path("Invalid RFC3339 encoded date%U", path);
}

static PyObject *
ms_decode_time(const char *buf, Py_ssize_t size, TypeNode *type, PathNode *path) {
    int hour, minute, second, microsecond = 0;
    const char *buf_end = buf + size;
    bool round_up_micros = false;
    PyObject *tz = NULL;
    char c;

    /* A valid time is at least 8 characters in length */
    if (size < 8) goto invalid;

    /* Parse time */
    if ((buf = ms_read_fixint(buf, 2, &hour)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &minute)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &second)) == NULL) goto invalid;

    /* Remaining reads require bounds check */
#define next_or_null() (buf == buf_end) ? '\0' : *buf++
    c = next_or_null();

    if (c == '.') {
        int ndigits = 0;
        while (ndigits < 6) {
            c = next_or_null();
            if (!is_digit(c)) goto end_decimal;
            ndigits++;
            microsecond = microsecond * 10 + (c - '0');
        }
        c = next_or_null();
        if (is_digit(c)) {
            /* This timestamp has higher precision than microseconds; parse
            * the next digit to support rounding, then skip all remaining
            * digits */
            if ((c - '0') >= 5) {
                round_up_micros = true;
            }
            while (true) {
                c = next_or_null();
                if (!is_digit(c)) break;
            }
        }
end_decimal:
        /* Error if no digits after decimal */
        if (ndigits == 0) goto invalid;
        int pow10[6] = {100000, 10000, 1000, 100, 10, 1};
        /* Scale microseconds appropriately */
        microsecond *= pow10[ndigits - 1];
    }
#undef next_or_null

    if (c != '\0') {
        /* Parse timezone */
        int offset = 0;
        if (c == 'Z' || c == 'z') {
            /* Check for trailing characters */
            if (buf != buf_end) goto invalid;
        }
        else {
            int offset_hour, offset_min;
            if (c == '-') {
                offset = -1;
            }
            else if (c == '+') {
                offset = 1;
            }
            else {
                goto invalid;
            }

            if (buf_end - buf < 3) goto invalid;
            if ((buf = ms_read_fixint(buf, 2, &offset_hour)) == NULL) goto invalid;
            /* RFC3339 requires a ':' separator, ISO8601 doesn't. We support
             * either */
            if (*buf == ':') buf++;
            if (buf_end - buf != 2) goto invalid;
            if ((buf = ms_read_fixint(buf, 2, &offset_min)) == NULL) goto invalid;
            if (offset_hour > 23 || offset_min > 59) goto invalid;
            offset *= (offset_hour * 60 + offset_min);
        }
        if (offset == 0) {
            tz = PyDateTime_TimeZone_UTC;
            Py_INCREF(tz);
        }
        else {
            tz = timezone_from_offset(offset);
            if (tz == NULL) goto error;
        }
    }
    else {
        tz = Py_None;
        Py_INCREF(tz);
    }

    /* Ensure all numbers are valid */
    if (hour > 23) goto invalid;
    if (minute > 59) goto invalid;
    if (second > 59) goto invalid;

    if (MS_UNLIKELY(round_up_micros)) {
        time_round_up_micros(&hour, &minute, &second, &microsecond);
    }

    if (!ms_passes_tz_constraint(tz, type, path)) goto error;
    PyObject *out = PyDateTimeAPI->Time_FromTime(
        hour, minute, second, microsecond, tz, PyDateTimeAPI->TimeType
    );
    Py_XDECREF(tz);
    return out;

invalid:
    ms_error_with_path("Invalid RFC3339 encoded time%U", path);
error:
    Py_XDECREF(tz);
    return NULL;
}

static PyObject *
ms_decode_datetime_from_int64(
    int64_t timestamp, TypeNode *type, PathNode *path
) {
    return datetime_from_epoch(timestamp, 0, type, path);
}

static PyObject *
ms_decode_datetime_from_uint64(
    uint64_t timestamp, TypeNode *type, PathNode *path
) {
    if (timestamp > LLONG_MAX) {
        timestamp = LLONG_MAX; /* will raise out of range error later */
    }
    return datetime_from_epoch((int64_t)timestamp, 0, type, path);
}

static PyObject *
ms_decode_datetime_from_float(
    double timestamp, TypeNode *type, PathNode *path
) {
    if (MS_UNLIKELY(!isfinite(timestamp))) {
        return ms_error_with_path("Invalid epoch timestamp%U", path);
    }
    int64_t secs = (int64_t)trunc(timestamp);
    int32_t nanos = (int32_t)(1000000000 * (timestamp - secs));
    if (nanos && timestamp < 0) {
        secs--;
        nanos += 1000000000;
    }
    return datetime_from_epoch(secs, nanos, type, path);
}

static PyObject *
ms_decode_datetime_from_str(
    const char *buf, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    int year, month, day, hour, minute, second, microsecond = 0;
    const char *buf_end = buf + size;
    bool round_up_micros = false;
    PyObject *tz = NULL;
    char c;

    /* A valid datetime is at least 19 characters in length */
    if (size < 19) goto invalid;

    /* Parse date */
    if ((buf = ms_read_fixint(buf, 4, &year)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &month)) == NULL) goto invalid;
    if (*buf++ != '-') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &day)) == NULL) goto invalid;

    /* RFC3339 date/time separator can be T or t. We also support ' ', which is
     * ISO8601 compatible. */
    c = *buf++;
    if (!(c == 'T' || c == 't' || c == ' ')) goto invalid;

    /* Parse time */
    if ((buf = ms_read_fixint(buf, 2, &hour)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &minute)) == NULL) goto invalid;
    if (*buf++ != ':') goto invalid;
    if ((buf = ms_read_fixint(buf, 2, &second)) == NULL) goto invalid;

    /* Remaining reads require bounds check */
#define next_or_null() (buf == buf_end) ? '\0' : *buf++
    c = next_or_null();

    /* Parse decimal if present.
     *
     * Python datetime's only supports microsecond precision, but RFC3339
     * doesn't specify a decimal precision limit. To work around this we
     * support infinite decimal digits, but round to the closest microsecond.
     * This means that nanosecond timestamps won't properly roundtrip through
     * datetime objects, but there's not much we can do about that. Other
     * systems commonly accept 3 or 6 digits, support for/usage of nanosecond
     * precision is rare. */
    if (c == '.') {
        int ndigits = 0;
        while (ndigits < 6) {
            c = next_or_null();
            if (!is_digit(c)) goto end_decimal;
            ndigits++;
            microsecond = microsecond * 10 + (c - '0');
        }
        c = next_or_null();
        if (is_digit(c)) {
            /* This timestamp has higher precision than microseconds; parse
            * the next digit to support rounding, then skip all remaining
            * digits */
            if ((c - '0') >= 5) {
                round_up_micros = true;
            }
            while (true) {
                c = next_or_null();
                if (!is_digit(c)) break;
            }
        }
end_decimal:
        /* Error if no digits after decimal */
        if (ndigits == 0) goto invalid;
        int pow10[6] = {100000, 10000, 1000, 100, 10, 1};
        /* Scale microseconds appropriately */
        microsecond *= pow10[ndigits - 1];
    }
#undef next_or_null

    if (c != '\0') {
        /* Parse timezone */
        int offset = 0;
        if (c == 'Z' || c == 'z') {
            /* Check for trailing characters */
            if (buf != buf_end) goto invalid;
        }
        else {
            int offset_hour, offset_min;
            if (c == '-') {
                offset = -1;
            }
            else if (c == '+') {
                offset = 1;
            }
            else {
                goto invalid;
            }

            if (buf_end - buf < 3) goto invalid;
            if ((buf = ms_read_fixint(buf, 2, &offset_hour)) == NULL) goto invalid;
            /* RFC3339 requires a ':' separator, ISO8601 doesn't. We support
             * either */
            if (*buf == ':') buf++;
            if (buf_end - buf != 2) goto invalid;
            if ((buf = ms_read_fixint(buf, 2, &offset_min)) == NULL) goto invalid;
            if (offset_hour > 23 || offset_min > 59) goto invalid;
            offset *= (offset_hour * 60 + offset_min);
        }
        if (offset == 0) {
            tz = PyDateTime_TimeZone_UTC;
            Py_INCREF(tz);
        }
        else {
            tz = timezone_from_offset(offset);
            if (tz == NULL) goto error;
        }
    }
    else {
        tz = Py_None;
        Py_INCREF(tz);
    }

    /* Ensure all numbers are valid */
    if (year == 0) goto invalid;
    if (month == 0 || month > 12) goto invalid;
    if (day == 0 || day > days_in_month(year, month)) goto invalid;
    if (hour > 23) goto invalid;
    if (minute > 59) goto invalid;
    if (second > 59) goto invalid;

    if (MS_UNLIKELY(round_up_micros)) {
        if (
            datetime_round_up_micros(
                &year, &month, &day, &hour, &minute, &second, &microsecond
            ) < 0
        ) goto invalid;
    }

    if (!ms_passes_tz_constraint(tz, type, path)) goto error;
    PyObject *out = PyDateTimeAPI->DateTime_FromDateAndTime(
        year, month, day, hour, minute, second, microsecond, tz,
        PyDateTimeAPI->DateTimeType
    );
    Py_XDECREF(tz);
    return out;

invalid:
    ms_error_with_path("Invalid RFC3339 encoded datetime%U", path);
error:
    Py_XDECREF(tz);
    return NULL;
}

/* Requires 26 bytes of scratch space */
static int
ms_encode_timedelta(PyObject *obj, char *out) {
    const char *start = out;

    int days = PyDateTime_DELTA_GET_DAYS(obj);
    int secs = PyDateTime_DELTA_GET_SECONDS(obj);
    int micros = PyDateTime_DELTA_GET_MICROSECONDS(obj);

    if (days < 0) {
        *out++ = '-';
        days = -days;
        if (secs != 0 || micros != 0) {
            days--;
            if (micros != 0) {
                micros = 1000000 - micros;
                secs += 1;
            }
            secs = 86400 - secs;
        }
    }
    *out++ = 'P';

    if (days != 0) {
        out = write_u64(days, out);
        *out++ = 'D';
    }
    if (secs != 0 || micros != 0) {
        *out++ = 'T';
        out = write_u64(secs, out);
        if (micros != 0) {
            *out++ = '.';
            write_u32_6_digits(micros, out);
            out += 6;
            while (*(out - 1) == '0') {
                out--;
            }
        }
        *out++ = 'S';
    }
    else if (days == 0) {
        *out++ = '0';
        *out++ = 'D';
    }
    return (int)(out - start);
}

#define MS_TIMEDELTA_MAX_SECONDS (86399999999999LL)
#define MS_TIMEDELTA_MIN_SECONDS (-86399999913600LL)

static PyObject *
ms_timedelta_from_parts(int64_t secs, int micros) {
    int64_t days = secs / (24 * 60 * 60);
    secs -= days * (24 * 60 * 60);
    return PyDelta_FromDSU((int)days, (int)secs, micros);
}

static PyObject *
ms_decode_timedelta_from_uint64(uint64_t x, PathNode *path) {
    if (x > (uint64_t)MS_TIMEDELTA_MAX_SECONDS) {
        return ms_error_with_path("Duration is out of range%U", path);
    }
    return ms_timedelta_from_parts((int64_t)x, 0);
}

static PyObject *
ms_decode_timedelta_from_int64(int64_t x, PathNode *path) {
    if ((x > MS_TIMEDELTA_MAX_SECONDS) || (x < MS_TIMEDELTA_MIN_SECONDS)) {
        return ms_error_with_path("Duration is out of range%U", path);
    }
    return ms_timedelta_from_parts(x, 0);
}

static PyObject *
ms_decode_timedelta_from_float(double x, PathNode *path) {
    if (
        (!isfinite(x)) ||
        (x > (double)MS_TIMEDELTA_MAX_SECONDS) ||
        (x < (double)MS_TIMEDELTA_MIN_SECONDS)
    ) {
        return ms_error_with_path("Duration is out of range%U", path);
    }
    int64_t secs = (int64_t)trunc(x);
    long micros = lround(1000000 * (x - secs));
    return ms_timedelta_from_parts(secs, micros);
}

enum timedelta_parse_state {
    TIMEDELTA_START = 0,
    TIMEDELTA_D = 1,
    TIMEDELTA_T = 2,
    TIMEDELTA_H = 3,
    TIMEDELTA_M = 4,
    TIMEDELTA_S = 5,
};

/* Parses a timedelta from an extended ISO8601 Duration. We try to follow the
 * ISO8601-2 spec as closely as possible, extrapolating as needed when the spec
 * is vague. We've also limited support for some units/ranges as needed to fit
 * python's timedelta model.
 *
 * Values parsed here are compatible with Java's `Time.Duration.parse`, as well
 * as the proposed javascript `Temporal.Duration.from` API.
 *
 * We accept the following format: [-/+]P#DT#H#M#S
 *
 * where # is a number with optional decimal component. Characters are case
 * insensitive.
 *
 * Additional restrictions, mostly taken from the ISO spec. Extrapolations or
 * self-imposed restrictions are noted.
 *
 * - The units D (days), H (hours), M (minutes) and S (seconds) may be omitted
 *   if their value is 0.
 *
 * - At least one value must be present (`P0D` is a valid 0 duration, but `P`
 *   is not).
 *
 * - If present, the values must be in the proper unit order (D, H, M, S).
 *
 * - If a unit value contains a fractional number, it must be the last unit.
 *   `PT1H1.5S` and `PT1.5H` are valid but `PT1.5H1S` is not.
 *
 * - Unlike JSON numbers, leading 0s are fine. `P001D` and `P1D` are
 *   equivalent.
 *
 * - If `T` is present, there must be at least one time component after it.
 *   This isn't explicitly said in the spec, but is enforced by both Java and
 *   Temporal APIs.
 *
 * - A leading `-` or `+` may be used to indicate a negative or positive
 *   duration. This is part of the extended spec in ISO8601-2, and is supported
 *   by both Java and Temporal apis.
 *
 * - We don't support relative duration units (Y (year), M (month), W (week)).
 *   Python's timedeltas are absolute deltas composed of fixed-length days,
 *   seconds, and microseconds. There's no way to accurately store a relative
 *   quantity like "months". Java's Duration implementation has the same
 *   restriction.
 *
 * - This implementation will error for durations that exceed python's
 *   timedelta range limits. The ISO spec provides no restrictions on duration
 *   range.
 */
static PyObject *
ms_decode_timedelta(
    const char *p, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    bool neg = false;
    const char *end = p + size;

    if (p == end) goto invalid;

    if (*p == '-') {
        neg = true;
        p++;
        if (p == end) goto invalid;
    }
    else if (*p == '+') {
        p++;
        if (p == end) goto invalid;
    }

    if (*p != 'P' && *p != 'p') goto invalid;
    p++;
    if (p == end) goto invalid;

    int64_t seconds = 0;
    int micros = 0;
    enum timedelta_parse_state parse_state = TIMEDELTA_START;

    while (true) {
        if (p == end) goto invalid;  /* Missing segment */

        if (*p == 'T' || *p == 't') {
            if (parse_state >= TIMEDELTA_T) goto invalid;
            parse_state = TIMEDELTA_T;
            p++;
            continue;
        }

        uint32_t scale;
        bool has_frac = false;
        uint64_t x = 0, frac_num = 0, frac_den = 10;

        /* Read integral part */
        if (!is_digit(*p)) goto invalid;
        x = *p - '0';
        p++;

        while (true) {
            if (p == end) goto invalid;  /* missing unit */
            if (!is_digit(*p)) break;
            x = x * 10 + (uint64_t)(*p - '0');
            if (x > (1ULL << 47)) goto out_of_range;
            p++;
        }

        if (*p == '.') {
            /* Read fractional part */
            has_frac = true;
            p++;
            if (p == end) goto invalid;
            if (!is_digit(*p)) goto invalid;
            frac_num = *p - '0';
            p++;

            /* Parse up to 10 more decimal digits.
             *
             * Since the largest unit we support is days, and python's
             * timedelta has microsecond precision, only 11 decimal digits need
             * to be read. Any further precision will have no effect on the
             * output. */
            for (int i = 0; i < 10; i++) {
                if (p == end) goto invalid;  /* missing unit */
                if (!is_digit(*p)) goto parse_unit;
                frac_num = frac_num * 10 + (uint64_t)(*p - '0');
                frac_den *= 10;
                p++;
            }
            /* Remaining digits won't have any effect on output, skip them */
            while (true) {
                if (p == end) goto invalid;  /* missing unit */
                if (!is_digit(*p)) goto parse_unit;
                p++;
            }
        }

    parse_unit:

        switch (*p++) {
            case 'D':
            case 'd': {
                if (parse_state >= TIMEDELTA_D) goto invalid;
                parse_state = TIMEDELTA_D;
                scale = 24 * 60 * 60;
                break;
            };
            case 'H':
            case 'h': {
                if (parse_state < TIMEDELTA_T || parse_state >= TIMEDELTA_H) goto invalid;
                parse_state = TIMEDELTA_H;
                scale = 60 * 60;
                break;
            }
            case 'M':
            case 'm': {
                if (parse_state < TIMEDELTA_T) goto unsupported;
                if (parse_state >= TIMEDELTA_M) goto invalid;
                parse_state = TIMEDELTA_M;
                scale = 60;
                break;
            };
            case 'S':
            case 's': {
                if (parse_state < TIMEDELTA_T || parse_state >= TIMEDELTA_S) goto invalid;
                parse_state = TIMEDELTA_S;
                scale = 1;
                break;
            };
            case 'W':
            case 'w':
            case 'Y':
            case 'y':
                goto unsupported;
            default:
                goto invalid;
        }

        /* Apply integral part */
        seconds += scale * x;

        if (has_frac) {
            /* Apply fractional part */
            frac_num *= scale;
            int64_t extra_secs = (int64_t)(frac_num / frac_den);
            int extra_micros = (int)DIV_ROUND_CLOSEST(1000000 * (frac_num - (extra_secs * frac_den)), frac_den);
            micros += extra_micros;
            if (micros >= 1000000) {
                extra_secs++;
                micros -= 1000000;
            }
            seconds += extra_secs;
        }

        /* Ensure seconds are still in-bounds */
        if (seconds > MS_TIMEDELTA_MAX_SECONDS) goto out_of_range;

        if (p == end) {
            break;
        }
        else if (has_frac) {
            goto invalid; /* decimal segment must be last */
        }
    }
    if (neg) {
        seconds = -seconds;
        if (micros) {
            micros = 1000000 - micros;
            seconds -= 1;
        }
        /* abs(lower_limit) < upper_limit, so we need to recheck it here */
        if (seconds < MS_TIMEDELTA_MIN_SECONDS) goto out_of_range;
    }
    int64_t days = seconds / (24 * 60 * 60);
    seconds -= days * (24 * 60 * 60);
    return PyDelta_FromDSU((int)days, (int)seconds, micros);

invalid:
    return ms_error_with_path("Invalid ISO8601 duration%U", path);
out_of_range:
    return ms_error_with_path("Duration is out of range%U", path);
unsupported:
    return ms_error_with_path(
        "Only units 'D', 'H', 'M', and 'S' are supported when parsing ISO8601 durations%U",
        path
    );
}

/*************************************************************************
 * Base64 Encoder                                                        *
 *************************************************************************/

static Py_ssize_t
ms_encode_base64_size(StructspecState *mod, Py_ssize_t input_size) {
    if (input_size >= (1LL << 32)) {
        PyErr_SetString(
            mod->EncodeError,
            "Can't encode bytes-like objects longer than 2**32 - 1"
        );
        return -1;
    }
    /* ceil(4/3 * input_size) */
    return 4 * ((input_size + 2) / 3);
}

static void
ms_encode_base64(const char *input, Py_ssize_t input_size, char *out) {
    int nbits = 0;
    unsigned int charbuf = 0;
    for (; input_size > 0; input_size--, input++) {
        charbuf = (charbuf << 8) | (unsigned char)(*input);
        nbits += 8;
        while (nbits >= 6) {
            unsigned char ind = (charbuf >> (nbits - 6)) & 0x3f;
            nbits -= 6;
            *out++ = base64_encode_table[ind];
        }
    }
    if (nbits == 2) {
        *out++ = base64_encode_table[(charbuf & 3) << 4];
        *out++ = '=';
        *out++ = '=';
    }
    else if (nbits == 4) {
        *out++ = base64_encode_table[(charbuf & 0xf) << 2];
        *out++ = '=';
    }
}

/*************************************************************************
 * UUID Utilities                                                        *
 *************************************************************************/

static int
ms_uuid_to_16_bytes(StructspecState *mod, PyObject *obj, unsigned char *buf) {
    PyObject *int128 = PyObject_GetAttr(obj, mod->str_int);
    if (int128 == NULL) return -1;
    if (MS_UNLIKELY(!PyLong_CheckExact(int128))) {
        PyErr_SetString(PyExc_TypeError, "uuid.int must be an int");
        Py_DECREF(int128);
        return -1;
    }
#if PY313_PLUS
    int out = (int)PyLong_AsNativeBytes(
        int128,
        buf,
        16,
        Py_ASNATIVEBYTES_BIG_ENDIAN | Py_ASNATIVEBYTES_UNSIGNED_BUFFER
    );
#else
    int out = _PyLong_AsByteArray((PyLongObject *)int128, buf, 16, 0, 0);
#endif
    Py_DECREF(int128);
    return out;
}

static PyObject *
ms_uuid_from_16_bytes(const unsigned char *buf) {
    PyObject *int128 = _PyLong_FromByteArray(buf, 16, 0, 0);
    if (int128 == NULL) return NULL;

    StructspecState *mod = structtype_get_global_state();
    PyTypeObject *uuid_type = (PyTypeObject *)(mod->UUIDType);
    PyObject *out = uuid_type->tp_alloc(uuid_type, 0);
    if (out == NULL) goto error;

    /* UUID objects are immutable, use GenericSetAttr instead of SetAttr */
    if (PyObject_GenericSetAttr(out, mod->str_int, int128) < 0) goto error;
    if (PyObject_GenericSetAttr(out, mod->str_is_safe, mod->uuid_safeuuid_unknown) < 0) goto error;

    Py_DECREF(int128);
    return out;

error:
    Py_DECREF(int128);
    Py_XDECREF(out);
    return NULL;
}

/* Requires up to 36 bytes of space */
static int
ms_encode_uuid(StructspecState *mod, PyObject *obj, char *out, bool canonical) {
    unsigned char scratch[16];
    unsigned char *buf = scratch;

    if (ms_uuid_to_16_bytes(mod, obj, scratch) < 0) return -1;

    for (int i = 0; i < 4; i++) {
        unsigned char c = *buf++;
        *out++ = hex_encode_table[c >> 4];
        *out++ = hex_encode_table[c & 0xF];
    }
    if (canonical) *out++ = '-';
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 2; i++) {
            unsigned char c = *buf++;
            *out++ = hex_encode_table[c >> 4];
            *out++ = hex_encode_table[c & 0xF];
        }
        if (canonical) *out++ = '-';
    }
    for (int i = 0; i < 6; i++) {
        unsigned char c = *buf++;
        *out++ = hex_encode_table[c >> 4];
        *out++ = hex_encode_table[c & 0xF];
    }
    return 0;
}

static PyObject *
ms_decode_uuid_from_str(const char *buf, Py_ssize_t size, PathNode *path) {
    unsigned char scratch[16];
    unsigned char *decoded = scratch;
    int segments[] = {4, 2, 2, 2, 6};

    /* A valid uuid is 32 or 36 characters in length */
    if (size != 32 && size != 36) goto invalid;
    bool has_hyphens = size == 36;

    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < segments[i]; j++) {
            char hi = *buf++;
            if (hi >= '0' && hi <= '9') { hi -= '0'; }
            else if (hi >= 'a' && hi <= 'f') { hi = hi - 'a' + 10; }
            else if (hi >= 'A' && hi <= 'F') { hi = hi - 'A' + 10; }
            else { goto invalid; }

            char lo = *buf++;
            if (lo >= '0' && lo <= '9') { lo -= '0'; }
            else if (lo >= 'a' && lo <= 'f') { lo = lo - 'a' + 10; }
            else if (lo >= 'A' && lo <= 'F') { lo = lo - 'A' + 10; }
            else { goto invalid; }

            *decoded++ = ((unsigned char)hi << 4) + (unsigned char)lo;
        }
        if (has_hyphens && i < 4 && *buf++ != '-') goto invalid;
    }
    return ms_uuid_from_16_bytes(scratch);

invalid:
    return ms_error_with_path("Invalid UUID%U", path);
}

static PyObject *
ms_decode_uuid_from_bytes(const char *buf, Py_ssize_t size, PathNode *path) {
    if (size == 16) {
        return ms_uuid_from_16_bytes((unsigned char *)buf);
    }
    return ms_error_with_path("Invalid UUID bytes%U", path);
}

/*************************************************************************
 * Decimal Utilities                                                     *
 *************************************************************************/

static PyObject *
ms_decode_decimal_from_pyobj(PyObject *str, PathNode *path, StructspecState *mod) {
    if (mod == NULL) {
        mod = structtype_get_global_state();
    }
    return PyObject_CallOneArg(mod->DecimalType, str);
}

static PyObject *
ms_decode_decimal_from_pystr(PyObject *str, PathNode *path, StructspecState *mod) {
    PyObject *out = ms_decode_decimal_from_pyobj(str, path, mod);
    if (out == NULL) {
        ms_error_with_path("Invalid decimal string%U", path);
    }
    return out;
}

static PyObject *
ms_decode_decimal(
    const char *view, Py_ssize_t size, bool is_ascii, PathNode *path, StructspecState *mod
) {
    PyObject *str;

    if (MS_LIKELY(is_ascii)) {
        str = PyUnicode_New(size, 127);
        if (str == NULL) return NULL;
        memcpy(ascii_get_buffer(str), view, size);
    }
    else {
        str = PyUnicode_DecodeUTF8(view, size, NULL);
        if (str == NULL) return NULL;
    }
    PyObject *out = ms_decode_decimal_from_pystr(str, path, mod);
    Py_DECREF(str);
    return out;
}

static PyObject *
ms_decode_decimal_from_int64(int64_t x, PathNode *path) {
    PyObject *temp = PyLong_FromLongLong(x);
    if (temp == NULL) return NULL;
    PyObject *out = ms_decode_decimal_from_pyobj(temp, path, NULL);
    Py_DECREF(temp);
    return out;
}

static PyObject *
ms_decode_decimal_from_uint64(uint64_t x, PathNode *path) {
    PyObject *temp = PyLong_FromUnsignedLongLong(x);
    if (temp == NULL) return NULL;
    PyObject *out = ms_decode_decimal_from_pyobj(temp, path, NULL);
    Py_DECREF(temp);
    return out;
}

static PyObject *
ms_decode_decimal_from_float(double val, PathNode *path, StructspecState *mod) {
    if (MS_LIKELY(isfinite(val))) {
        /* For finite values, render as the nearest IEEE754 double in string
         * form, then call decimal.Decimal to parse */
        char buf[24];
        int n = write_f64(val, buf, false);
        return ms_decode_decimal(buf, n, true, path, mod);
    }
    else {
        /* For nonfinite values, convert to float obj and go through python */
        PyObject *temp = PyFloat_FromDouble(val);
        if (temp == NULL) return NULL;
        PyObject *out = ms_decode_decimal_from_pyobj(temp, path, mod);
        Py_DECREF(temp);
        return out;
    }
}


/*************************************************************************
 * strict=False Utilities                                                *
 *************************************************************************/

static bool
maybe_parse_number(
    const char *, Py_ssize_t, TypeNode *, PathNode *, bool, PyObject **
);

static PyObject *
ms_decode_str_lax(
    const char *view,
    Py_ssize_t size,
    TypeNode *type,
    PathNode *path,
    bool *invalid
) {
    if (type->types & (
            MS_TYPE_INT | MS_TYPE_INTENUM | MS_TYPE_INTLITERAL |
            MS_TYPE_BOOL |
            MS_TYPE_FLOAT | MS_TYPE_DECIMAL |
            MS_TYPE_DATETIME | MS_TYPE_TIMEDELTA
        )
    ) {
        PyObject *out;
        if (maybe_parse_number(view, size, type, path, false, &out)) {
            return out;
        }
    }

    if (type->types & MS_TYPE_BOOL) {
        if (size == 4) {
            if (
                (view[0] == 't' || view[0] == 'T') &&
                (view[1] == 'r' || view[1] == 'R') &&
                (view[2] == 'u' || view[2] == 'U') &&
                (view[3] == 'e' || view[3] == 'E')
            ) {
                Py_RETURN_TRUE;
            }
        }
        else if (size == 5) {
            if (
                (view[0] == 'f' || view[0] == 'F') &&
                (view[1] == 'a' || view[1] == 'A') &&
                (view[2] == 'l' || view[2] == 'L') &&
                (view[3] == 's' || view[3] == 'S') &&
                (view[4] == 'e' || view[4] == 'E')
            ) {
                Py_RETURN_FALSE;
            }
        }
    }

    if (type->types & MS_TYPE_NONE) {
        if (size == 4 &&
            (view[0] == 'n' || view[0] == 'N') &&
            (view[1] == 'u' || view[1] == 'U') &&
            (view[2] == 'l' || view[2] == 'L') &&
            (view[3] == 'l' || view[3] == 'L')
        ) {
            Py_RETURN_NONE;
        }
    }
    *invalid = true;
    return NULL;
}

/*************************************************************************
 * Shared Post-Decode Handlers                                           *
 *************************************************************************/

static PyObject *
ms_post_decode_int64(
    int64_t x, TypeNode *type, PathNode *path, bool strict, bool from_str
) {
    if (MS_LIKELY(type->types & (MS_TYPE_ANY | MS_TYPE_INT))) {
        return ms_decode_int(x, type, path);
    }
    else if (type->types & (MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        return ms_decode_int_enum_or_literal_int64(x, type, path);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return ms_decode_float((double)x, type, path);
    }
    else if (type->types & MS_TYPE_DECIMAL) {
        return ms_decode_decimal_from_int64(x, path);
    }
    else if (!strict) {
        if (type->types & MS_TYPE_BOOL) {
            PyObject *out = ms_maybe_decode_bool_from_int64(x);
            if (out != NULL) return out;
        }
        if (type->types & MS_TYPE_DATETIME) {
            return ms_decode_datetime_from_int64(x, type, path);
        }
        if (type->types & MS_TYPE_TIMEDELTA) {
            return ms_decode_timedelta_from_int64(x, path);
        }
    }
    return ms_validation_error(from_str ? "str" : "int", type, path);
}

static PyObject *
ms_post_decode_uint64(
    uint64_t x, TypeNode *type, PathNode *path, bool strict, bool from_str
) {
    if (MS_LIKELY(type->types & (MS_TYPE_ANY | MS_TYPE_INT))) {
        return ms_decode_uint(x, type, path);
    }
    else if (type->types & (MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        return ms_decode_int_enum_or_literal_uint64(x, type, path);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return ms_decode_float((double)x, type, path);
    }
    else if (type->types & MS_TYPE_DECIMAL) {
        return ms_decode_decimal_from_uint64(x, path);
    }
    else if (!strict) {
        if (type->types & MS_TYPE_BOOL) {
            PyObject *out = ms_maybe_decode_bool_from_uint64(x);
            if (out != NULL) return out;
        }
        if (type->types & MS_TYPE_DATETIME) {
            return ms_decode_datetime_from_uint64(x, type, path);
        }
        if (type->types & MS_TYPE_TIMEDELTA) {
            return ms_decode_timedelta_from_uint64(x, path);
        }
    }
    return ms_validation_error(from_str ? "str" : "int", type, path);
}

static bool
double_as_int64(double x, int64_t *out) {
    if (fmod(x, 1.0) != 0.0) return false;
    if (x > (1LL << 53)) return false;
    if (x < -(1LL << 53)) return false;
    *out = (int64_t)x;
    return true;
}

static PyObject *
ms_post_decode_float(
    double x, TypeNode *type, PathNode *path, bool strict, bool from_str
) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT)) {
        return ms_decode_float(x, type, path);
    }
    else if (!strict) {
        if (type->types & MS_TYPE_INT) {
            int64_t out;
            if (double_as_int64(x, &out)) {
                return ms_post_decode_int64(out, type, path, strict, from_str);
            }
        }
        if (type->types & MS_TYPE_DATETIME) {
            return ms_decode_datetime_from_float(x, type, path);
        }
        if (type->types & MS_TYPE_TIMEDELTA) {
            return ms_decode_timedelta_from_float(x, path);
        }
    }
    return ms_validation_error(from_str ? "str" : "float", type, path);
}

/*************************************************************************
 * Number Parser                                                         *
 *************************************************************************/

#define ONE_E18 1000000000000000000ULL
#define ONE_E19_MINUS_ONE 9999999999999999999ULL

static MS_NOINLINE PyObject *
parse_number_fallback(
    const unsigned char* integer_start,
    const unsigned char* integer_end,
    const unsigned char* fraction_start,
    const unsigned char* fraction_end,
    int32_t exp_part,
    bool is_negative,
    TypeNode *type,
    PathNode *path,
    bool strict,
    bool from_str
) {
    uint32_t nd = 0;
    int32_t dp = 0;

    ms_hpd dec;
    dec.num_digits = 0;
    dec.decimal_point = 0;
    dec.negative = is_negative;
    dec.truncated = false;

    /* Parse integer */
    const unsigned char *p = integer_start;
    if (*p != '0') {
        while (p < integer_end) {
            if (MS_LIKELY(nd < MS_HPD_MAX_DIGITS)) {
                dec.digits[nd++] = (uint8_t)(*p - '0');
            }
            else if (*p != '0') {
                dec.truncated = true;
            }
            dp++;
            p++;
        }
    }

    p = fraction_start;
    while (p < fraction_end) {
        if (*p == '0') {
            if (nd == 0) {
                /* Track leading zeros implicitly */
                dp--;
            }
            else if (nd < MS_HPD_MAX_DIGITS) {
                dec.digits[nd++] = (uint8_t)(*p - '0');
            }
        }
        else if ('1' <= *p && *p <= '9') {
            if (nd < MS_HPD_MAX_DIGITS) {
                dec.digits[nd++] = (uint8_t)(*p - '0');
            }
            else {
                dec.truncated = true;
            }
        }
        p++;
    }

    dp += exp_part;

    dec.num_digits = nd;
    if (dp < -MS_HPD_DP_RANGE) {
        dec.decimal_point = -(MS_HPD_DP_RANGE + 1);
    }
    else if (dp > MS_HPD_DP_RANGE) {
        dec.decimal_point = (MS_HPD_DP_RANGE + 1);
    }
    else {
        dec.decimal_point = dp;
    }
    ms_hpd_trim(&dec);
    double res = ms_hpd_to_double(&dec);
    if (Py_IS_INFINITY(res)) {
        return ms_error_with_path("Number out of range%U", path);
    }
    return ms_post_decode_float(res, type, path, strict, from_str);
}

static MS_NOINLINE PyObject *
parse_number_nonfinite(
    const unsigned char *start,
    bool is_negative,
    const unsigned char *p,
    const unsigned char *pend,
    const char **errmsg,
    TypeNode *type,
    PathNode *path,
    bool strict
) {
    size_t size = pend - p;
    double val;
    if (size == 3) {
        if (
            (p[0] == 'n' || p[0] == 'N') &&
            (p[1] == 'a' || p[1] == 'A') &&
            (p[2] == 'n' || p[2] == 'N')
        ) {
            val = NAN;
            goto done;
        }
        else if (
            (p[0] == 'i' || p[0] == 'I') &&
            (p[1] == 'n' || p[1] == 'N') &&
            (p[2] == 'f' || p[2] == 'F')
        ) {
            val = INFINITY;
            goto done;
        }
    }
    else if (size == 8) {
        if (
            (p[0] == 'i' || p[0] == 'I') &&
            (p[1] == 'n' || p[1] == 'N') &&
            (p[2] == 'f' || p[2] == 'F') &&
            (p[3] == 'i' || p[3] == 'I') &&
            (p[4] == 'n' || p[4] == 'N') &&
            (p[5] == 'i' || p[5] == 'I') &&
            (p[6] == 't' || p[6] == 'T') &&
            (p[7] == 'y' || p[7] == 'Y')
        ) {
            val = INFINITY;
            goto done;
        }
    }

    *errmsg = "invalid number";
    return NULL;

done:
    if (
        MS_UNLIKELY(
            !(type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT))
            && type->types & MS_TYPE_DECIMAL
        )
    ) {
        return ms_decode_decimal(
            (char *)start, pend - start, true, path, NULL
        );
    }
    if (is_negative) {
        val = -val;
    }
    return ms_post_decode_float(val, type, path, strict, true);
}

static MS_NOINLINE PyObject *
json_float_hook(
    const char *buf, Py_ssize_t size, PathNode *path, PyObject *float_hook
) {
    PyObject *str = PyUnicode_New(size, 127);
    if (str == NULL) return NULL;
    memcpy(ascii_get_buffer(str), buf, size);
    PyObject *out = PyObject_CallOneArg(float_hook, str);
    Py_DECREF(str);
    if (out == NULL) {
        ms_maybe_wrap_validation_error(path);
        return NULL;
    }
    return out;
}

static MS_INLINE PyObject *
parse_number_inline(
    const unsigned char *p,
    const unsigned char *pend,
    const unsigned char **pout,
    const char **errmsg,
    TypeNode *type,
    PathNode *path,
    bool strict,
    PyObject *float_hook,
    bool from_str
) {
    uint64_t mantissa = 0;
    int64_t exponent = 0;
    int32_t exp_part = 0;
    bool is_negative = false;
    bool is_float = false;
    bool is_truncated = false;

    const unsigned char *start = p;

    /* We know there is at least one byte available when this function is
     * called */
    if (*p == '-') {
        /* Parse minus sign */
        p++;
        if (p == pend) goto invalid_number;
        is_negative = true;
    }

    const unsigned char *integer_start = p;

    /* Parse integer */
    if (MS_UNLIKELY(*p == '0')) {
        p++;
        if (MS_UNLIKELY(p != pend && is_digit(*p))) goto invalid_number;
    }
    else {
        unsigned char c = *p;
        while (MS_LIKELY(is_digit(c))) {
            mantissa = mantissa * 10 + (uint8_t)(c - '0');
            p++;
            if (MS_UNLIKELY(p == pend)) break;
            c = *p;
        }
        /* There must be at least one digit */
        if (MS_UNLIKELY(integer_start == p)) {
            if (MS_UNLIKELY(from_str)) {
                return parse_number_nonfinite(
                    start, is_negative, p, pend, errmsg, type, path, strict
                );
            }
            else {
                goto invalid_character;
            }
        }
    }
    const unsigned char *integer_end = p;

    int64_t digit_count = integer_end - integer_start;

    const unsigned char *fraction_start = NULL;
    const unsigned char *fraction_end = NULL;

    if (MS_UNLIKELY(p == pend)) goto end_parsing;

    if (*p == '.') {
        is_float = true;
        p++;

        /* Parse fraction */
        fraction_start = p;
        while (MS_LIKELY(p != pend && is_digit(*p))) {
            mantissa = mantissa * 10 + (uint8_t)(*p - '0');
            p++;
        }
        /* Error if no digits after decimal */
        if (MS_UNLIKELY(fraction_start == p)) goto invalid_number;
        fraction_end = p;
        exponent = fraction_start - p;
        digit_count -= exponent;
        if (MS_UNLIKELY(p == pend)) goto end_parsing;
    }

    if (*p == 'e' || *p == 'E') {
        is_float = true;
        int32_t exp_sign = 1;

        p++;
        if (p == pend) goto invalid_number;

        /* Parse exponent sign (if any) */
        if (*p == '+') {
            p++;
        }
        else if (*p == '-') {
            p++;
            exp_sign = -1;
        }

        /* Parse exponent digits */
        const unsigned char *start_exponent = p;
        while (p != pend && is_digit(*p)) {
            if (MS_LIKELY(exp_part < 10000)) {
                exp_part = exp_part * 10 + (uint8_t)(*p - '0');
            }
            p++;
        }
        /* Error if no digits in exponent */
        if (MS_UNLIKELY(start_exponent == p)) goto invalid_number;

        exp_part *= exp_sign;
        exponent += exp_part;
    }

    if (MS_UNLIKELY(from_str)) {
        if (MS_UNLIKELY(p != pend)) goto invalid_number;
    }

end_parsing:
    /* Check for overflow and maybe reparse if needed */
    if (MS_UNLIKELY(digit_count > 19)) {
        const unsigned char *cur = integer_start;
        while ((cur != pend) && (*cur == '0' || *cur == '.')) {
            if (*cur == '0') {
                digit_count--;
            }
            cur++;
        }

        if (
            (digit_count > 19) &&
            (
                (is_float) ||
                ((integer_end - integer_start) != 20) ||
                (*integer_start != '1') ||
                (mantissa <= ONE_E19_MINUS_ONE)
            )
        ) {
            /* We overflowed. Redo parsing, truncating at 19 digits */
            is_truncated = true;
            const unsigned char *cur = integer_start;
            mantissa = 0;
            while ((mantissa < ONE_E18) && (cur != integer_end)) {
                mantissa = mantissa * 10 + (uint64_t)(*cur - '0');
                cur++;
            }
            if (mantissa >= ONE_E18) {
                exponent = integer_end - cur + exp_part;
            }
            else {
                cur = fraction_start;
                while ((mantissa < ONE_E18) && (cur != fraction_end)) {
                    mantissa = mantissa * 10 + (uint64_t)(*cur - '0');
                    cur++;
                }
                exponent = fraction_start - cur + exp_part;
            }
        }
    }

    /* Forward output position */
    *pout = p;

    if (!is_float) {
        if (is_negative) {
            is_truncated |= (mantissa > (1ull << 63));
        }
        if (MS_UNLIKELY(is_truncated)) {
            if (type->types & (MS_TYPE_ANY | MS_TYPE_INT)) {
                return ms_decode_bigint((char *)start, p - start, type, path);
            }
        }
        else {
            if (is_negative) {
                return ms_post_decode_int64(
                    -1 * (int64_t)(mantissa), type, path, strict, from_str
                );
            }
            else {
                return ms_post_decode_uint64(mantissa, type, path, strict, from_str);
            }
        }
    }

    if (
        MS_UNLIKELY(
            !(type->types & (MS_TYPE_ANY | MS_TYPE_FLOAT))
            && type->types & MS_TYPE_DECIMAL
        )
    ) {
        return ms_decode_decimal(
            (char *)start, p - start, true, path, NULL
        );
    }
    else if (MS_UNLIKELY(float_hook != NULL && type->types & MS_TYPE_ANY)) {
        return json_float_hook((char *)start, p - start, path, float_hook);
    }
    else {
        if (MS_UNLIKELY(exponent > 288 || exponent < -307)) {
            /* Exponent is out of bounds */
            goto fallback;
        }

        double val;
        if (MS_LIKELY((-22 <= exponent) && (exponent <= 22) && ((mantissa >> 53) == 0))) {
            /* If both `mantissa` and `10 ** exponent` can be exactly
             * represented as a double, we can take a fast path */
            val = (double)(mantissa);
            if (exponent >= 0) {
                val *= ms_atof_f64_powers_of_10[exponent];
            } else {
                val /= ms_atof_f64_powers_of_10[-exponent];
            }
        }
        else if (MS_UNLIKELY(mantissa == 0)) {
            /* Special case 0 handling. This is only hit if the mantissa is 0
             * and the exponent is out of bounds above (i.e. rarely) */
            val = 0.0;
        }
        else {
            int64_t r1 = eisel_lemire(mantissa, (int32_t)exponent);
            if (MS_UNLIKELY(r1 < 0)) goto fallback;
            if (MS_UNLIKELY(is_truncated)) {
                int64_t r2 = eisel_lemire(mantissa + 1, (int32_t)exponent);
                if (r1 != r2) goto fallback;
            }
            uint64_t bits = ((uint64_t)r1);
            memcpy(&val, &bits, sizeof(double));
        }
        if (is_negative) {
            val = -val;
        }
        return ms_post_decode_float(val, type, path, strict, from_str);
    }

fallback:
    return parse_number_fallback(
        integer_start, integer_end,
        fraction_start, fraction_end,
        exp_part,
        is_negative,
        type, path, strict, from_str
    );

invalid_number:
    if (pout != NULL) *pout = p;
    *errmsg = "invalid number";
    return NULL;

invalid_character:
    if (pout != NULL) *pout = p;
    *errmsg = "invalid character";
    return NULL;
}

static MS_NOINLINE bool
maybe_parse_number(
    const char *view,
    Py_ssize_t size,
    TypeNode *type,
    PathNode *path,
    bool strict,
    PyObject **out
) {
    const char *errmsg = NULL;
    const unsigned char *pout;
    *out = parse_number_inline(
        (const unsigned char *)view,
        (const unsigned char *)(view + size),
        &pout,
        &errmsg,
        type,
        path,
        strict,
        NULL,
        true
    );
    return (*out != NULL || errmsg == NULL);
}

/*************************************************************************
 * JSON Encoder                                                          *
 *************************************************************************/

PyDoc_STRVAR(JSONEncoder__doc__,
"Encoder(*, enc_hook=None, decimal_format='string', uuid_format='canonical', sort_keys=False)\n"
"--\n"
"\n"
"A JSON encoder.\n"
"\n"
"Parameters\n"
"----------\n"
"enc_hook : callable, optional\n"
"    A callable to call for objects that aren't supported structtype types. Takes\n"
"    the unsupported object and should return a supported object, or raise a\n"
"    ``NotImplementedError`` if unsupported.\n"
"decimal_format : {'string', 'number'} or callable, optional\n"
"    The format to use for encoding `decimal.Decimal` objects. If 'string'\n"
"    they're encoded as strings, if 'number', they're encoded as floats. If\n"
"    a callable is provided, it will be called with the `decimal.Decimal`\n"
"    object and must return a JSON-serializable value (but **not** another\n"
"    `decimal.Decimal` or a nested structure containing `decimal.Decimal`).\n"
"    Defaults to 'string', which is the recommended value since 'number' may\n"
"    result in precision loss when decoding for some JSON library implementations.\n"
"uuid_format : {'canonical', 'hex'}, optional\n"
"    The format to use for encoding `uuid.UUID` objects. The 'canonical'\n"
"    and 'hex' formats encode them as strings with and without hyphens\n"
"    respectively. Defaults to 'canonical'.\n"
"sort_keys : bool, optional\n"
"    If ``True``, dict keys and set elements are sorted so that values which\n"
"    compare equal produce identical encoded output, regardless of insertion\n"
"    or iteration order. Struct, dataclass, and object fields keep their\n"
"    declaration order. Default is ``False``."
);

static int json_encode_inline(EncoderState*, PyObject*);
static int json_encode(EncoderState*, PyObject*);

static MS_NOINLINE int
json_encode_long_fallback(EncoderState *self, PyObject *obj) {
    int out = -1;
    PyObject *encoded = PyLong_Type.tp_repr(obj);
    if (MS_LIKELY(encoded != NULL)) {
        Py_ssize_t len;
        const char* buf = unicode_str_and_size(encoded, &len);
        if (MS_LIKELY(buf != NULL)) {
            out = ms_write(self, buf, len);
        }
        Py_DECREF(encoded);
    }
    return out;
}

static MS_NOINLINE int
json_encode_long(EncoderState *self, PyObject *obj) {
    uint64_t x;
    bool neg, overflow;
    overflow = fast_long_extract_parts(obj, &neg, &x);
    if (MS_UNLIKELY(overflow)) {
        return json_encode_long_fallback(self, obj);
    }
    if (ms_ensure_space(self, 20) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    if (neg) {
        *p++ = '-';
    }
    self->output_len = write_u64(x, p) - self->output_buffer_raw;
    return 0;
}

static int
json_encode_long_as_str(EncoderState *self, PyObject *obj) {
    if (ms_write(self, "\"", 1) < 0) return -1;
    if (json_encode_long(self, obj) < 0) return -1;
    return ms_write(self, "\"", 1);
}

static MS_NOINLINE int
json_encode_float(EncoderState *self, PyObject *obj) {
    double x = PyFloat_AS_DOUBLE(obj);
    if (ms_ensure_space(self, 24) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    self->output_len += write_f64(x, p, false);
    return 0;
}

static MS_NOINLINE int
json_encode_float_as_str(EncoderState *self, PyObject *obj) {
    double x = PyFloat_AS_DOUBLE(obj);
    if (ms_ensure_space(self, 26) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    *p = '"';
    int n = write_f64(x, p + 1, true);
    *(p + 1 + n) = '"';
    self->output_len += n + 2;
    return 0;
}

/* A table of escape characters to use for each byte (0 if no escape needed) */
static const char escape_table[256] = {
    'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'b', 't', 'n', 'u', 'f', 'r', 'u', 'u',
    'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u',
    0, 0, '"', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '\\', 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static int
json_str_requires_escaping(PyObject *obj) {
    Py_ssize_t i, len;
    const char* buf = unicode_str_and_size(obj, &len);
    if (buf == NULL) return -1;
    for (i = 0; i < len; i++) {
        char escape = escape_table[(uint8_t)buf[i]];
        if (escape != 0) {
            return 1;
        }
    }
    return 0;
}

static MS_INLINE int
json_encode_cstr_inline(EncoderState *self, const char *src, Py_ssize_t len) {
    const char* src_end = src + len;

    if (ms_ensure_space(self, len + 2) < 0) return -1;

    char *out = self->output_buffer_raw + self->output_len;
    char *out_end = self->output_buffer_raw + self->max_output_len;
    *out++ = '"';

noescape:

#define write_ascii_pre(i) \
    if (MS_UNLIKELY(escape_table[(uint8_t)src[i]])) goto write_ascii_##i;

#define write_ascii_post(i) \
    write_ascii_##i: \
    memcpy(out, src, i); \
    out += i; \
    src += i; \
    goto escape;

    while (src_end - src >= 8) {
        repeat8(write_ascii_pre);
        memcpy(out, src, 8);
        out += 8;
        src += 8;
    }

    while (MS_LIKELY(src_end > src)) {
        write_ascii_pre(0);
        *out++ = *src++;
    }

    *out++ = '"';
    self->output_len = out - self->output_buffer_raw;
    return 0;

repeat8(write_ascii_post);

escape:
    {
        char c = *src++;
        char escape = escape_table[(uint8_t)c];

        /* Ensure enough space for the escape, final quote, and any remaining characters */
        Py_ssize_t remaining = 7 + src_end - src;
        if (MS_UNLIKELY(remaining > out_end - out)) {
            Py_ssize_t output_len = out - self->output_buffer_raw;
            if (output_len > PY_SSIZE_T_MAX - remaining) {
                PyErr_SetString(PyExc_OverflowError, "encoded output is too large");
                return -1;
            }
            if (MS_UNLIKELY(ms_resize(self, remaining + output_len) < 0)) return -1;
            out = self->output_buffer_raw + output_len;
            out_end = self->output_buffer_raw + self->max_output_len;
        }

        /* Write the escaped character */
        char escaped[6] = {'\\', escape, '0', '0'};
        if (MS_UNLIKELY(escape == 'u')) {
            escaped[4] = hex_encode_table[c >> 4];
            escaped[5] = hex_encode_table[c & 0xF];
            memcpy(out, escaped, 6);
            out += 6;
        }
        else {
            memcpy(out, escaped, 2);
            out += 2;
        }

        goto noescape;
    }
}

static int
json_encode_cstr(EncoderState *self, const char *src, Py_ssize_t len) {
    return json_encode_cstr_inline(self, src, len);
}

static MS_NOINLINE int
json_encode_str(EncoderState *self, PyObject *obj) {
    Py_ssize_t len;
    const char* buf = unicode_str_and_size(obj, &len);
    if (buf == NULL) return -1;
    return json_encode_cstr_inline(self, buf, len);
}

static MS_INLINE int
json_encode_cstr_noescape(EncoderState *self, const char *str, Py_ssize_t size) {
    if (ms_ensure_space(self, size + 2) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    *p++ = '"';
    memcpy(p, str, size);
    *(p + size) = '"';
    self->output_len += size + 2;
    return 0;
}

static inline int
json_encode_str_noescape(EncoderState *self, PyObject *obj) {
    Py_ssize_t len;
    const char* buf = unicode_str_and_size_nocheck(obj, &len);
    return json_encode_cstr_noescape(self, buf, len);
}

static int
json_encode_bin(EncoderState *self, const char* buf, Py_ssize_t len) {
    /* Preallocate the buffer (ceil(4/3 * len) + 2) */
    Py_ssize_t encoded_len = ms_encode_base64_size(self->mod, len);
    if (encoded_len < 0) return -1;
    if (ms_ensure_space(self, encoded_len + 2) < 0) return -1;

    /* Write to the buffer directly */
    char *out = self->output_buffer_raw + self->output_len;

    *out++ = '"';
    ms_encode_base64(buf, len, out);
    out += encoded_len;
    *out++ = '"';
    self->output_len += encoded_len + 2;
    return 0;
}

static int
json_encode_bytes(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyBytes_GET_SIZE(obj);
    const char* buf = PyBytes_AS_STRING(obj);
    return json_encode_bin(self, buf, len);
}

static int
json_encode_bytearray(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = PyByteArray_GET_SIZE(obj);
    const char* buf = PyByteArray_AS_STRING(obj);
    return json_encode_bin(self, buf, len);
}

static int
json_encode_memoryview(EncoderState *self, PyObject *obj)
{
    int out;
    Py_buffer buffer;
    if (PyObject_GetBuffer(obj, &buffer, PyBUF_CONTIG_RO) < 0) return -1;
    out = json_encode_bin(self, buffer.buf, buffer.len);
    PyBuffer_Release(&buffer);
    return out;
}

static int
json_encode_raw(EncoderState *self, PyObject *obj)
{
    Raw *raw = (Raw *)obj;
    return ms_write(self, raw->buf, raw->len);
}

static int json_encode_dict_key_noinline(EncoderState *, PyObject *);

/* Extract an enum member's value. Enum members store their value as a plain
 * data attribute `_value_` in the instance `__dict__`, so read it directly to
 * skip the full `PyObject_GetAttr` descriptor machinery. Falls back to
 * attribute access for exotic enum subclasses that don't store `_value_` in
 * `__dict__`. */
static PyObject *
dump_enum_value(StructspecState *mod, PyObject *obj)
{
    PyObject *dict = PyObject_GenericGetDict(obj, NULL);
    if (dict != NULL) {
        PyObject *value = PyDict_GetItemWithError(dict, mod->str__value_);
        Py_DECREF(dict);
        if (value != NULL) {
            Py_INCREF(value);
            return value;
        }
        if (PyErr_Occurred()) return NULL;
    }
    else {
        PyErr_Clear();
    }
    return PyObject_GetAttr(obj, mod->str__value_);
}

static int
json_encode_enum(EncoderState *self, PyObject *obj, bool is_key)
{
    if (PyLong_Check(obj)) {
        return is_key ? json_encode_long_as_str(self, obj) : json_encode_long(self, obj);
    }
    if (PyUnicode_Check(obj)) {
        return json_encode_str(self, obj);
    }

    PyObject *value = dump_enum_value(self->mod, obj);
    if (value == NULL) return -1;

    int status;
    if (is_key) {
        /* A str value must be written as a JSON object key directly.
         * json_encode_dict_key_noinline only handles non-str keys (str keys
         * are fast-pathed in json_encode_dict_key), so recursing into it with
         * a str value would wrongly raise "unsupported key" - e.g. for a plain
         * Enum whose members have str values when used as a dict key. */
        status = PyUnicode_Check(value)
            ? json_encode_str(self, value)
            : json_encode_dict_key_noinline(self, value);
    }
    else {
        status = json_encode(self, value);
    }

    Py_DECREF(value);
    return status;
}

static int
json_encode_uuid(EncoderState *self, PyObject *obj)
{
    char buf[38];
    buf[0] = '"';
    bool canonical = self->uuid_format == UUID_FORMAT_CANONICAL;
    if (ms_encode_uuid(self->mod, obj, buf + 1, canonical) < 0) return -1;
    int size = canonical ? 36 : 32;
    buf[size + 1] = '"';
    return ms_write(self, buf, size + 2);
}

static int
json_encode_decimal(EncoderState *self, PyObject *obj)
{
    PyObject *temp;

    if (self->decimal_format == DECIMAL_FORMAT_CALLABLE) {
        if (self->in_decimal_callable) {
            return ms_decimal_format_error();
        }
        temp = PyObject_CallFunctionObjArgs(self->decimal_callable, obj, NULL);
        if (temp == NULL) return -1;

        self->in_decimal_callable = true;
        int out = json_encode(self, temp);
        self->in_decimal_callable = false;
        Py_DECREF(temp);
        return out;
    }

    temp = PyObject_Str(obj);
    if (temp == NULL) return -1;

    Py_ssize_t size;
    const char* buf = unicode_str_and_size_nocheck(temp, &size);
    bool decimal_as_string = (self->decimal_format == DECIMAL_FORMAT_STRING);

    Py_ssize_t required = size + (2 * decimal_as_string);
    if (ms_ensure_space(self, size + 2) < 0) {
        Py_DECREF(temp);
        return -1;
    }
    char *p = self->output_buffer_raw + self->output_len;
    if (MS_LIKELY(decimal_as_string)) *p++ = '"';
    memcpy(p, buf, size);
    if (MS_LIKELY(decimal_as_string)) *(p + size) = '"';
    self->output_len += required;
    Py_DECREF(temp);

    return 0;
}

static int
json_encode_date(EncoderState *self, PyObject *obj)
{
    if (ms_ensure_space(self, 12) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    *p = '"';
    ms_encode_date(obj, p + 1);
    *(p + 11) = '"';
    self->output_len += 12;
    return 0;
}

static int
json_encode_time(EncoderState *self, PyObject *obj)
{
    if (ms_ensure_space(self, 23) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    *p = '"';
    int size = ms_encode_time(self->mod, obj, p + 1);
    if (size < 0) return -1;
    *(p + size + 1) = '"';
    self->output_len += (size + 2);
    return 0;
}

static int
json_encode_datetime(EncoderState *self, PyObject *obj)
{
    if (ms_ensure_space(self, 34) < 0) return -1;
    char *p = self->output_buffer_raw + self->output_len;
    *p = '"';
    int size = ms_encode_datetime(self->mod, obj, p + 1);
    if (size < 0) return -1;
    *(p + size + 1) = '"';
    self->output_len += (size + 2);
    return 0;
}

static int
json_encode_timedelta(EncoderState *self, PyObject *obj)
{
    char buf[28];
    buf[0] = '"';
    int n = ms_encode_timedelta(obj, buf + 1);
    buf[1 + n] = '"';
    return ms_write(self, buf, 2 + n);
}

static MS_INLINE int
json_encode_sequence(EncoderState *self, Py_ssize_t size, PyObject **arr)
{
    int status = -1;

    if (size == 0) return ms_write(self, "[]", 2);

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    for (Py_ssize_t i = 0; i < size; i++) {
        if (json_encode_inline(self, *(arr + i)) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with ] */
    *(self->output_buffer_raw + self->output_len - 1) = ']';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static MS_NOINLINE int
json_encode_list(EncoderState *self, PyObject *obj)
{
    int ret;
    Py_BEGIN_CRITICAL_SECTION(obj);
    ret = json_encode_sequence(
        self, PyList_GET_SIZE(obj), ((PyListObject *)obj)->ob_item
    );
    Py_END_CRITICAL_SECTION();
    return ret;
}

static MS_NOINLINE int
json_encode_tuple(EncoderState *self, PyObject *obj)
{
    return json_encode_sequence(
        self, PyTuple_GET_SIZE(obj), ((PyTupleObject *)obj)->ob_item
    );
}

static int
json_encode_set(EncoderState *self, PyObject *obj)
{
    Py_ssize_t len = 0;
    PyObject *item;
    int status = -1;

    len = PySet_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "[]", 2);

    if (MS_UNLIKELY(self->sort_keys)) {
        PyObject *temp = PySequence_List(obj);
        if (temp == NULL) return -1;
        if (PyList_Sort(temp) == 0) {
            status = json_encode_list(self, temp);
        }
        Py_DECREF(temp);
        return status;
    }

    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;

    PyObject *iter = PyObject_GetIter(obj);
    if (iter == NULL) goto cleanup;

    while ((item = PyIter_Next(iter))) {
        int status = json_encode_inline(self, item);
        Py_DECREF(item);
        if (status < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    if (PyErr_Occurred()) goto cleanup;
    /* Overwrite trailing comma with ] */
    *(self->output_buffer_raw + self->output_len - 1) = ']';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    Py_XDECREF(iter);
    return status;
}

/* Look up a dump callable for a runtime type in the current codec map,
 * walking the MRO so subclasses of a registered type resolve. Returns NULL
 * when no codec applies (any raised error is cleared). */
static MS_INLINE PyObject *
codecs_lookup(PyObject *codecs, PyTypeObject *type) {
    if (codecs == NULL) return NULL;
    PyObject *out = PyDict_GetItemWithError(codecs, (PyObject *)type);
    if (out != NULL) return out;
    if (PyErr_Occurred()) {
        PyErr_Clear();
        return NULL;
    }
    PyObject *mro = type->tp_mro;
    if (mro == NULL) {
        PyTypeObject *base = type->tp_base;
        while (base != NULL) {
            out = PyDict_GetItemWithError(codecs, (PyObject *)base);
            if (out != NULL) return out;
            if (PyErr_Occurred()) {
                PyErr_Clear();
                return NULL;
            }
            base = base->tp_base;
        }
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); i++) {
        PyObject *entry = PyTuple_GET_ITEM(mro, i);
        if (entry == (PyObject *)type) continue;
        out = PyDict_GetItemWithError(codecs, entry);
        if (out != NULL) return out;
        if (PyErr_Occurred()) {
            PyErr_Clear();
            return NULL;
        }
    }
    return NULL;
}

static MS_INLINE int
json_encode_dict_key(EncoderState *self, PyObject *key) {
    if (MS_LIKELY(PyUnicode_Check(key))) {
        return json_encode_str(self, key);
    }
    return json_encode_dict_key_noinline(self, key);
}

static MS_NOINLINE int
json_encode_dict_key_noinline(EncoderState *self, PyObject *obj) {
    PyTypeObject *type = Py_TYPE(obj);

    if (type == &PyLong_Type) {
        return json_encode_long_as_str(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return json_encode_float_as_str(self, obj);
    }
    else if (PyType_IsSubtype(Py_TYPE(type), self->mod->EnumMetaType)) {
        return json_encode_enum(self, obj, true);
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        return json_encode_datetime(self, obj);
    }
    else if (type == PyDateTimeAPI->DateType) {
        return json_encode_date(self, obj);
    }
    else if (type == PyDateTimeAPI->TimeType) {
        return json_encode_time(self, obj);
    }
    else if (type == PyDateTimeAPI->DeltaType) {
        return json_encode_timedelta(self, obj);
    }
    else if (type == &PyBytes_Type) {
        return json_encode_bytes(self, obj);
    }
    else if (type == (PyTypeObject *)(self->mod->DecimalType)) {
        return json_encode_decimal(self, obj);
    }
    else if (PyType_IsSubtype(type, (PyTypeObject *)(self->mod->UUIDType))) {
        return json_encode_uuid(self, obj);
    }
    if (self->codecs != NULL) {
        PyObject *dump = codecs_lookup(self->codecs, type);
        if (dump != NULL) {
            PyObject *temp = PyObject_CallOneArg(dump, obj);
            if (temp == NULL) return -1;
            int status = json_encode_dict_key(self, temp);
            Py_DECREF(temp);
            return status;
        }
    }
    return ms_encode_err_type_unsupported(type);
}

static int
json_encode_and_free_assoclist(EncoderState *self, AssocList *list, bool escape) {
    if (list == NULL) return -1;

    int status = -1;

    AssocList_Sort(list);

    if (Py_EnterRecursiveCall(" while serializing an object")) goto cleanup2;

    if (ms_write(self, "{", 1) < 0) goto cleanup;
    Py_ssize_t start_len = self->output_len;
    if (escape) {
        for (Py_ssize_t i = 0; i < list->size; i++) {
            AssocItem *item = &(list->items[i]);
            if (json_encode_cstr(self, item->key, item->key_size) < 0) goto cleanup;
            if (ms_write(self, ":", 1) < 0) goto cleanup;
            if (json_encode_inline(self, item->val) < 0) goto cleanup;
            if (ms_write(self, ",", 1) < 0) goto cleanup;
        }
    }
    else {
        for (Py_ssize_t i = 0; i < list->size; i++) {
            AssocItem *item = &(list->items[i]);
            if (json_encode_cstr_noescape(self, item->key, item->key_size) < 0) goto cleanup;
            if (ms_write(self, ":", 1) < 0) goto cleanup;
            if (json_encode_inline(self, item->val) < 0) goto cleanup;
            if (ms_write(self, ",", 1) < 0) goto cleanup;
        }
    }
    if (MS_UNLIKELY(start_len == self->output_len)) {
        /* Empty, append "}" */
        if (ms_write(self, "}", 1) < 0) goto cleanup;
    }
    else {
        /* Overwrite trailing comma with } */
        *(self->output_buffer_raw + self->output_len - 1) = '}';
    }
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
cleanup2:
    AssocList_Free(list);
    return status;
}

static MS_NOINLINE int
json_encode_dict(EncoderState *self, PyObject *obj)
{
    PyObject *key, *val;
    Py_ssize_t len, pos = 0;
    int status = -1;

    len = PyDict_GET_SIZE(obj);
    if (len == 0) return ms_write(self, "{}", 2);

    if (MS_UNLIKELY(self->sort_keys)) {
        return json_encode_and_free_assoclist(self, AssocList_FromDict(obj), true);
    }

    if (ms_write(self, "{", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    Py_BEGIN_CRITICAL_SECTION(obj);
    while (PyDict_Next(obj, &pos, &key, &val)) {
        if (json_encode_dict_key(self, key) < 0) goto cleanup;
        if (ms_write(self, ":", 1) < 0) goto cleanup;
        if (json_encode_inline(self, val) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    /* Overwrite trailing comma with } */
    *(self->output_buffer_raw + self->output_len - 1) = '}';
    status = 0;
cleanup:;
    Py_END_CRITICAL_SECTION();
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_dataclass(EncoderState *self, PyObject *obj, PyObject *fields)
{
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;

    int status = -1;
    DataclassIter iter;
    if (!dataclass_iter_setup(&iter, obj, fields)) goto cleanup;

    if (ms_write(self, "{", 1) < 0) goto cleanup;
    Py_ssize_t start_offset = self->output_len;

    PyObject *field, *val;
    while (dataclass_iter_next(&iter, &field, &val)) {
        Py_ssize_t field_len;
        const char* field_buf = unicode_str_and_size(field, &field_len);
        bool errored = (
            (field_buf == NULL) ||
            (json_encode_cstr_noescape(self, field_buf, field_len) < 0) ||
            (ms_write(self, ":", 1) < 0) ||
            (json_encode(self, val) < 0) ||
            (ms_write(self, ",", 1) < 0)
        );
        Py_DECREF(val);
        if (errored) goto cleanup;
    }

    /* If any fields written, overwrite trailing comma with }, otherwise append } */
    if (MS_LIKELY(self->output_len != start_offset)) {
        *(self->output_buffer_raw + self->output_len - 1) = '}';
        status = 0;
    }
    else {
        status = ms_write(self, "}", 1);
    }

cleanup:
    Py_LeaveRecursiveCall();
    dataclass_iter_cleanup(&iter);
    return status;
}

/* This method encodes an object as a map, with fields taken from `__dict__`,
 * followed by all `__slots__` in the class hierarchy. Any unset slots are
 * ignored, and `__weakref__` is not included. */
static int
json_encode_object(EncoderState *self, PyObject *obj)
{
    int status = -1;
    if (ms_write(self, "{", 1) < 0) return -1;
    Py_ssize_t start_offset = self->output_len;

    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    /* First encode everything in `__dict__` */
    PyObject *dict = PyObject_GenericGetDict(obj, NULL);
    Py_BEGIN_CRITICAL_SECTION(obj);
    if (MS_UNLIKELY(dict == NULL)) {
        PyErr_Clear();
    }
    else {
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        int err = 0;
        Py_BEGIN_CRITICAL_SECTION(dict);
        while (PyDict_Next(dict, &pos, &key, &val)) {
            if (MS_LIKELY(PyUnicode_CheckExact(key))) {
                Py_ssize_t key_len;
                const char* key_buf = unicode_str_and_size(key, &key_len);
                if (MS_UNLIKELY(val == UNSET)) continue;
                if (MS_UNLIKELY(key_buf == NULL)) {
                    err = 1;
                    break;
                }
                if (MS_UNLIKELY(*key_buf == '_')) continue;
                if (MS_UNLIKELY(json_encode_cstr_noescape(self, key_buf, key_len) < 0)) {
                    err = 1;
                    break;
                }
                if (MS_UNLIKELY(ms_write(self, ":", 1) < 0)) {
                    err = 1;
                    break;
                }
                if (MS_UNLIKELY(json_encode(self, val) < 0)) {
                    err = 1;
                    break;
                }
                if (MS_UNLIKELY(ms_write(self, ",", 1) < 0)) {
                    err = 1;
                    break;
                }
            }
        }
        Py_END_CRITICAL_SECTION();
        if (MS_UNLIKELY(err)) goto cleanup;
    }
    /* Then encode everything in slots */
    PyTypeObject *type = Py_TYPE(obj);
    while (type != NULL) {
        Py_ssize_t n = Py_SIZE(type);
        if (n) {
            PyMemberDef *mp = MS_PyHeapType_GET_MEMBERS((PyHeapTypeObject *)type);
            for (Py_ssize_t i = 0; i < n; i++, mp++) {
                if (MS_LIKELY(mp->type == T_OBJECT_EX && !(mp->flags & READONLY))) {
                    char *addr = (char *)obj + mp->offset;
                    PyObject *val = *(PyObject **)addr;
                    if (MS_UNLIKELY(val == NULL)) continue;
                    if (MS_UNLIKELY(val == UNSET)) continue;
                    if (MS_UNLIKELY(*mp->name == '_')) continue;
                    if (MS_UNLIKELY(json_encode_cstr_noescape(self, mp->name, strlen(mp->name)) < 0)) goto cleanup;
                    if (MS_UNLIKELY(ms_write(self, ":", 1) < 0)) goto cleanup;
                    if (MS_UNLIKELY(json_encode(self, val) < 0)) goto cleanup;
                    if (MS_UNLIKELY(ms_write(self, ",", 1) < 0)) goto cleanup;
                }
            }
        }
        type = type->tp_base;
    }
    /* If any fields written, overwrite trailing comma with }, otherwise append } */
    if (MS_LIKELY(self->output_len != start_offset)) {
        *(self->output_buffer_raw + self->output_len - 1) = '}';
        status = 0;
    }
    else {
        status = ms_write(self, "}", 1);
    }
cleanup:;
    Py_END_CRITICAL_SECTION();
    Py_XDECREF(dict);
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_struct_tag(EncoderState *self, PyObject *obj)
{
    PyTypeObject *type = Py_TYPE(obj);

    if (type == &PyUnicode_Type) {
        return json_encode_str(self, obj);
    }
    else {
        return json_encode_long(self, obj);
    }
}

static int
json_encode_struct_object(
    EncoderState *self, StructMetaObject *struct_type, PyObject *obj
) {
    PyObject *val, *fields, *defaults, *tag_value;
    Py_ssize_t i, nfields, nunchecked;
    int status = -1;
    tag_value = struct_type->struct_tag_value;
    fields = struct_type->struct_encode_fields;
    defaults = struct_type->struct_defaults;
    nfields = PyTuple_GET_SIZE(fields);
    nunchecked = nfields;
    if (struct_type->omit_defaults == OPT_TRUE) {
        nunchecked -= PyTuple_GET_SIZE(defaults);
    }
    PyObject *field_codecs = struct_type->struct_field_codecs;
    PyObject *saved_codecs = self->codecs;
    bool has_codecs = (field_codecs != NULL);

    if (ms_write(self, "{", 1) < 0) return -1;
    Py_ssize_t start_len = self->output_len;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    if (tag_value != NULL) {
        MS_StrView tv = struct_type->struct_tag_field_view;
        if (json_encode_cstr_inline(self, tv.buf, tv.size) < 0) goto cleanup;
        if (ms_write(self, ":", 1) < 0) goto cleanup;
        if (json_encode_struct_tag(self, tag_value) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }

    for (i = 0; i < nunchecked; i++) {
        val = Struct_get_index(obj, i);
        if (MS_UNLIKELY(val == NULL)) goto cleanup;
        if (MS_UNLIKELY(val == UNSET)) continue;
        MS_StrView kv = struct_type->struct_encode_keys[i];
        if (json_encode_cstr_noescape(self, kv.buf, kv.size) < 0) goto cleanup;
        if (ms_write(self, ":", 1) < 0) goto cleanup;
        if (has_codecs) self->codecs = PyTuple_GET_ITEM(field_codecs, i);
        if (json_encode(self, val) < 0) goto cleanup;
        if (has_codecs) self->codecs = saved_codecs;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    for (i = nunchecked; i < nfields; i++) {
        val = Struct_get_index(obj, i);
        if (MS_UNLIKELY(val == NULL)) goto cleanup;
        if (MS_UNLIKELY(val == UNSET)) continue;
        PyObject *default_val = PyTuple_GET_ITEM(defaults, i - nunchecked);
        if (!is_default(val, default_val)) {
            MS_StrView kv = struct_type->struct_encode_keys[i];
            if (json_encode_cstr_noescape(self, kv.buf, kv.size) < 0) goto cleanup;
            if (ms_write(self, ":", 1) < 0) goto cleanup;
            if (has_codecs) self->codecs = PyTuple_GET_ITEM(field_codecs, i);
            if (json_encode(self, val) < 0) goto cleanup;
            if (has_codecs) self->codecs = saved_codecs;
            if (ms_write(self, ",", 1) < 0) goto cleanup;
        }
    }
    self->codecs = saved_codecs;
    if (MS_UNLIKELY(start_len == self->output_len)) {
        /* Empty struct, append "}" */
        if (ms_write(self, "}", 1) < 0) goto cleanup;
    }
    else {
        /* Overwrite trailing comma with } */
        *(self->output_buffer_raw + self->output_len - 1) = '}';
    }
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_struct_array(
    EncoderState *self, StructMetaObject *struct_type, PyObject *obj
) {
    int status = -1;
    PyObject *tag_value = struct_type->struct_tag_value;
    PyObject *fields = struct_type->struct_encode_fields;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    PyObject *field_codecs = struct_type->struct_field_codecs;
    PyObject *saved_codecs = self->codecs;
    bool has_codecs = (field_codecs != NULL);

    if (nfields == 0 && tag_value == NULL) return ms_write(self, "[]", 2);
    if (ms_write(self, "[", 1) < 0) return -1;
    if (Py_EnterRecursiveCall(" while serializing an object")) return -1;
    if (tag_value != NULL) {
        if (json_encode_struct_tag(self, tag_value) < 0) goto cleanup;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *val = Struct_get_index(obj, i);
        if (val == NULL) goto cleanup;
        if (has_codecs) self->codecs = PyTuple_GET_ITEM(field_codecs, i);
        if (json_encode(self, val) < 0) goto cleanup;
        if (has_codecs) self->codecs = saved_codecs;
        if (ms_write(self, ",", 1) < 0) goto cleanup;
    }
    self->codecs = saved_codecs;
    /* Overwrite trailing comma with ] */
    *(self->output_buffer_raw + self->output_len - 1) = ']';
    status = 0;
cleanup:
    Py_LeaveRecursiveCall();
    return status;
}

static int
json_encode_struct(EncoderState *self, PyObject *obj)
{
    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(obj);

    if (struct_type->array_like == OPT_TRUE) {
        return json_encode_struct_array(self, struct_type, obj);
    }
    return json_encode_struct_object(self, struct_type, obj);
}

static MS_NOINLINE int
json_encode_uncommon(EncoderState *self, PyTypeObject *type, PyObject *obj) {
    if (obj == Py_None) {
        return ms_write(self, "null", 4);
    }
    else if (obj == Py_True) {
        return ms_write(self, "true", 4);
    }
    else if (obj == Py_False) {
        return ms_write(self, "false", 5);
    }
    else if (PyTuple_Check(obj)) {
        return json_encode_tuple(self, obj);
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        return json_encode_datetime(self, obj);
    }
    else if (type == PyDateTimeAPI->DateType) {
        return json_encode_date(self, obj);
    }
    else if (type == PyDateTimeAPI->TimeType) {
        return json_encode_time(self, obj);
    }
    else if (type == PyDateTimeAPI->DeltaType) {
        return json_encode_timedelta(self, obj);
    }
    else if (type == &PyBytes_Type) {
        return json_encode_bytes(self, obj);
    }
    else if (type == &PyByteArray_Type) {
        return json_encode_bytearray(self, obj);
    }
    else if (type == &PyMemoryView_Type) {
        return json_encode_memoryview(self, obj);
    }
    else if (type == &Raw_Type) {
        return json_encode_raw(self, obj);
    }
    else if (PyType_IsSubtype(Py_TYPE(type), self->mod->EnumMetaType)) {
        return json_encode_enum(self, obj, false);
    }
    else if (PyType_IsSubtype(type, (PyTypeObject *)(self->mod->UUIDType))) {
        return json_encode_uuid(self, obj);
    }
    else if (type == (PyTypeObject *)(self->mod->DecimalType)) {
        return json_encode_decimal(self, obj);
    }
    else if (PyAnySet_Check(obj)) {
        return json_encode_set(self, obj);
    }
    if (self->codecs != NULL) {
        PyObject *dump = codecs_lookup(self->codecs, type);
        if (dump != NULL) {
            PyObject *temp = PyObject_CallOneArg(dump, obj);
            if (temp == NULL) return -1;
            int status = json_encode_inline(self, temp);
            Py_DECREF(temp);
            return status;
        }
    }
    if (PyObject_HasAttrString(obj, "struct_dump")) {
        /* Custom type — struct_dump() to a base type, then re-encode */
        PyObject *dumped = PyObject_CallMethod(obj, "struct_dump", NULL);
        if (dumped == NULL) return -1;
        int status = json_encode_inline(self, dumped);
        Py_DECREF(dumped);
        return status;
    }
    else if (PyObject_HasAttrString(obj, "model_dump")) {
        /* Pydantic BaseModel — model_dump() to dict, then encode */
        PyObject *dict = PyObject_CallMethod(obj, "model_dump", NULL);
        if (dict == NULL) return -1;
        int status = json_encode_dict(self, dict);
        Py_DECREF(dict);
        return status;
    }
    else if (!ms_is_struct_inst(obj) && PyObject_HasAttrString(obj, "__struct_fields__")) {
        /* External struct type (e.g. msgspec) — iterate fields and encode as JSON object */
        PyObject *fields = PyObject_GetAttrString(obj, "__struct_fields__");
        if (fields == NULL) return -1;
        if (!PyTuple_CheckExact(fields)) { Py_DECREF(fields); return -1; }

        Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
        if (ms_write(self, "{", 1) < 0) { Py_DECREF(fields); return -1; }
        int status = -1;
        Py_ssize_t start_offset = self->output_len;
        if (Py_EnterRecursiveCall(" while serializing an object")) { Py_DECREF(fields); return -1; }

        for (Py_ssize_t i = 0; i < nfields; i++) {
            PyObject *key = PyTuple_GET_ITEM(fields, i);
            PyObject *val = PyObject_GetAttr(obj, key);
            if (val == NULL) { PyErr_Clear(); continue; }
            if (json_encode_str_noescape(self, key) < 0 || ms_write(self, ":", 1) < 0) {
                Py_DECREF(val); goto cleanup;
            }
            if (json_encode(self, val) < 0) { Py_DECREF(val); goto cleanup; }
            if (ms_write(self, ",", 1) < 0) { Py_DECREF(val); goto cleanup; }
            Py_DECREF(val);
        }

        if (start_offset == self->output_len) {
            status = ms_write(self, "}", 1);
        }
        else {
            *(self->output_buffer_raw + self->output_len - 1) = '}';
            status = 0;
        }
cleanup:
        Py_LeaveRecursiveCall();
        Py_DECREF(fields);
        return status;
    }
    else if (!PyType_Check(obj) && type->tp_dict != NULL) {
        PyObject *fields = PyObject_GetAttr(obj, self->mod->str___dataclass_fields__);
        if (fields != NULL) {
            int status = json_encode_dataclass(self, obj, fields);
            Py_DECREF(fields);
            return status;
        }
        else {
            PyErr_Clear();
        }
        if (PyDict_Contains(type->tp_dict, self->mod->str___attrs_attrs__)) {
            return json_encode_object(self, obj);
        }
    }

    return ms_encode_err_type_unsupported(type);
}

static MS_INLINE int
json_encode_inline(EncoderState *self, PyObject *obj)
{
    PyTypeObject *type = Py_TYPE(obj);

    if (PyUnicode_Check(obj)) {
        return json_encode_str(self, obj);
    }
    else if (type == &PyLong_Type) {
        return json_encode_long(self, obj);
    }
    else if (type == &PyFloat_Type) {
        return json_encode_float(self, obj);
    }
    else if (PyList_Check(obj)) {
        return json_encode_list(self, obj);
    }
    else if (PyDict_Check(obj)) {
        return json_encode_dict(self, obj);
    }
#if PY315_PLUS
    else if (PyFrozenDict_Check(obj)) {
        return json_encode_dict(self, obj);
    }
#endif
    else if (ms_is_struct_type(type)) {
        /* Structs are common in the payload; check before the uncommon
         * dispatch so they avoid the MS_NOINLINE call and its chain of
         * type checks. */
        return json_encode_struct(self, obj);
    }
    else {
        return json_encode_uncommon(self, type, obj);
    }
}

static int
json_encode(EncoderState *self, PyObject *obj)
{
    return json_encode_inline(self, obj);
}

static PyObject*
JSONEncoder_encode(Encoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    return encoder_encode_common(self, args, nargs, &json_encode);
}


static struct PyMethodDef JSONEncoder_methods[] = {
    {
        "encode", (PyCFunction) JSONEncoder_encode, METH_FASTCALL,
        Encoder_encode__doc__,
    },
    {NULL, NULL}                /* sentinel */
};

static PyTypeObject JSONEncoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype.json.Encoder",
    .tp_doc = JSONEncoder__doc__,
    .tp_basicsize = sizeof(Encoder),
    .tp_dealloc = (destructor)Encoder_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)Encoder_traverse,
    .tp_clear = (inquiry)Encoder_clear,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Encoder_init,
    .tp_methods = JSONEncoder_methods,
    .tp_members = Encoder_members,
    .tp_getset = Encoder_getset,
};

static PyObject*
structtype_json_encode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    return encode_common(self, args, nargs, kwnames, &json_encode);
}

/*************************************************************************
 * JSON Decoder                                                          *
 *************************************************************************/

typedef struct JSONDecoderState {
    /* Configuration */
    TypeNode *type;
    PyObject *float_hook;
    bool strict;

    /* Temporary scratch space */
    unsigned char *scratch;
    Py_ssize_t scratch_capacity;
    Py_ssize_t scratch_len;

    /* Per-message attributes */
    PyObject *buffer_obj;
    unsigned char *input_start;
    unsigned char *input_pos;
    unsigned char *input_end;
} JSONDecoderState;

typedef struct JSONDecoder {
    PyObject_HEAD
    PyObject *orig_type;

    /* Configuration */
    TypeNode *type;
    char strict;
    PyObject *float_hook;
} JSONDecoder;

PyDoc_STRVAR(JSONDecoder__doc__,
"Decoder(type='Any', *, strict=True, float_hook=None)\n"
"--\n"
"\n"
"A JSON decoder.\n"
"\n"
"Parameters\n"
"----------\n"
"type : type, optional\n"
"    A Python type (in type annotation form) to decode the object as. If\n"
"    provided, the message will be type checked and decoded as the specified\n"
"    type. Defaults to `Any`, in which case the message will be decoded using\n"
"    the default JSON types.\n"
"strict : bool, optional\n"
"    Whether type coercion rules should be strict. Setting to False enables a\n"
"    wider set of coercion rules from string to non-string types for all values.\n"
"    Default is True.\n"
"float_hook : callable, optional\n"
"    An optional callback for handling decoding untyped float literals. Should\n"
"    have the signature ``float_hook(val: str) -> Any``, where ``val`` is the\n"
"    raw string value of the JSON float. This hook is called to decode any\n"
"    \"untyped\" float value (e.g. ``typing.Any`` typed). The default is\n"
"    equivalent to ``float_hook=float``, where all untyped JSON floats are\n"
"    decoded as python floats. Specifying ``float_hook=decimal.Decimal``\n"
"    will decode all untyped JSON floats as decimals instead."
);
static int
JSONDecoder_init(JSONDecoder *self, PyObject *args, PyObject *kwds)
{
    char *kwlist[] = {"type", "strict", "float_hook", NULL};
    StructspecState *st = structtype_get_global_state();
    PyObject *type = st->typing_any;
    PyObject *float_hook = NULL;
    int strict = 1;

    if (!PyArg_ParseTupleAndKeywords(
        args, kwds, "|O$pO", kwlist, &type, &strict, &float_hook)
    ) {
        return -1;
    }

    /* Handle float_hook */
    if (float_hook == Py_None) {
        float_hook = NULL;
    }
    if (float_hook != NULL) {
        if (!PyCallable_Check(float_hook)) {
            PyErr_SetString(PyExc_TypeError, "float_hook must be callable");
            return -1;
        }
        Py_INCREF(float_hook);
    }
    Py_XSETREF(self->float_hook, float_hook);

    /* Handle strict */
    self->strict = strict;

    /* Handle type */
    self->type = TypeNode_Convert(type);
    if (self->type == NULL) return -1;
    Py_INCREF(type);
    self->orig_type = type;

    return 0;
}

static int
JSONDecoder_traverse(JSONDecoder *self, visitproc visit, void *arg)
{
    int out = TypeNode_traverse(self->type, visit, arg);
    if (out != 0) return out;
    Py_VISIT(self->orig_type);
    Py_VISIT(self->float_hook);
    return 0;
}

static int
JSONDecoder_clear(JSONDecoder *self)
{
    Py_CLEAR(self->orig_type);
    TypeNode_Free(self->type);
    self->type = NULL;
    Py_CLEAR(self->float_hook);
    return 0;
}

static void
JSONDecoder_dealloc(JSONDecoder *self)
{
    PyObject_GC_UnTrack(self);
    JSONDecoder_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
JSONDecoder_repr(JSONDecoder *self) {
    int recursive;
    PyObject *typstr, *out = NULL;

    recursive = Py_ReprEnter((PyObject *)self);
    if (recursive != 0) {
        return (recursive < 0) ? NULL : PyUnicode_FromString("...");  /* cpylint-ignore */
    }
    typstr = PyObject_Repr(self->orig_type);
    if (typstr != NULL) {
        out = PyUnicode_FromFormat("structtype.json.Decoder(%U)", typstr);
    }
    Py_XDECREF(typstr);
    Py_ReprLeave((PyObject *)self);
    return out;
}

static MS_INLINE bool
json_read1(JSONDecoderState *self, unsigned char *c)
{
    if (MS_UNLIKELY(self->input_pos == self->input_end)) {
        ms_err_truncated();
        return false;
    }
    *c = *self->input_pos;
    self->input_pos += 1;
    return true;
}

static MS_INLINE char
json_peek_or_null(JSONDecoderState *self) {
    if (MS_UNLIKELY(self->input_pos == self->input_end)) return '\0';
    return *self->input_pos;
}

static MS_INLINE bool
json_peek_skip_ws(JSONDecoderState *self, unsigned char *s)
{
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) {
            ms_err_truncated();
            return false;
        }
        unsigned char c = *self->input_pos;
        if (MS_LIKELY(c != ' ' && c != '\n' && c != '\r' && c != '\t')) {
            *s = c;
            return true;
        }
        self->input_pos++;
    }
}

static MS_INLINE bool
json_remaining(JSONDecoderState *self, ptrdiff_t remaining)
{
    return self->input_end - self->input_pos >= remaining;
}

static PyObject *
json_err_invalid(JSONDecoderState *self, const char *msg)
{
    PyErr_Format(
        structtype_get_global_state()->DecodeError,
        "JSON is malformed: %s (byte %zd)",
        msg,
        (Py_ssize_t)(self->input_pos - self->input_start)
    );
    return NULL;
}

static MS_INLINE bool
json_has_trailing_characters(JSONDecoderState *self)
{
    while (self->input_pos != self->input_end) {
        unsigned char c = *self->input_pos++;
        if (MS_UNLIKELY(!(c == ' ' || c == '\n' || c == '\t' || c == '\r'))) {
            json_err_invalid(self, "trailing characters");
            return true;
        }
    }
    return false;
}

static int json_skip(JSONDecoderState *self);

static PyObject * json_decode(
    JSONDecoderState *self, TypeNode *type, PathNode *path
);

static PyObject *
json_decode_none(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    self->input_pos++;  /* Already checked as 'n' */
    if (MS_UNLIKELY(!json_remaining(self, 3))) {
        ms_err_truncated();
        return NULL;
    }
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'u' || c2 != 'l' || c3 != 'l')) {
        return json_err_invalid(self, "invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_NONE)) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    return ms_validation_error("null", type, path);
}

static PyObject *
json_decode_true(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    self->input_pos++;  /* Already checked as 't' */
    if (MS_UNLIKELY(!json_remaining(self, 3))) {
        ms_err_truncated();
        return NULL;
    }
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'r' || c2 != 'u' || c3 != 'e')) {
        return json_err_invalid(self, "invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL | MS_TYPE_BOOLLITERAL_TRUE)) {
        Py_INCREF(Py_True);
        return Py_True;
    }
    if (type->types & MS_TYPE_BOOLLITERAL_FALSE) {
        ms_raise_validation_error(path, "Invalid enum value %R%U", Py_True);
        return NULL;
    }
    return ms_validation_error("bool", type, path);
}

static PyObject *
json_decode_false(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    self->input_pos++;  /* Already checked as 'f' */
    if (MS_UNLIKELY(!json_remaining(self, 4))) {
        ms_err_truncated();
        return NULL;
    }
    unsigned char c1 = *self->input_pos++;
    unsigned char c2 = *self->input_pos++;
    unsigned char c3 = *self->input_pos++;
    unsigned char c4 = *self->input_pos++;
    if (MS_UNLIKELY(c1 != 'a' || c2 != 'l' || c3 != 's' || c4 != 'e')) {
        return json_err_invalid(self, "invalid character");
    }
    if (type->types & (MS_TYPE_ANY | MS_TYPE_BOOL | MS_TYPE_BOOLLITERAL_FALSE)) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    if (type->types & MS_TYPE_BOOLLITERAL_TRUE) {
        ms_raise_validation_error(path, "Invalid enum value %R%U", Py_False);
        return NULL;
    }
    return ms_validation_error("bool", type, path);
}

#define JS_SCRATCH_MAX_SIZE 1024

static int
json_scratch_resize(JSONDecoderState *state, Py_ssize_t size) {
    unsigned char *temp = PyMem_Realloc(state->scratch, size);
    if (MS_UNLIKELY(temp == NULL)) {
        PyErr_NoMemory();
        return -1;
    }
    state->scratch = temp;
    state->scratch_capacity = size;
    return 0;
}

static MS_NOINLINE int
json_scratch_expand(JSONDecoderState *state, Py_ssize_t required) {
    size_t new_size = (size_t)Py_MAX(8, 1.5 * required);
    return json_scratch_resize(state, new_size);
}

static int
json_scratch_extend(JSONDecoderState *state, const void *buf, Py_ssize_t size) {
    Py_ssize_t required = state->scratch_len + size;
    if (MS_UNLIKELY(required >= state->scratch_capacity)) {
        if (MS_UNLIKELY(json_scratch_expand(state, required) < 0)) return -1;
    }
    memcpy(state->scratch + state->scratch_len, buf, size);
    state->scratch_len += size;
    return 0;
}

/* -1: '\', '"', and forbidden characters
 * 0: ascii
 * 1: non-ascii */
static const int8_t char_types[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, -1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
};

/* Is char `"`, `\`, or nonascii? */
static MS_INLINE bool char_is_special_or_nonascii(unsigned char c) {
    return char_types[c] != 0;
}

/* Is char `"` or `\`? */
static MS_INLINE bool char_is_special(unsigned char c) {
    return char_types[c] < 0;
}

static int
json_read_codepoint(JSONDecoderState *self, unsigned int *out) {
    unsigned char c;
    unsigned int cp = 0;
    if (!json_remaining(self, 4)) return ms_err_truncated();
    for (int i = 0; i < 4; i++) {
        c = *self->input_pos++;
        if (c >= '0' && c <= '9') {
            c -= '0';
        }
        else if (c >= 'a' && c <= 'f') {
            c = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F') {
            c = c - 'A' + 10;
        }
        else {
            json_err_invalid(self, "invalid character in unicode escape");
            return -1;
        }
        cp = (cp << 4) + c;
    }
    *out = cp;
    return 0;
}

static MS_NOINLINE int
json_handle_unicode_escape(JSONDecoderState *self) {
    unsigned int cp;
    if (json_read_codepoint(self, &cp) < 0) return -1;

    if (0xDC00 <= cp && cp <= 0xDFFF) {
        json_err_invalid(self, "invalid utf-16 surrogate pair");
        return -1;
    }
    else if (0xD800 <= cp && cp <= 0xDBFF) {
        /* utf-16 pair, parse 2nd pair */
        unsigned int cp2;
        if (!json_remaining(self, 6)) return ms_err_truncated();
        if (self->input_pos[0] != '\\' || self->input_pos[1] != 'u') {
            json_err_invalid(self, "unexpected end of escaped utf-16 surrogate pair");
            return -1;
        }
        self->input_pos += 2;
        if (json_read_codepoint(self, &cp2) < 0) return -1;
        if (cp2 < 0xDC00 || cp2 > 0xDFFF) {
            json_err_invalid(self, "invalid utf-16 surrogate pair");
            return -1;
        }
        cp = 0x10000 + (((cp - 0xD800) << 10) | (cp2 - 0xDC00));
    }

    /* Encode the codepoint as utf-8 */
    unsigned char *p = self->scratch + self->scratch_len;
    if (cp < 0x80) {
        *p++ = cp;
        self->scratch_len += 1;
    } else if (cp < 0x800) {
        *p++ = 0xC0 | (cp >> 6);
        *p++ = 0x80 | (cp & 0x3F);
        self->scratch_len += 2;
    } else if (cp < 0x10000) {
        *p++ = 0xE0 | (cp >> 12);
        *p++ = 0x80 | ((cp >> 6) & 0x3F);
        *p++ = 0x80 | (cp & 0x3F);
        self->scratch_len += 3;
    } else {
        *p++ = 0xF0 | (cp >> 18);
        *p++ = 0x80 | ((cp >> 12) & 0x3F);
        *p++ = 0x80 | ((cp >> 6) & 0x3F);
        *p++ = 0x80 | (cp & 0x3F);
        self->scratch_len += 4;
    }
    return 0;
}

#define parse_ascii_pre(i) \
    if (MS_UNLIKELY(char_is_special_or_nonascii(self->input_pos[i]))) goto parse_ascii_##i;

#define parse_ascii_post(i) \
    parse_ascii_##i: \
    self->input_pos += i; \
    goto parse_ascii_end;

#define parse_unicode_pre(i) \
    if (MS_UNLIKELY(char_is_special(self->input_pos[i]))) goto parse_unicode_##i;

#define parse_unicode_post(i) \
    parse_unicode_##i: \
    self->input_pos += i; \
    goto parse_unicode_end;

static MS_NOINLINE Py_ssize_t
json_decode_string_view_copy(
    JSONDecoderState *self, char **out, bool *is_ascii, unsigned char *start
) {
    unsigned char c;
    self->scratch_len = 0;

top:
    OPT_FORCE_RELOAD(*self->input_pos);

    c = *self->input_pos;
    if (c == '\\') {
        /* Write the current block to scratch */
        Py_ssize_t block_size = self->input_pos - start;
        /* An escape string requires at most 4 bytes to decode */
        Py_ssize_t required = self->scratch_len + block_size + 4;
        if (MS_UNLIKELY(required >= self->scratch_capacity)) {
            if (MS_UNLIKELY(json_scratch_expand(self, required) < 0)) return -1;
        }
        memcpy(self->scratch + self->scratch_len, start, block_size);
        self->scratch_len += block_size;

        self->input_pos++;
        if (!json_read1(self, &c)) return -1;

        switch (c) {
            case 'n': {
                *(self->scratch + self->scratch_len) = '\n';
                self->scratch_len++;
                break;
            }
            case '"': {
                *(self->scratch + self->scratch_len) = '"';
                self->scratch_len++;
                break;
            }
            case 't': {
                *(self->scratch + self->scratch_len) = '\t';
                self->scratch_len++;
                break;
            }
            case 'r': {
                *(self->scratch + self->scratch_len) = '\r';
                self->scratch_len++;
                break;
            }
            case '\\': {
                *(self->scratch + self->scratch_len) = '\\';
                self->scratch_len++;
                break;
            }
            case '/': {
                *(self->scratch + self->scratch_len) = '/';
                self->scratch_len++;
                break;
            }
            case 'b': {
                *(self->scratch + self->scratch_len) = '\b';
                self->scratch_len++;
                break;
            }
            case 'f': {
                *(self->scratch + self->scratch_len) = '\f';
                self->scratch_len++;
                break;
            }
            case 'u': {
                *is_ascii = false;
                if (json_handle_unicode_escape(self) < 0) return -1;
                break;
            }
            default:
                json_err_invalid(self, "invalid escape character in string");
                return -1;
        }

        start = self->input_pos;
    }
    else if (c == '"') {
        if (json_scratch_extend(self, start, self->input_pos - start) < 0) return -1;
        self->input_pos++;
        *out = (char *)(self->scratch);
        return self->scratch_len;
    }
    else {
        json_err_invalid(self, "invalid character");
        return -1;
    }

    /* Loop until `"`, `\`, or a non-ascii character */
    while (self->input_end - self->input_pos >= 8) {
        repeat8(parse_ascii_pre);
        self->input_pos += 8;
        continue;
        repeat8(parse_ascii_post);
    }
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
        if (MS_UNLIKELY(char_is_special_or_nonascii(*self->input_pos))) break;
        self->input_pos++;
    }

parse_ascii_end:
    OPT_FORCE_RELOAD(*self->input_pos);

    if (MS_UNLIKELY(*self->input_pos & 0x80)) {
        *is_ascii = false;
        /* Loop until `"` or `\` */
        while (self->input_end - self->input_pos >= 8) {
            repeat8(parse_unicode_pre);
            self->input_pos += 8;
            continue;
            repeat8(parse_unicode_post);
        }
        while (true) {
            if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
            if (MS_UNLIKELY(char_is_special(*self->input_pos))) break;
            self->input_pos++;
        }
    }
parse_unicode_end:
    goto top;
}

static Py_ssize_t
json_decode_string_view(JSONDecoderState *self, char **out, bool *is_ascii) {
    self->input_pos++; /* Skip '"' */
    unsigned char *start = self->input_pos;

    /* Loop until `"`, `\`, or a non-ascii character */
    while (self->input_end - self->input_pos >= 8) {
        repeat8(parse_ascii_pre);
        self->input_pos += 8;
        continue;
        repeat8(parse_ascii_post);
    }
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
        if (MS_UNLIKELY(char_is_special_or_nonascii(*self->input_pos))) break;
        self->input_pos++;
    }

parse_ascii_end:
    OPT_FORCE_RELOAD(*self->input_pos);

    if (MS_LIKELY(*self->input_pos == '"')) {
        Py_ssize_t size = self->input_pos - start;
        self->input_pos++;
        *out = (char *)start;
        return size;
    }

    if (MS_UNLIKELY(*self->input_pos & 0x80)) {
        *is_ascii = false;
        /* Loop until `"` or `\` */
        while (self->input_end - self->input_pos >= 8) {
            repeat8(parse_unicode_pre);
            self->input_pos += 8;
            continue;
            repeat8(parse_unicode_post);
        }
        while (true) {
            if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
            if (MS_UNLIKELY(char_is_special(*self->input_pos))) break;
            self->input_pos++;
        }
    }

parse_unicode_end:
    OPT_FORCE_RELOAD(*self->input_pos);

    if (MS_LIKELY(*self->input_pos == '"')) {
        Py_ssize_t size = self->input_pos - start;
        self->input_pos++;
        *out = (char *)start;
        return size;
    }

    return json_decode_string_view_copy(self, out, is_ascii, start);
}

static int
json_skip_string(JSONDecoderState *self) {
    self->input_pos++; /* Skip '"' */

parse_unicode:
    /* Loop until `"` or `\` */
    while (self->input_end - self->input_pos >= 8) {
        repeat8(parse_unicode_pre);
        self->input_pos += 8;
        continue;
        repeat8(parse_unicode_post);
    }
    while (true) {
        if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();
        if (MS_UNLIKELY(char_is_special(*self->input_pos))) break;
        self->input_pos++;
    }

parse_unicode_end:
    OPT_FORCE_RELOAD(*self->input_pos);

    if (MS_LIKELY(*self->input_pos == '"')) {
        self->input_pos++;
        return 0;
    }
    else if (*self->input_pos == '\\') {
        self->input_pos++;
        if (MS_UNLIKELY(self->input_pos == self->input_end)) return ms_err_truncated();

        switch (*self->input_pos) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                self->input_pos++;
                break;
            case 'u': {
                self->input_pos++;
                unsigned int cp;
                if (json_read_codepoint(self, &cp) < 0) return -1;

                if (0xDC00 <= cp && cp <= 0xDFFF) {
                    json_err_invalid(self, "invalid utf-16 surrogate pair");
                    return -1;
                }
                else if (0xD800 <= cp && cp <= 0xDBFF) {
                    /* utf-16 pair, parse 2nd pair */
                    unsigned int cp2;
                    if (!json_remaining(self, 6)) return ms_err_truncated();
                    if (self->input_pos[0] != '\\' || self->input_pos[1] != 'u') {
                        json_err_invalid(self, "unexpected end of hex escape");
                        return -1;
                    }
                    self->input_pos += 2;
                    if (json_read_codepoint(self, &cp2) < 0) return -1;
                    if (cp2 < 0xDC00 || cp2 > 0xDFFF) {
                        json_err_invalid(self, "invalid utf-16 surrogate pair");
                        return -1;
                    }
                    cp = 0x10000 + (((cp - 0xD800) << 10) | (cp2 - 0xDC00));
                }
                break;
            }
            default: {
                json_err_invalid(self, "invalid escaped character");
                return -1;
            }
        }
        goto parse_unicode;
    }
    else {
        json_err_invalid(self, "invalid character");
        return -1;
    }
}

#undef parse_ascii_pre
#undef parse_ascii_post
#undef parse_unicode_pre
#undef parse_unicode_post

/* A table of the corresponding base64 value for each character, or -1 if an
 * invalid character in the base64 alphabet (note the padding char '=' is
 * handled elsewhere, so is marked as invalid here as well) */
static const uint8_t base64_decode_table[] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63,
    52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1,-1,-1,-1,
    -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1,
    -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
    41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
};

static PyObject *
json_decode_binary(
    const char *buffer, Py_ssize_t size, TypeNode *type, PathNode *path
) {
    PyObject *out = NULL;
    char *bin_buffer;
    Py_ssize_t bin_size, i;

    if (size % 4 != 0) goto invalid;

    int npad = 0;
    if (size > 0 && buffer[size - 1] == '=') npad++;
    if (size > 1 && buffer[size - 2] == '=') npad++;

    bin_size = (size / 4) * 3 - npad;
    if (!ms_passes_bytes_constraints(bin_size, type, path)) return NULL;

    if (type->types & MS_TYPE_BYTES) {
        out = PyBytes_FromStringAndSize(NULL, bin_size);
        if (out == NULL) return NULL;
        bin_buffer = PyBytes_AS_STRING(out);
    }
    else if (type->types & MS_TYPE_BYTEARRAY) {
        out = PyByteArray_FromStringAndSize(NULL, bin_size);
        if (out == NULL) return NULL;
        bin_buffer = PyByteArray_AS_STRING(out);
    }
    else {
        PyObject *temp = PyBytes_FromStringAndSize(NULL, bin_size);
        if (temp == NULL) return NULL;
        bin_buffer = PyBytes_AS_STRING(temp);
        out = PyMemoryView_FromObject(temp);
        Py_DECREF(temp);
        if (out == NULL) return NULL;
    }

    int quad = 0;
    uint8_t left_c = 0;
    for (i = 0; i < size - npad; i++) {
        uint8_t c = base64_decode_table[(uint8_t)(buffer[i])];
        if (c >= 64) goto invalid;

        switch (quad) {
            case 0:
                quad = 1;
                left_c = c;
                break;
            case 1:
                quad = 2;
                *bin_buffer++ = (left_c << 2) | (c >> 4);
                left_c = c & 0x0f;
                break;
            case 2:
                quad = 3;
                *bin_buffer++ = (left_c << 4) | (c >> 2);
                left_c = c & 0x03;
                break;
            case 3:
                quad = 0;
                *bin_buffer++ = (left_c << 6) | c;
                left_c = 0;
                break;
        }
    }
    return out;

invalid:
    Py_XDECREF(out);
    return ms_error_with_path("Invalid base64 encoded string%U", path);
}

static PyObject *
json_decode_string(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    char *view = NULL;
    bool is_ascii = true;
    Py_ssize_t size = json_decode_string_view(self, &view, &is_ascii);
    if (size < 0) return NULL;

    if (MS_LIKELY(type->types & (MS_TYPE_STR | MS_TYPE_ANY))) {
        PyObject *out;
        if (MS_LIKELY(is_ascii)) {
            out = PyUnicode_New(size, 127);
            if (MS_UNLIKELY(out == NULL)) return NULL;
            memcpy(ascii_get_buffer(out), view, size);
        }
        else {
            out = PyUnicode_DecodeUTF8(view, size, NULL);
        }
        return ms_check_str_constraints(out, type, path);
    }
    else if (MS_UNLIKELY(!self->strict)) {
        bool invalid = false;
        PyObject *out = ms_decode_str_lax(view, size, type, path, &invalid);
        if (!invalid) return out;
    }

    if (MS_UNLIKELY(type->types & MS_TYPE_DATETIME)) {
        return ms_decode_datetime_from_str(view, size, type, path);
    }
    else if (MS_UNLIKELY(type->types & MS_TYPE_DATE)) {
        return ms_decode_date(view, size, path);
    }
    else if (MS_UNLIKELY(type->types & MS_TYPE_TIME)) {
        return ms_decode_time(view, size, type, path);
    }
    else if (MS_UNLIKELY(type->types & MS_TYPE_TIMEDELTA)) {
        return ms_decode_timedelta(view, size, type, path);
    }
    else if (MS_UNLIKELY(type->types & MS_TYPE_UUID)) {
        return ms_decode_uuid_from_str(view, size, path);
    }
    else if (MS_UNLIKELY(type->types & MS_TYPE_DECIMAL)) {
        return ms_decode_decimal(view, size, is_ascii, path, NULL);
    }
    else if (
        MS_UNLIKELY(type->types &
            (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY | MS_TYPE_MEMORYVIEW)
        )
    ) {
        return json_decode_binary(view, size, type, path);
    }
    else if (MS_UNLIKELY(type->types & (MS_TYPE_ENUM | MS_TYPE_STRLITERAL))) {
        return ms_decode_str_enum_or_literal(view, size, type, path);
    }
    return ms_validation_error("str", type, path);
}

static PyObject *
json_decode_dict_key_fallback(
    JSONDecoderState *self,
    const char *view, Py_ssize_t size, bool is_ascii, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_STR | MS_TYPE_ANY)) {
        PyObject *out;
        if (is_ascii) {
            out = PyUnicode_New(size, 127);
            if (MS_UNLIKELY(out == NULL)) return NULL;
            memcpy(ascii_get_buffer(out), view, size);
        }
        else {
            out = PyUnicode_DecodeUTF8(view, size, NULL);
        }
        if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC))) {
            return ms_decode_custom(out, type, path);
        }
        return ms_check_str_constraints(out, type, path);
    }
    if (type->types & (
            MS_TYPE_INT | MS_TYPE_INTENUM | MS_TYPE_INTLITERAL |
            MS_TYPE_FLOAT | MS_TYPE_DECIMAL |
            ((!self->strict) * (MS_TYPE_DATETIME | MS_TYPE_TIMEDELTA))
        )
    ) {
        PyObject *out;
        if (maybe_parse_number(view, size, type, path, self->strict, &out)) {
            return out;
        }
    }

    if (type->types & (MS_TYPE_ENUM | MS_TYPE_STRLITERAL)) {
        return ms_decode_str_enum_or_literal(view, size, type, path);
    }
    else if (type->types & MS_TYPE_UUID) {
        return ms_decode_uuid_from_str(view, size, path);
    }
    else if (type->types & MS_TYPE_DATETIME) {
        return ms_decode_datetime_from_str(view, size, type, path);
    }
    else if (type->types & MS_TYPE_DATE) {
        return ms_decode_date(view, size, path);
    }
    else if (type->types & MS_TYPE_TIME) {
        return ms_decode_time(view, size, type, path);
    }
    else if (type->types & MS_TYPE_TIMEDELTA) {
        return ms_decode_timedelta(view, size, type, path);
    }
    else if (type->types & (MS_TYPE_BYTES | MS_TYPE_MEMORYVIEW)) {
        return json_decode_binary(view, size, type, path);
    }
    else {
        return ms_validation_error("str", type, path);
    }
}

static PyObject *
json_decode_dict_key(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    bool is_ascii = true;
    char *view = NULL;
    Py_ssize_t size;
    size = json_decode_string_view(self, &view, &is_ascii);
    if (size < 0) return NULL;
    return json_decode_dict_key_fallback(self, view, size, is_ascii, type, path);
}

static PyObject *
json_decode_list(JSONDecoderState *self, TypeNode *type, TypeNode *el_type, PathNode *path) {
    unsigned char c;
    bool first = true;
    PathNode el_path = {path, 0, NULL};

    self->input_pos++; /* Skip '[' */

    PyObject *out = PyList_New(0);
    if (out == NULL) return NULL;
    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Parse item */
        PyObject *item = json_decode(self, el_type, &el_path);
        if (item == NULL) goto error;
        el_path.index++;

        /* Append item to list */
        if (MS_LIKELY((LIST_CAPACITY(out) > Py_SIZE(out)))) {
            PyList_SET_ITEM(out, Py_SIZE(out), item);
            Py_SET_SIZE(out, Py_SIZE(out) + 1);
        }
        else {
            int status = PyList_Append(out, item);
            Py_DECREF(item);
            if (MS_UNLIKELY(status < 0)) goto error;
        }
    }

    if (MS_UNLIKELY(!ms_passes_array_constraints(PyList_GET_SIZE(out), type, path))) {
        goto error;
    }

    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_set(
    JSONDecoderState *self, TypeNode *type, TypeNode *el_type, PathNode *path
) {
    PyObject *out, *item = NULL;
    unsigned char c;
    bool first = true;
    PathNode el_path = {path, 0, NULL};

    self->input_pos++; /* Skip '[' */

    out = (type->types & MS_TYPE_SET) ?  PySet_New(NULL) : PyFrozenSet_New(NULL);
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Parse item */
        item = json_decode(self, el_type, &el_path);
        if (item == NULL) goto error;
        el_path.index++;

        /* Append item to set */
        if (PySet_Add(out, item) < 0) goto error;
        Py_CLEAR(item);
    }

    if (MS_UNLIKELY(!ms_passes_array_constraints(PySet_GET_SIZE(out), type, path))) {
        goto error;
    }

    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    Py_XDECREF(item);
    return NULL;
}

static PyObject *
json_decode_vartuple(JSONDecoderState *self, TypeNode *type, TypeNode *el_type, PathNode *path) {
    PyObject *list, *item, *out = NULL;
    Py_ssize_t size, i;

    list = json_decode_list(self, type, el_type, path);
    if (list == NULL) return NULL;

    size = PyList_GET_SIZE(list);
    out = PyTuple_New(size);
    if (out != NULL) {
        for (i = 0; i < size; i++) {
            item = PyList_GET_ITEM(list, i);
            PyTuple_SET_ITEM(out, i, item);
            PyList_SET_ITEM(list, i, NULL);  /* Drop reference in old list */
        }
    }
    Py_DECREF(list);
    return out;
}

static PyObject *
json_decode_fixtuple(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    PyObject *out, *item;
    unsigned char c;
    bool first = true;
    PathNode el_path = {path, 0, NULL};
    Py_ssize_t i = 0, offset, fixtuple_size;

    TypeNode_get_fixtuple(type, &offset, &fixtuple_size);

    self->input_pos++; /* Skip '[' */

    out = PyTuple_New(fixtuple_size);
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }

    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            if (MS_UNLIKELY(i < fixtuple_size)) goto size_error;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Check we don't have too many elements */
        if (MS_UNLIKELY(i >= fixtuple_size)) goto size_error;

        /* Parse item */
        item = json_decode(self, type->details[offset + i].pointer, &el_path);
        if (item == NULL) goto error;
        el_path.index++;

        /* Add item to tuple */
        PyTuple_SET_ITEM(out, i, item);
        i++;
    }
    Py_LeaveRecursiveCall();
    return out;

size_error:
    ms_raise_validation_error(
        path,
        "Expected `array` of length %zd%U",
        fixtuple_size
    );
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_namedtuple(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    unsigned char c;
    bool first = true;
    Py_ssize_t nfields, ndefaults, nrequired;
    NamedTupleInfo *info = TypeNode_get_namedtuple_info(type);

    nfields = Py_SIZE(info);
    ndefaults = info->defaults == NULL ? 0 : PyTuple_GET_SIZE(info->defaults);
    nrequired = nfields - ndefaults;

    self->input_pos++; /* Skip '[' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyTypeObject *nt_type = (PyTypeObject *)(info->class);
    PyObject *out = nt_type->tp_alloc(nt_type, nfields);
    if (out == NULL) goto error;
    MS_TUPLE_RESET_HASH(out);
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyTuple_SET_ITEM(out, i, NULL);
    }

    Py_ssize_t i = 0;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            if (MS_UNLIKELY(i < nrequired)) goto size_error;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        /* Check we don't have too many elements */
        if (MS_UNLIKELY(i >= nfields)) goto size_error;

        /* Parse item */
        PathNode el_path = {path, i, NULL};
        PyObject *item = json_decode(self, info->types[i], &el_path);
        if (item == NULL) goto error;

        /* Add item to tuple */
        PyTuple_SET_ITEM(out, i, item);
        i++;
    }
    Py_LeaveRecursiveCall();

    /* Fill in defaults */
    for (; i < nfields; i++) {
        PyObject *item = PyTuple_GET_ITEM(info->defaults, i - nrequired);
        Py_INCREF(item);
        PyTuple_SET_ITEM(out, i, item);
    }

    return out;

size_error:
    if (ndefaults == 0) {
        ms_raise_validation_error(
            path,
            "Expected `array` of length %zd%U",
            nfields
        );
    }
    else {
        ms_raise_validation_error(
            path,
            "Expected `array` of length %zd to %zd%U",
            nrequired,
            nfields
        );
    }
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
json_decode_struct_array_inner(
    JSONDecoderState *self, StructInfo *info, PathNode *path,
    Py_ssize_t starting_index
) {
    Py_ssize_t nfields, ndefaults, nrequired, npos, i = 0;
    PyObject *out, *item = NULL;
    unsigned char c;
    bool is_gc, should_untrack;
    bool first = starting_index == 0;
    StructMetaObject *st_type = info->class;
    PathNode item_path = {path, starting_index};

    out = Struct_alloc((PyTypeObject *)(st_type));
    if (out == NULL) return NULL;

    nfields = PyTuple_GET_SIZE(st_type->struct_encode_fields);
    ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    nrequired = nfields - st_type->n_trailing_defaults;
    npos = nfields - ndefaults;
    is_gc = MS_TYPE_IS_GC(st_type);
    should_untrack = is_gc;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        /* Parse ']' or ',', then peek the next character */
        if (c == ']') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            goto error;
        }

        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            goto error;
        }

        if (MS_LIKELY(i < nfields)) {
            /* Parse item */
            item = json_decode(self, info->types[i], &item_path);
            if (MS_UNLIKELY(item == NULL)) goto error;
            Struct_set_index(out, i, item);
            if (should_untrack) {
                should_untrack = !MS_MAYBE_TRACKED(item);
            }
            i++;
            item_path.index++;
        }
        else {
            if (MS_UNLIKELY(st_type->forbid_unknown_fields == OPT_TRUE)) {
                ms_raise_validation_error(
                    path,
                    "Expected `array` of at most length %zd",
                    nfields
                );
                goto error;
            }
            else {
                /* Skip trailing fields */
                if (json_skip(self) < 0) goto error;
            }
        }
    }

    /* Check for missing required fields */
    if (i < nrequired) {
        ms_raise_validation_error(
            path,
            "Expected `array` of at least length %zd, got %zd%U",
            nrequired + starting_index,
            i + starting_index
        );
        goto error;
    }
    /* Fill in missing fields with defaults */
    for (; i < nfields; i++) {
        item = get_default(
            PyTuple_GET_ITEM(st_type->struct_defaults, i - npos)
        );
        if (item == NULL) goto error;
        Struct_set_index(out, i, item);
        if (should_untrack) {
            should_untrack = !MS_MAYBE_TRACKED(item);
        }
    }
    if (Struct_decode_post_init(st_type, out, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    if (is_gc && should_untrack && MS_IS_TRACKED(out))
        PyObject_GC_UnTrack(out);
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

/* Decode an integer. If the value fits in an int64_t, it will be stored in
 * `out`, otherwise it will be stored in `uout`. A return value of -1 indicates
 * an error. */
static int
json_decode_cint(JSONDecoderState *self, int64_t *out, uint64_t *uout, PathNode *path) {
    uint64_t mantissa = 0;
    bool is_negative = false;
    unsigned char c;
    unsigned char *orig_input_pos = self->input_pos;

    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return -1;

    /* Parse minus sign (if present) */
    if (c == '-') {
        self->input_pos++;
        c = json_peek_or_null(self);
        is_negative = true;
    }

    /* Parse integer */
    if (MS_UNLIKELY(c == '0')) {
        /* Ensure at most one leading zero */
        self->input_pos++;
        c = json_peek_or_null(self);
        if (MS_UNLIKELY(is_digit(c))) {
            json_err_invalid(self, "invalid number");
            return -1;
        }
    }
    else {
        /* Parse the integer part of the number.
         *
         * We can read the first 19 digits safely into a uint64 without
         * checking for overflow. Removing overflow checks from the loop gives
         * a measurable performance boost. */
        size_t remaining = self->input_end - self->input_pos;
        size_t n_safe = Py_MIN(19, remaining);
        while (n_safe) {
            c = *self->input_pos;
            if (!is_digit(c)) goto end_integer;
            self->input_pos++;
            n_safe--;
            mantissa = mantissa * 10 + (uint64_t)(c - '0');
        }
        if (MS_UNLIKELY(remaining > 19)) {
            /* Reading a 20th digit may or may not cause overflow. Any
             * additional digits definitely will. Read the 20th digit (and
             * check for a 21st), taking the slow path upon overflow. */
            c = *self->input_pos;
            if (MS_UNLIKELY(is_digit(c))) {
                self->input_pos++;
                uint64_t mantissa2 = mantissa * 10 + (uint64_t)(c - '0');
                bool overflowed = (mantissa2 < mantissa) || ((mantissa2 - (uint64_t)(c - '0')) / 10) != mantissa;
                if (MS_UNLIKELY(overflowed || is_digit(json_peek_or_null(self)))) {
                    goto error_not_int;
                }
                mantissa = mantissa2;
                c = json_peek_or_null(self);
            }
        }

end_integer:
        /* There must be at least one digit */
        if (MS_UNLIKELY(mantissa == 0)) goto error_not_int;
    }

    if (c == '.' || c == 'e' || c == 'E') goto error_not_int;

    if (is_negative) {
        if (mantissa > 1ull << 63) goto error_not_int;
        *out = -1 * (int64_t)mantissa;
    }
    else {
        if (mantissa > LLONG_MAX) {
            *uout = mantissa;
        }
        else {
            *out = mantissa;
        }
    }
    return 0;

error_not_int:
    /* Use skip to catch malformed JSON */
    self->input_pos = orig_input_pos;
    if (json_skip(self) < 0) return -1;

    ms_error_with_path("Expected `int`%U", path);
    return -1;
}

static Py_ssize_t
json_decode_cstr(JSONDecoderState *self, char **out, PathNode *path) {
    unsigned char c;
    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return -1;
    if (c != '"') {
        /* Use skip to catch malformed JSON */
        if (json_skip(self) < 0) return -1;
        /* JSON is valid but the wrong type */
        ms_error_with_path("Expected `str`%U", path);
        return -1;
    }
    bool is_ascii = true;
    return json_decode_string_view(self, out, &is_ascii);
}

static int
json_ensure_array_nonempty(
    JSONDecoderState *self, StructMetaObject *st_type, PathNode *path
) {
    unsigned char c;
    /* Check for an early end to the array */
    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return -1;
    if (c == ']') {
        Py_ssize_t expected_size;
        if (st_type == NULL) {
            /* If we don't know the type, the most we know is that the minimum
             * size is 1 */
            expected_size = 1;
        }
        else {
            /* n_fields - n_optional_fields + 1 tag */
            expected_size = PyTuple_GET_SIZE(st_type->struct_encode_fields)
                            - PyTuple_GET_SIZE(st_type->struct_defaults)
                            + 1;
        }
        ms_raise_validation_error(
            path,
            "Expected `array` of at least length %zd, got 0%U",
            expected_size
        );
        return -1;
    }
    return 0;
}

static int
json_ensure_tag_matches(
    JSONDecoderState *self, PathNode *path, PyObject *expected_tag
) {
    if (PyUnicode_CheckExact(expected_tag)) {
        char *tag = NULL;
        Py_ssize_t tag_size;
        tag_size = json_decode_cstr(self, &tag, path);
        if (tag_size < 0) return -1;

        /* Check that tag matches expected tag value */
        Py_ssize_t expected_size;
        const char *expected_str = unicode_str_and_size_nocheck(
            expected_tag, &expected_size
        );
        if (tag_size != expected_size || memcmp(tag, expected_str, expected_size) != 0) {
            /* Tag doesn't match the expected value, error nicely */
            ms_invalid_cstr_value(tag, tag_size, path);
            return -1;
        }
    }
    else {
        int64_t tag = 0;
        uint64_t utag = 0;
        if (json_decode_cint(self, &tag, &utag, path) < 0) return -1;
        int64_t expected = PyLong_AsLongLong(expected_tag);
        /* Tags must be int64s, if utag != 0 then we know the tags don't match.
         * We parse the full uint64 value only to validate the message and
         * raise a nice error */
        if (utag != 0) {
            ms_invalid_cuint_value(utag, path);
            return -1;
        }
        if (tag != expected) {
            ms_invalid_cint_value(tag, path);
            return -1;
        }
    }
    return 0;
}

static StructInfo *
json_decode_tag_and_lookup_type(
    JSONDecoderState *self, Lookup *lookup, PathNode *path
) {
    StructInfo *out = NULL;
    if (Lookup_IsStrLookup(lookup)) {
        Py_ssize_t tag_size;
        char *tag = NULL;
        tag_size = json_decode_cstr(self, &tag, path);
        if (tag_size < 0) return NULL;
        out = (StructInfo *)StrLookup_Get((StrLookup *)lookup, tag, tag_size);
        if (out == NULL) {
            ms_invalid_cstr_value(tag, tag_size, path);
        }
    }
    else {
        int64_t tag = 0;
        uint64_t utag = 0;
        if (json_decode_cint(self, &tag, &utag, path) < 0) return NULL;
        if (utag == 0) {
            out = (StructInfo *)IntLookup_GetInt64((IntLookup *)lookup, tag);
            if (out == NULL) {
                ms_invalid_cint_value(tag, path);
            }
        }
        else {
            /* tags can't be uint64 values, we only decode to give a nice error */
            ms_invalid_cuint_value(utag, path);
        }
    }
    return out;
}

static PyObject *
json_decode_struct_array(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    Py_ssize_t starting_index = 0;
    StructInfo *info = TypeNode_get_struct_info(type);

    self->input_pos++; /* Skip '[' */

    /* If this is a tagged struct, first read and validate the tag */
    if (info->class->struct_tag_value != NULL) {
        PathNode tag_path = {path, 0};
        if (json_ensure_array_nonempty(self, info->class, path) < 0) return NULL;
        if (json_ensure_tag_matches(self, &tag_path, info->class->struct_tag_value) < 0) return NULL;
        starting_index = 1;
    }

    /* Decode the rest of the struct */
    return json_decode_struct_array_inner(self, info, path, starting_index);
}

static PyObject *
json_decode_struct_array_union(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    PathNode tag_path = {path, 0};
    Lookup *lookup = TypeNode_get_struct_union(type);

    self->input_pos++; /* Skip '[' */
    /* Decode & lookup struct type from tag */
    if (json_ensure_array_nonempty(self, NULL, path) < 0) return NULL;
    StructInfo *info = json_decode_tag_and_lookup_type(self, lookup, &tag_path);
    if (info == NULL) return NULL;

    /* Finish decoding the rest of the struct */
    return json_decode_struct_array_inner(self, info, path, 1);
}

static PyObject *
json_decode_array(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        return json_decode_list(self, type, &type_any, path);
    }
    else if (type->types & MS_TYPE_LIST) {
        return json_decode_list(self, type, TypeNode_get_array(type), path);
    }
    else if (type->types & (MS_TYPE_SET | MS_TYPE_FROZENSET)) {
        return json_decode_set(self, type, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        return json_decode_vartuple(self, type, TypeNode_get_array(type), path);
    }
    else if (type->types & MS_TYPE_FIXTUPLE) {
        return json_decode_fixtuple(self, type, path);
    }
    else if (type->types & MS_TYPE_NAMEDTUPLE) {
        return json_decode_namedtuple(self, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY) {
        return json_decode_struct_array(self, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY_UNION) {
        return json_decode_struct_array_union(self, type, path);
    }
    return ms_validation_error("array", type, path);
}

static PyObject *
json_decode_dict(
    JSONDecoderState *self, TypeNode *type, TypeNode *key_type, TypeNode *val_type, PathNode *path
) {
    PyObject *out, *key = NULL, *val = NULL;
    unsigned char c;
    bool first = true;
    bool key_is_str = (key_type->types == MS_TYPE_STR);
    PathNode key_path = {path, PATH_KEY, NULL};
    PathNode val_path = {path, PATH_STR_BRACKET, NULL};

    self->input_pos++; /* Skip '{' */

    out = PyDict_New();
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            if (MS_LIKELY(key_is_str)) {
                /* Fast path: JSON object keys are always strings, and a plain
                 * `str` key type needs no type dispatch or constraint checks. */
                bool is_ascii = true;
                char *view = NULL;
                Py_ssize_t size = json_decode_string_view(self, &view, &is_ascii);
                if (size < 0) goto error;
                if (MS_LIKELY(is_ascii)) {
                    key = PyUnicode_New(size, 127);
                    if (MS_UNLIKELY(key == NULL)) goto error;
                    memcpy(ascii_get_buffer(key), view, size);
                }
                else {
                    key = PyUnicode_DecodeUTF8(view, size, NULL);
                    if (MS_UNLIKELY(key == NULL)) goto error;
                }
            }
            else {
                key = json_decode_dict_key(self, key_type, &key_path);
                if (key == NULL) goto error;
            }
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        val_path.object = key;
        val = json_decode(self, val_type, &val_path);
        if (val == NULL) goto error;

        /* Add item to dict */
        if (MS_UNLIKELY(PyDict_SetItem(out, key, val) < 0))
            goto error;
        Py_CLEAR(key);
        Py_CLEAR(val);
    }

    if (MS_UNLIKELY(!ms_passes_map_constraints(PyDict_GET_SIZE(out), type, path))) goto error;

    Py_LeaveRecursiveCall();
    return out;

error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(key);
    Py_XDECREF(val);
    Py_DECREF(out);
    return NULL;
}

#if PY315_PLUS
static PyObject *
json_decode_frozendict(
    JSONDecoderState *self, TypeNode *type, TypeNode *key_type, TypeNode *val_type, PathNode *path
) {
    PyObject *dict = json_decode_dict(self, type, key_type, val_type, path);
    return _PyFrozenDict_NewSteal(dict);
}
#endif

static PyObject *
json_decode_typeddict(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    PyObject *out;
    unsigned char c;
    char *key = NULL;
    bool first = true;
    Py_ssize_t key_size, nrequired = 0, pos = 0;
    TypedDictInfo *info = TypeNode_get_typeddict_info(type);

    self->input_pos++; /* Skip '{' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    out = PyDict_New();
    if (out == NULL) goto error;

    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            bool is_ascii = true;
            key_size = json_decode_string_view(self, &key, &is_ascii);
            if (key_size < 0) goto error;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        TypeNode *field_type;
        PyObject *field = TypedDictInfo_lookup_key(info, key, key_size, &field_type, &pos);

        if (field != NULL) {
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = json_decode(self, field_type, &field_path);
            if (val == NULL) goto error;
            /* We want to keep a count of required fields we've decoded. Since
             * duplicates can occur, we stash the current dict size, then only
             * increment if the dict size has changed _and_ the field is
             * required. */
            Py_ssize_t cur_size = PyDict_GET_SIZE(out);
            int status = PyDict_SetItem(out, field, val);
            /* Always decref value, no need to decref key since it's a borrowed
             * reference. */
            Py_DECREF(val);
            if (status < 0) goto error;
            if ((PyDict_GET_SIZE(out) != cur_size) && (field_type->types & MS_EXTRA_FLAG)) {
                nrequired++;
            }
        }
        else {
            /* Skip unknown fields */
            if (json_skip(self) < 0) goto error;
        }
    }
    if (nrequired < info->nrequired) {
        /* A required field is missing, determine which one and raise */
        TypedDictInfo_error_missing(info, out, path);
        goto error;
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_dataclass(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    PyObject *out;
    unsigned char c;
    char *key = NULL;
    bool first = true;
    Py_ssize_t key_size, pos = 0;
    DataclassInfo *info = TypeNode_get_dataclass_info(type);

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyTypeObject *dataclass_type = (PyTypeObject *)(info->class);
    out = dataclass_type->tp_alloc(dataclass_type, 0);
    if (out == NULL) goto error;
    if (info->pre_init != NULL) {
        PyObject *res = PyObject_CallOneArg(info->pre_init, out);
        if (res == NULL) goto error;
        Py_DECREF(res);
    }

    self->input_pos++; /* Skip '{' */

    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            bool is_ascii = true;
            key_size = json_decode_string_view(self, &key, &is_ascii);
            if (key_size < 0) goto error;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        TypeNode *field_type;
        PyObject *field = DataclassInfo_lookup_key(info, key, key_size, &field_type, &pos);

        if (field != NULL) {
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = json_decode(self, field_type, &field_path);
            if (val == NULL) goto error;
            int status = PyObject_GenericSetAttr(out, field, val);
            Py_DECREF(val);
            if (status < 0) goto error;
        }
        else {
            /* Skip unknown fields */
            if (json_skip(self) < 0) goto error;
        }
    }
    if (DataclassInfo_post_decode(info, out, path) < 0) goto error;

    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
json_decode_struct_map_inner(
    JSONDecoderState *self, StructInfo *info, PathNode *path,
    Py_ssize_t starting_index
) {
    PyObject *out, *val = NULL;
    Py_ssize_t key_size, field_index, pos = 0;
    unsigned char c;
    char *key = NULL;
    bool first = starting_index == 0;
    StructMetaObject *st_type = info->class;
    PathNode field_path = {path, 0, (PyObject *)st_type};

    out = Struct_alloc((PyTypeObject *)(st_type));
    if (out == NULL) return NULL;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    while (true) {
        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        }
        else if (first) {
            /* Only the first item doesn't need a comma delimiter */
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            goto error;
        }

        /* Parse a string key */
        if (c == '"') {
            bool is_ascii = true;
            key_size = json_decode_string_view(self, &key, &is_ascii);
            if (key_size < 0) goto error;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            goto error;
        }
        else {
            json_err_invalid(self, "object keys must be strings");
            goto error;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) goto error;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            goto error;
        }
        self->input_pos++;

        /* Parse value */
        field_index = StructMeta_get_field_index(st_type, key, key_size, &pos);
        if (MS_LIKELY(field_index >= 0)) {
            field_path.index = field_index;
            TypeNode *type = info->types[field_index];
            assert(type != NULL);
            val = json_decode(self, type, &field_path);
            if (val == NULL) goto error;
            Struct_set_index(out, field_index, val);
        }
        else if (MS_UNLIKELY(field_index == -2)) {
            /* Decode and check that the tag value matches the expected value */
            PathNode tag_path = {path, PATH_STR, st_type->struct_tag_field};
            if (json_ensure_tag_matches(self, &tag_path, st_type->struct_tag_value) < 0) {
                goto error;
            }
        }
        else {
            /* Unknown field */
            if (MS_UNLIKELY(st_type->forbid_unknown_fields == OPT_TRUE)) {
                ms_error_unknown_field(key, key_size, path);
                goto error;
            }
            else {
                if (json_skip(self) < 0) goto error;
            }
        }
    }
    if (Struct_fill_in_defaults(st_type, out, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    return out;

error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

static PyObject *
json_decode_struct_map(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    StructInfo *info = TypeNode_get_struct_info(type);

    self->input_pos++; /* Skip '{' */

    return json_decode_struct_map_inner(self, info, path, 0);
}

static PyObject *
json_decode_struct_union(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    Lookup *lookup = TypeNode_get_struct_union(type);
    PathNode tag_path = {path, PATH_STR, Lookup_tag_field(lookup)};
    Py_ssize_t tag_field_size;
    const char *tag_field = unicode_str_and_size_nocheck(
        Lookup_tag_field(lookup), &tag_field_size
    );

    self->input_pos++; /* Skip '{' */

    /* Cache the current input position in case we need to reset it once the
     * tag is found */
    unsigned char *orig_input_pos = self->input_pos;

    for (Py_ssize_t i = 0; ; i++) {
        unsigned char c;

        /* Parse '}' or ',', then peek the next character */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;
        if (c == '}') {
            self->input_pos++;
            break;
        }
        else if (c == ',' && (i != 0)) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;
        }
        else if (i != 0) {
            return json_err_invalid(self, "expected ',' or '}'");
        }

        /* Parse a string key */
        Py_ssize_t key_size;
        char *key = NULL;
        if (c == '"') {
            bool is_ascii = true;
            key_size = json_decode_string_view(self, &key, &is_ascii);
            if (key_size < 0) return NULL;
        }
        else if (c == '}') {
            return json_err_invalid(self, "trailing comma in object");
        }
        else {
            return json_err_invalid(self, "object keys must be strings");
        }

        /* Check if key matches tag_field */
        bool tag_found = false;
        if (key_size == tag_field_size && memcmp(key, tag_field, key_size) == 0) {
            tag_found = true;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;
        if (c != ':') {
            return json_err_invalid(self, "expected ':'");
        }
        self->input_pos++;

        /* Parse value */
        if (tag_found) {
            /* Decode & lookup struct type from tag */
            StructInfo *info = json_decode_tag_and_lookup_type(self, lookup, &tag_path);
            if (info == NULL) return NULL;
            if (i != 0) {
                /* tag wasn't first field, reset decoder position */
                self->input_pos = orig_input_pos;
            }
            return json_decode_struct_map_inner(self, info, path, i == 0 ? 1 : 0);
        }
        else {
            if (json_skip(self) < 0) return NULL;
        }
    }

    ms_missing_required_field(Lookup_tag_field(lookup), path);
    return NULL;
}

static PyObject *
json_decode_object(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_ANY) {
        TypeNode type_any = {MS_TYPE_ANY};
        return json_decode_dict(self, type, &type_any, &type_any, path);
    }
    else if (type->types & MS_ANY_DICT) {
        TypeNode *key, *val;
        TypeNode_get_dict(type, &key, &val);
#if PY315_PLUS
        if (type->types & MS_TYPE_FROZENDICT) {
            return json_decode_frozendict(self, type, key, val, path);
        }
#endif
        return json_decode_dict(self, type, key, val, path);
    }
    else if (type->types & MS_TYPE_TYPEDDICT) {
        return json_decode_typeddict(self, type, path);
    }
    else if (type->types & MS_TYPE_DATACLASS) {
        return json_decode_dataclass(self, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT) {
        return json_decode_struct_map(self, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_UNION) {
        return json_decode_struct_union(self, type, path);
    }
    return ms_validation_error("object", type, path);
}

static PyObject *
json_maybe_decode_number(JSONDecoderState *self, TypeNode *type, PathNode *path) {
    const char *errmsg = NULL;
    const unsigned char *pout;
    PyObject *out = parse_number_inline(
        self->input_pos, self->input_end,
        &pout, &errmsg,
        type, path, self->strict, self->float_hook, false
    );
    self->input_pos = (unsigned char *)pout;

    if (MS_UNLIKELY(out == NULL)) {
        if (errmsg != NULL) {
            json_err_invalid(self, errmsg);
        }
    }
    return out;
}

static MS_NOINLINE PyObject *
json_decode_raw(JSONDecoderState *self) {
    unsigned char c;
    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;
    const unsigned char *start = self->input_pos;
    if (json_skip(self) < 0) return NULL;
    Py_ssize_t size = self->input_pos - start;
    return Raw_FromView(self->buffer_obj, (char *)start, size);
}

/* Fast path for the common `str`/`Any` string decode, inlined so the hot path
 * avoids the large non-inline `json_decode_string` (which also handles
 * datetime, uuid, decimal, bytes, enum, ...). */
static MS_INLINE PyObject *
json_decode_str_fast(JSONDecoderState *self, TypeNode *type, PathNode *path)
{
    char *view = NULL;
    bool is_ascii = true;
    Py_ssize_t size = json_decode_string_view(self, &view, &is_ascii);
    if (size < 0) return NULL;
    PyObject *out;
    if (MS_LIKELY(is_ascii)) {
        out = PyUnicode_New(size, 127);
        if (MS_UNLIKELY(out == NULL)) return NULL;
        memcpy(ascii_get_buffer(out), view, size);
    }
    else {
        out = PyUnicode_DecodeUTF8(view, size, NULL);
    }
    return ms_check_str_constraints(out, type, path);
}

static MS_INLINE PyObject *
json_decode_nocustom(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    unsigned char c;

    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return NULL;

    switch (c) {
        case 'n': return json_decode_none(self, type, path);
        case 't': return json_decode_true(self, type, path);
        case 'f': return json_decode_false(self, type, path);
        case '[': return json_decode_array(self, type, path);
        case '{': return json_decode_object(self, type, path);
        case '"':
            if (MS_LIKELY(type->types & (MS_TYPE_STR | MS_TYPE_ANY))) {
                return json_decode_str_fast(self, type, path);
            }
            return json_decode_string(self, type, path);
        default: return json_maybe_decode_number(self, type, path);
    }
}

static PyObject *
json_decode(
    JSONDecoderState *self, TypeNode *type, PathNode *path
) {
    if (MS_UNLIKELY(type->types == 0)) {
        return json_decode_raw(self);
    }
    PyObject *obj = json_decode_nocustom(self, type, path);
    if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC))) {
        return ms_decode_custom(obj, type, path);
    }
    return obj;
}

static int
json_skip_ident(JSONDecoderState *self, const char *ident, size_t len) {
    self->input_pos++;  /* Already checked first char */
    if (MS_UNLIKELY(!json_remaining(self, len))) return ms_err_truncated();
    if (memcmp(self->input_pos, ident, len) != 0) {
        json_err_invalid(self, "invalid character");
        return -1;
    }
    self->input_pos += len;
    return 0;
}

static int
json_skip_array(JSONDecoderState *self) {
    unsigned char c;
    bool first = true;
    int out = -1;

    self->input_pos++; /* Skip '[' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        if (c == ']') {
            self->input_pos++;
            out = 0;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        }
        else if (first) {
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or ']'");
            break;
        }
        if (MS_UNLIKELY(c == ']')) {
            json_err_invalid(self, "trailing comma in array");
            break;
        }

        if (json_skip(self) < 0) break;
    }
    Py_LeaveRecursiveCall();
    return out;
}

static int
json_skip_object(JSONDecoderState *self) {
    unsigned char c;
    bool first = true;
    int out = -1;

    self->input_pos++; /* Skip '{' */

    if (Py_EnterRecursiveCall(" while deserializing an object")) return -1;
    while (true) {
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        if (c == '}') {
            self->input_pos++;
            out = 0;
            break;
        }
        else if (c == ',' && !first) {
            self->input_pos++;
            if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        }
        else if (first) {
            first = false;
        }
        else {
            json_err_invalid(self, "expected ',' or '}'");
            break;
        }

        /* Skip key */
        if (c == '"') {
            if (json_skip_string(self) < 0) break;
        }
        else if (c == '}') {
            json_err_invalid(self, "trailing comma in object");
            break;
        }
        else {
            json_err_invalid(self, "expected '\"'");
            break;
        }

        /* Parse colon */
        if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) break;
        if (c != ':') {
            json_err_invalid(self, "expected ':'");
            break;
        }
        self->input_pos++;

        /* Skip value */
        if (json_skip(self) < 0) break;
    }
    Py_LeaveRecursiveCall();
    return out;
}

static int
json_maybe_skip_number(JSONDecoderState *self) {
    /* We know there is at least one byte available when this function is
     * called */
    char c = *self->input_pos;

    /* Parse minus sign (if present) */
    if (c == '-') {
        self->input_pos++;
        c = json_peek_or_null(self);
    }

    /* Parse integer */
    if (MS_UNLIKELY(c == '0')) {
        /* Ensure at most one leading zero */
        self->input_pos++;
        c = json_peek_or_null(self);
        if (MS_UNLIKELY(is_digit(c))) {
            json_err_invalid(self, "invalid number");
            return -1;
        }
    }
    else {
        /* Skip the integer part of the number. */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            self->input_pos++;
        }
        /* There must be at least one digit */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) {
            json_err_invalid(self, "invalid character");
            return -1;
        }
    }

    c = json_peek_or_null(self);
    if (c == '.') {
        self->input_pos++;
        /* Skip remaining digits until invalid/unknown character */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            self->input_pos++;
        }
        /* Error if no digits after decimal */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) {
            json_err_invalid(self, "invalid number");
            return -1;
        }

        c = json_peek_or_null(self);
    }
    if (c == 'e' || c == 'E') {
        self->input_pos++;

        /* Parse exponent sign (if any) */
        c = json_peek_or_null(self);
        if (c == '+' || c == '-') {
            self->input_pos++;
        }

        /* Parse exponent digits */
        unsigned char *cur_pos = self->input_pos;
        while (self->input_pos < self->input_end && is_digit(*self->input_pos)) {
            self->input_pos++;
        }
        /* Error if no digits in exponent */
        if (MS_UNLIKELY(cur_pos == self->input_pos)) {
            json_err_invalid(self, "invalid number");
            return -1;
        }
    }
    return 0;
}

static int
json_skip(JSONDecoderState *self)
{
    unsigned char c;

    if (MS_UNLIKELY(!json_peek_skip_ws(self, &c))) return -1;

    switch (c) {
        case 'n': return json_skip_ident(self, "ull", 3);
        case 't': return json_skip_ident(self, "rue", 3);
        case 'f': return json_skip_ident(self, "alse", 4);
        case '"': return json_skip_string(self);
        case '[': return json_skip_array(self);
        case '{': return json_skip_object(self);
        default: return json_maybe_skip_number(self);
    }
}


PyDoc_STRVAR(JSONDecoder_decode__doc__,
"decode(self, buf)\n"
"--\n"
"\n"
"Deserialize an object from JSON.\n"
"\n"
"Parameters\n"
"----------\n"
"buf : bytes-like or str\n"
"    The message to decode.\n"
"\n"
"Returns\n"
"-------\n"
"obj : Any\n"
"    The deserialized object.\n"
);
static PyObject*
JSONDecoder_decode(JSONDecoder *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (!check_positional_nargs(nargs, 1, 1)) {
        return NULL;
    }

    JSONDecoderState state = {
        .type = self->type,
        .strict = self->strict,
        .float_hook = self->float_hook,
        .scratch = NULL,
        .scratch_capacity = 0,
        .scratch_len = 0
    };

    Py_buffer buffer;
    buffer.buf = NULL;
    if (ms_get_buffer(args[0], &buffer) >= 0) {

        state.buffer_obj = args[0];
        state.input_start = buffer.buf;
        state.input_pos = buffer.buf;
        state.input_end = state.input_pos + buffer.len;

        PyObject *res = json_decode(&state, state.type, NULL);

        if (res != NULL && json_has_trailing_characters(&state)) {
            Py_CLEAR(res);
        }

        ms_release_buffer(&buffer);

        PyMem_Free(state.scratch);
        return res;
    }

    return NULL;
}

static PyMemberDef JSONDecoder_members[] = {
    {"type", T_OBJECT_EX, offsetof(JSONDecoder, orig_type), READONLY, "The Decoder type"},
    {"strict", T_BOOL, offsetof(JSONDecoder, strict), READONLY, "The Decoder strict setting"},
    {"float_hook", T_OBJECT, offsetof(JSONDecoder, float_hook), READONLY, "The Decoder float_hook"},
    {NULL},
};

static struct PyMethodDef JSONDecoder_methods[] = {
    {
        "decode", (PyCFunction) JSONDecoder_decode, METH_FASTCALL,
        JSONDecoder_decode__doc__,
    },
    {"__class_getitem__", Py_GenericAlias, METH_O|METH_CLASS},
    {NULL, NULL}                /* sentinel */
};

static PyTypeObject JSONDecoder_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "structtype.json.Decoder",
    .tp_doc = JSONDecoder__doc__,
    .tp_basicsize = sizeof(JSONDecoder),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)JSONDecoder_init,
    .tp_traverse = (traverseproc)JSONDecoder_traverse,
    .tp_clear = (inquiry)JSONDecoder_clear,
    .tp_dealloc = (destructor)JSONDecoder_dealloc,
    .tp_repr = (reprfunc)JSONDecoder_repr,
    .tp_methods = JSONDecoder_methods,
    .tp_members = JSONDecoder_members,
};

/* Shared decode core: parse `buf` as JSON into `type` with the given
 * `strict` option. Used both by the module-level
 * `structtype_json_decode` and by the `Struct.struct_validate_json` method
 * (which supplies the struct type directly, avoiding keyword re-marshaling). */
static PyObject *
json_decode_common(
    StructspecState *mod, PyObject *buf, PyObject *type, int strict
)
{
    PyObject *res = NULL;

    JSONDecoderState state = {
        .strict = strict,
        .float_hook = NULL,
        .scratch = NULL,
        .scratch_capacity = 0,
        .scratch_len = 0
    };

    /* Allocate Any & Struct type nodes (simple, common cases) on the stack,
     * everything else on the heap */
    TypeNode typenode_any = {MS_TYPE_ANY};
    TypeNodeSimple typenode_struct;
    if (type == NULL || type == mod->typing_any) {
        state.type = &typenode_any;
    }
    else if (ms_is_struct_cls(type)) {
        PyObject *info = StructInfo_Convert(type);
        if (info == NULL) return NULL;
        bool array_like = ((StructMetaObject *)type)->array_like == OPT_TRUE;
        typenode_struct.types = array_like ? MS_TYPE_STRUCT_ARRAY : MS_TYPE_STRUCT;
        typenode_struct.details[0].pointer = info;
        state.type = (TypeNode *)(&typenode_struct);
    }
    else {
        state.type = TypeNode_Convert(type);
        if (state.type == NULL) return NULL;
    }

    Py_buffer buffer;
    buffer.buf = NULL;
    if (ms_get_buffer(buf, &buffer) >= 0) {
        state.buffer_obj = buf;
        state.input_start = buffer.buf;
        state.input_pos = buffer.buf;
        state.input_end = state.input_pos + buffer.len;

        res = json_decode(&state, state.type, NULL);

        if (res != NULL && json_has_trailing_characters(&state)) {
            Py_CLEAR(res);
        }

        ms_release_buffer(&buffer);
    }

    PyMem_Free(state.scratch);

    if (state.type == (TypeNode *)&typenode_struct) {
        Py_DECREF(typenode_struct.details[0].pointer);
    }
    else if (state.type != &typenode_any) {
        TypeNode_Free(state.type);
    }

    return res;
}

static PyObject*
structtype_json_decode(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyObject *buf = NULL, *type = NULL, *strict_obj = NULL;
    int strict = 1;
    StructspecState *mod = structtype_get_state(self);

    /* Parse arguments */
    if (!check_positional_nargs(nargs, 1, 1)) return NULL;
    buf = args[0];
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        if ((type = find_keyword(kwnames, args + nargs, mod->str_type)) != NULL) nkwargs--;
        if ((strict_obj = find_keyword(kwnames, args + nargs, mod->str_strict)) != NULL) nkwargs--;
        if (nkwargs > 0) {
            PyErr_SetString(
                PyExc_TypeError,
                "Extra keyword arguments provided"
            );
            return NULL;
        }
    }

    /* Handle strict */
    if (strict_obj != NULL) {
        strict = PyObject_IsTrue(strict_obj);
        if (strict < 0) return NULL;
    }

    return json_decode_common(mod, buf, type, strict);
}

/*************************************************************************
 * dump                                                           *
 *************************************************************************/

#define MS_BUILTIN_BYTES      (1ull << 0)
#define MS_BUILTIN_BYTEARRAY  (1ull << 1)
#define MS_BUILTIN_MEMORYVIEW (1ull << 2)
#define MS_BUILTIN_DATETIME   (1ull << 3)
#define MS_BUILTIN_DATE       (1ull << 4)
#define MS_BUILTIN_TIME       (1ull << 5)
#define MS_BUILTIN_UUID       (1ull << 6)
#define MS_BUILTIN_DECIMAL    (1ull << 7)
#define MS_BUILTIN_TIMEDELTA  (1ull << 8)
#define MS_BUILTIN_FROZENDICT (1ull << 9)
#define MS_BUILTIN_ENUM       (1ull << 10)

typedef struct {
    StructspecState *mod;
    PyObject *codecs;           /* current per-field {type: dump} map, or NULL */
    bool str_keys;
    bool sort_keys;
    uint32_t builtin_types;
    PyObject *builtin_types_seq;
} DumpState;

static PyObject * dump_obj(DumpState *, PyObject *, bool);

static PyObject *
dump_enum(DumpState *self, PyObject *obj)
{
    return dump_enum_value(self->mod, obj);
}

static PyObject *
dump_binary(DumpState *self, const char *buf, Py_ssize_t size) {
    Py_ssize_t output_size = ms_encode_base64_size(self->mod, size);
    if (output_size < 0) return NULL;
    PyObject *out = PyUnicode_New(output_size, 127);
    if (out == NULL) return NULL;
    ms_encode_base64(buf, size, ascii_get_buffer(out));
    return out;
}

static PyObject *
dump_datetime(DumpState *self, PyObject *obj) {
    char buf[32];
    int size = ms_encode_datetime(self->mod, obj, buf);
    if (size < 0) return NULL;
    PyObject *out = PyUnicode_New(size, 127);
    if (MS_UNLIKELY(out == NULL)) return NULL;
    memcpy(ascii_get_buffer(out), buf, size);
    return out;
}

static PyObject *
dump_date(DumpState *self, PyObject *obj) {
    PyObject *out = PyUnicode_New(10, 127);
    if (out == NULL) return NULL;
    ms_encode_date(obj, ascii_get_buffer(out));
    return out;
}

static PyObject *
dump_time(DumpState *self, PyObject *obj) {
    char buf[21];
    int size = ms_encode_time(self->mod, obj, buf);
    if (size < 0) return NULL;
    PyObject *out = PyUnicode_New(size, 127);
    if (MS_UNLIKELY(out == NULL)) return NULL;
    memcpy(ascii_get_buffer(out), buf, size);
    return out;
}

static PyObject *
dump_timedelta(DumpState *self, PyObject *obj) {
    char buf[26];
    int size = ms_encode_timedelta(obj, buf);
    if (MS_UNLIKELY(size < 0)) return NULL;
    PyObject *out = PyUnicode_New(size, 127);
    if (MS_UNLIKELY(out == NULL)) return NULL;
    memcpy(ascii_get_buffer(out), buf, size);
    return out;
}

static PyObject *
dump_uuid(DumpState *self, PyObject *obj) {
    PyObject *out = PyUnicode_New(36, 127);
    if (out == NULL) return NULL;
    if (ms_encode_uuid(self->mod, obj, ascii_get_buffer(out), true) < 0) {
        Py_CLEAR(out);
    }
    return out;
}

static PyObject *
dump_decimal(DumpState *self, PyObject *obj) {
    return PyObject_Str(obj);
}

static PyObject *
dump_list(DumpState *self, PyObject *obj) {
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    Py_ssize_t size = PyList_GET_SIZE(obj);
    PyObject *out = PyList_New(size);
    if (out == NULL) goto cleanup;
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *item = PyList_GET_ITEM(obj, i);
        PyObject *new = dump_obj(self, item, false);
        if (new == NULL) {
            Py_CLEAR(out);
            goto cleanup;
        }
        PyList_SET_ITEM(out, i, new);
    }

cleanup:
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
dump_tuple(DumpState *self, PyObject *obj, bool is_key) {
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    Py_ssize_t size = PyTuple_GET_SIZE(obj);
    PyObject *out = PyTuple_New(size);
    if (out == NULL) goto cleanup;
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *item = PyTuple_GET_ITEM(obj, i);
        PyObject *new = dump_obj(self, item, is_key);
        if (new == NULL) {
            Py_CLEAR(out);
            goto cleanup;
        }
        PyTuple_SET_ITEM(out, i, new);
    }
cleanup:
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
dump_set(DumpState *self, PyObject *obj, bool is_key) {
    PyObject *out = NULL, *list = NULL, *iter = NULL, *item;
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    list = PyList_New(PySet_GET_SIZE(obj));
    if (list == NULL) goto cleanup;

    iter = PyObject_GetIter(obj);
    if (iter == NULL) goto cleanup;

    Py_ssize_t i = 0;
    while ((item = PyIter_Next(iter)) != NULL) {
        if (self->sort_keys) {
            PyList_SET_ITEM(list, i++, item);
        }
        else {
            PyObject *new_item = dump_obj(self, item, is_key);
            Py_DECREF(item);
            if (new_item == NULL) goto cleanup;
            PyList_SET_ITEM(list, i++, new_item);
        }
    }
    if (PyErr_Occurred()) goto cleanup;

    if (self->sort_keys) {
        if (PyList_Sort(list) < 0) goto cleanup;
        Py_ssize_t size = PyList_GET_SIZE(list);
        for (i = 0; i < size; i++) {
            item = PyList_GET_ITEM(list, i);
            PyObject *new_item = dump_obj(self, item, is_key);
            if (new_item == NULL) goto cleanup;
            PyList_SET_ITEM(list, i, new_item);
            Py_DECREF(item);
        }
    }

    if (is_key) {
        out = PyList_AsTuple(list);
    }
    else {
        Py_INCREF(list);
        out = list;
    }

cleanup:
    Py_LeaveRecursiveCall();
    Py_XDECREF(iter);
    Py_XDECREF(list);
    return out;
}

static void
sort_dict_inplace(PyObject **dict) {
    PyObject *out = NULL, *new = NULL, *keys = NULL;

    new = PyDict_New();
    if (new == NULL) goto error;

    keys = PyDict_Keys(*dict);
    if (keys == NULL) goto error;
    if (PyList_Sort(keys) < 0) goto error;

    Py_ssize_t size = PyList_GET_SIZE(keys);
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *key = PyList_GET_ITEM(keys, i);
        PyObject *val = PyDict_GetItem(*dict, key);
        if (val == NULL) goto error;
        if (PyDict_SetItem(new, key, val) < 0) goto error;
    }
    Py_INCREF(new);
    out = new;
error:
    Py_XDECREF(new);
    Py_XDECREF(keys);
    Py_XDECREF(*dict);
    *dict = out;
}

static PyObject *
dump_dict(DumpState *self, PyObject *obj) {
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    PyObject *new_key = NULL, *new_val = NULL, *key, *val;
    bool ok = false;
    PyObject *out = PyDict_New();
    Py_BEGIN_CRITICAL_SECTION(obj);
    if (out == NULL) goto cleanup;

    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &key, &val)) {
        new_key = dump_obj(self, key, true);
        if (new_key == NULL) goto cleanup;
        if (self->str_keys) {
            if (PyLong_CheckExact(new_key) || PyFloat_CheckExact(new_key)) {
                PyObject *temp = PyObject_Str(new_key);
                if (temp == NULL) goto cleanup;
                Py_DECREF(new_key);
                new_key = temp;
            }
            else if (!PyUnicode_CheckExact(new_key)) {
                PyErr_SetString(
                    PyExc_TypeError,
                    "Only dicts with str-like or number-like keys are supported"
                );
                goto cleanup;
            }
        }
        new_val = dump_obj(self, val, false);
        if (new_val == NULL) goto cleanup;
        if (PyDict_SetItem(out, new_key, new_val) < 0) goto cleanup;
        Py_CLEAR(new_key);
        Py_CLEAR(new_val);
    }
    if (MS_UNLIKELY(self->sort_keys)) {
        sort_dict_inplace(&out);
    }
    ok = true;

cleanup:;
    Py_END_CRITICAL_SECTION();
    Py_LeaveRecursiveCall();
    if (!ok) {
        Py_CLEAR(out);
        Py_XDECREF(new_key);
        Py_XDECREF(new_val);
    }
    return out;
}

#if PY315_PLUS
static PyObject *
dump_frozendict(DumpState *self, PyObject *obj) {
    PyObject *out = dump_dict(self, obj);
    if (self->builtin_types & MS_BUILTIN_FROZENDICT) {
        return _PyFrozenDict_NewSteal(out);
    }
    return out;
}
#endif

static PyObject *
dump_struct(DumpState *self, PyObject *obj, bool is_key) {
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    bool ok = false;
    PyObject *out = NULL;
    StructMetaObject *struct_type = (StructMetaObject *)Py_TYPE(obj);
    PyObject *tag_field = struct_type->struct_tag_field;
    PyObject *tag_value = struct_type->struct_tag_value;
    PyObject *fields = struct_type->struct_encode_fields;
    PyObject *defaults = struct_type->struct_defaults;
    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    Py_ssize_t npos = nfields - PyTuple_GET_SIZE(defaults);
    bool omit_defaults = struct_type->omit_defaults == OPT_TRUE;
    PyObject *field_codecs = struct_type->struct_field_codecs;
    PyObject *saved_codecs = self->codecs;
    bool has_codecs = (field_codecs != NULL);

    if (struct_type->array_like == OPT_TRUE) {
        Py_ssize_t tagged = (tag_value != NULL);
        Py_ssize_t size = nfields + tagged;
        if (is_key) {
            out = PyTuple_New(size);
        }
        else {
            out = PyList_New(size);
        }
        if (out == NULL) goto cleanup;

        if (tagged) {
            Py_INCREF(tag_value);
            if (is_key) {
                PyTuple_SET_ITEM(out, 0, tag_value);
            }
            else {
                PyList_SET_ITEM(out, 0, tag_value);
            }
        }
        for (Py_ssize_t i = 0; i < nfields; i++) {
            PyObject *val = Struct_get_index(obj, i);
            if (val == NULL) goto cleanup;
            if (has_codecs) self->codecs = PyTuple_GET_ITEM(field_codecs, i);
            PyObject *val2 = dump_obj(self, val, is_key);
            if (has_codecs) self->codecs = saved_codecs;
            if (val2 == NULL) goto cleanup;
            if (is_key) {
                PyTuple_SET_ITEM(out, i + tagged, val2);
            }
            else {
                PyList_SET_ITEM(out, i + tagged, val2);
            }
        }
    }
    else {
        out = PyDict_New();
        if (out == NULL) goto cleanup;
        if (tag_value != NULL) {
            if (PyDict_SetItem(out, tag_field, tag_value) < 0) goto cleanup;
        }
        for (Py_ssize_t i = 0; i < nfields; i++) {
            PyObject *key = PyTuple_GET_ITEM(fields, i);
            PyObject *val = Struct_get_index(obj, i);
            if (MS_UNLIKELY(val == NULL)) goto cleanup;
            if (MS_UNLIKELY(val == UNSET)) continue;
            if (
                !omit_defaults ||
                i < npos ||
                !is_default(val, PyTuple_GET_ITEM(defaults, i - npos))
            ) {
                if (has_codecs) self->codecs = PyTuple_GET_ITEM(field_codecs, i);
                PyObject *val2 = dump_obj(self, val, false);
                if (has_codecs) self->codecs = saved_codecs;
                if (val2 == NULL) goto cleanup;
                int status = PyDict_SetItem(out, key, val2);
                Py_DECREF(val2);
                if (status < 0) goto cleanup;
            }
        }
    }
    self->codecs = saved_codecs;
    ok = true;

cleanup:
    Py_LeaveRecursiveCall();
    if (!ok) {
        Py_CLEAR(out);
    }
    return out;
}

static PyObject *
dump_external_struct(DumpState *self, PyObject *obj) {
    /* External struct type (e.g. msgspec.Struct) — convert fields to dict
     * using Python-level attribute access (no C offset assumptions). */
    PyObject *fields = PyObject_GetAttrString(obj, "__struct_fields__");
    if (fields == NULL) return NULL;

    Py_ssize_t nfields = PyTuple_GET_SIZE(fields);
    PyObject *out = PyDict_New();
    if (out == NULL) { Py_DECREF(fields); return NULL; }

    bool ok = false;
    if (Py_EnterRecursiveCall(" while serializing an object")) { Py_DECREF(fields); Py_DECREF(out); return NULL; }

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *key = PyTuple_GET_ITEM(fields, i);
        PyObject *val = PyObject_GetAttr(obj, key);
        if (val == NULL) { PyErr_Clear(); continue; }
        PyObject *val2 = dump_obj(self, val, false);
        if (val2 == NULL) { Py_DECREF(val); goto cleanup; }
        int status = PyDict_SetItem(out, key, val2);
        Py_DECREF(val2);
        Py_DECREF(val);
        if (status < 0) goto cleanup;
    }

    ok = true;
cleanup:
    Py_LeaveRecursiveCall();
    Py_DECREF(fields);
    if (!ok) { Py_CLEAR(out); }
    return out;
}

static PyObject *
dump_dataclass(DumpState *self, PyObject *obj, PyObject *fields)
{
    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    bool ok = false;
    PyObject *out = NULL;
    DataclassIter iter;
    if (!dataclass_iter_setup(&iter, obj, fields)) goto cleanup;

    out = PyDict_New();
    if (out == NULL) goto cleanup;

    PyObject *field, *val;
    while (dataclass_iter_next(&iter, &field, &val)) {
        PyObject *val2 = dump_obj(self, val, false);
        int status = (
            (val2 == NULL) ? -1 : PyDict_SetItem(out, field, val2)
        );
        Py_DECREF(val);
        Py_XDECREF(val2);
        if (status < 0) goto cleanup;
    }
    ok = true;

cleanup:
    Py_LeaveRecursiveCall();
    dataclass_iter_cleanup(&iter);
    if (!ok) {
        Py_CLEAR(out);
    }
    return out;
}

static PyObject*
dump_object(DumpState *self, PyObject *obj) {
    bool ok = false;
    PyObject *dict = NULL, *out = NULL;

    if (Py_EnterRecursiveCall(" while serializing an object")) return NULL;

    out = PyDict_New();
    Py_BEGIN_CRITICAL_SECTION(obj);
    if (out == NULL) goto cleanup;

    /* First encode everything in `__dict__` */
    dict = PyObject_GenericGetDict(obj, NULL);
    if (MS_UNLIKELY(dict == NULL)) {
        PyErr_Clear();
    }
    else {
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        int err = 0;
        Py_BEGIN_CRITICAL_SECTION(dict);
        while (PyDict_Next(dict, &pos, &key, &val)) {
            if (MS_LIKELY(PyUnicode_CheckExact(key))) {
                Py_ssize_t key_len;
                if (MS_UNLIKELY(val == UNSET)) continue;
                const char* key_buf = unicode_str_and_size(key, &key_len);
                if (MS_UNLIKELY(key_buf == NULL)) {
                    err = 1;
                    break;
                }
                if (MS_UNLIKELY(*key_buf == '_')) continue;

                PyObject *val2 = dump_obj(self, val, false);
                if (val2 == NULL) {
                    err = 1;
                    break;
                }
                int status = PyDict_SetItem(out, key, val2);
                Py_DECREF(val2);
                if (status < 0) {
                    err = 1;
                    break;
                }
            }
        }
        Py_END_CRITICAL_SECTION();
        if (MS_UNLIKELY(err)) goto cleanup;
    }
    /* Then encode everything in slots */
    PyTypeObject *type = Py_TYPE(obj);
    while (type != NULL) {
        Py_ssize_t n = Py_SIZE(type);
        if (n) {
            PyMemberDef *mp = MS_PyHeapType_GET_MEMBERS((PyHeapTypeObject *)type);
            for (Py_ssize_t i = 0; i < n; i++, mp++) {
                if (MS_LIKELY(mp->type == T_OBJECT_EX && !(mp->flags & READONLY))) {
                    char *addr = (char *)obj + mp->offset;
                    PyObject *val = *(PyObject **)addr;
                    if (MS_UNLIKELY(val == UNSET)) continue;
                    if (MS_UNLIKELY(val == NULL)) continue;
                    if (MS_UNLIKELY(*mp->name == '_')) continue;

                    PyObject *key = PyUnicode_InternFromString(mp->name);
                    if (key == NULL) goto cleanup;

                    int status = -1;
                    PyObject *val2 = dump_obj(self, val, false);
                    if (val2 != NULL) {
                        status = PyDict_SetItem(out, key, val2);
                        Py_DECREF(val2);
                    }
                    Py_DECREF(key);
                    if (status < 0) goto cleanup;
                }
            }
        }
        type = type->tp_base;
    }
    ok = true;

cleanup:
    Py_XDECREF(dict);
    Py_END_CRITICAL_SECTION();
    Py_LeaveRecursiveCall();
    if (!ok) {
        Py_CLEAR(out);
    }
    return out;
}

static PyObject *
dump_obj(DumpState *self, PyObject *obj, bool is_key) {
    PyTypeObject *type = Py_TYPE(obj);

    if (
        obj == Py_None ||
        type == &PyBool_Type ||
        type == &PyLong_Type ||
        type == &PyFloat_Type ||
        PyUnicode_Check(obj)
    ) {
        goto builtin;
    }
    else if (type == &PyBytes_Type) {
        if (self->builtin_types & MS_BUILTIN_BYTES) goto builtin;
        return dump_binary(
            self, PyBytes_AS_STRING(obj), PyBytes_GET_SIZE(obj)
        );
    }
    else if (type == &PyByteArray_Type) {
        if (self->builtin_types & MS_BUILTIN_BYTEARRAY) goto builtin;
        return dump_binary(
            self, PyByteArray_AS_STRING(obj), PyByteArray_GET_SIZE(obj)
        );
    }
    else if (type == &PyMemoryView_Type) {
        if (self->builtin_types & MS_BUILTIN_MEMORYVIEW) goto builtin;
        PyObject *out;
        Py_buffer buffer;
        if (PyObject_GetBuffer(obj, &buffer, PyBUF_CONTIG_RO) < 0) return NULL;
        out = dump_binary(self, buffer.buf, buffer.len);
        PyBuffer_Release(&buffer);
        return out;
    }
    else if (type == PyDateTimeAPI->DateTimeType) {
        if (self->builtin_types & MS_BUILTIN_DATETIME) goto builtin;
        return dump_datetime(self, obj);
    }
    else if (type == PyDateTimeAPI->DateType) {
        if (self->builtin_types & MS_BUILTIN_DATE) goto builtin;
        return dump_date(self, obj);
    }
    else if (type == PyDateTimeAPI->TimeType) {
        if (self->builtin_types & MS_BUILTIN_TIME) goto builtin;
        return dump_time(self, obj);
    }
    else if (type == PyDateTimeAPI->DeltaType) {
        if (self->builtin_types & MS_BUILTIN_TIMEDELTA) goto builtin;
        return dump_timedelta(self, obj);
    }
    else if (type == (PyTypeObject *)(self->mod->DecimalType)) {
        if (self->builtin_types & MS_BUILTIN_DECIMAL) goto builtin;
        return dump_decimal(self, obj);
    }
    else if (PyList_Check(obj)) {
        return dump_list(self, obj);
    }
    else if (PyTuple_Check(obj)) {
        return dump_tuple(self, obj, is_key);
    }
    else if (PyDict_Check(obj)) {
        return dump_dict(self, obj);
    }
#if PY315_PLUS
    else if (PyFrozenDict_Check(obj)) {
        return dump_frozendict(self, obj);
    }
#endif
    else if (ms_is_struct_type(type)) {
        return dump_struct(self, obj, is_key);
    }
    else if (!ms_is_struct_inst(obj) && PyObject_HasAttrString(obj, "__struct_fields__")) {
        /* External struct type (e.g. msgspec) — use Python-level attribute access */
        return dump_external_struct(self, obj);
    }
    else if (PyType_IsSubtype(Py_TYPE(type), self->mod->EnumMetaType)) {
        if (self->builtin_types & MS_BUILTIN_ENUM) goto builtin;
        return dump_enum(self, obj);
    }
    else if (is_key & PyUnicode_Check(obj)) {
        return PyObject_Str(obj);
    }
    else if (PyType_IsSubtype(type, (PyTypeObject *)(self->mod->UUIDType))) {
        if (self->builtin_types & MS_BUILTIN_UUID) goto builtin;
        return dump_uuid(self, obj);
    }
    else if (PyAnySet_Check(obj)) {
        return dump_set(self, obj, is_key);
    }
    if (self->codecs != NULL) {
        PyObject *dump = codecs_lookup(self->codecs, type);
        if (dump != NULL) {
            PyObject *temp = PyObject_CallOneArg(dump, obj);
            if (temp == NULL) return NULL;
            PyObject *result = dump_obj(self, temp, is_key);
            Py_DECREF(temp);
            return result;
        }
    }
    if (PyObject_HasAttrString(obj, "struct_dump")) {
        /* Custom type — struct_dump() to a base type, then re-process */
        PyObject *dumped = PyObject_CallMethod(obj, "struct_dump", NULL);
        if (dumped == NULL) return NULL;
        PyObject *result = dump_obj(self, dumped, is_key);
        Py_DECREF(dumped);
        return result;
    }
    else if (PyObject_HasAttrString(obj, "model_dump")) {
        /* Pydantic BaseModel — model_dump() to dict, then process */
        PyObject *dict = PyObject_CallMethod(obj, "model_dump", NULL);
        if (dict == NULL) return NULL;
        PyObject *result = dump_dict(self, dict);
        Py_DECREF(dict);
        return result;
    }
    else if (!PyType_Check(obj) && type->tp_dict != NULL) {
        PyObject *fields = PyObject_GetAttr(obj, self->mod->str___dataclass_fields__);
        if (fields != NULL) {
            PyObject *out = dump_dataclass(self, obj, fields);
            Py_DECREF(fields);
            return out;
        }
        else {
            PyErr_Clear();
        }
        if (PyDict_Contains(type->tp_dict, self->mod->str___attrs_attrs__)) {
            return dump_object(self, obj);
        }
    }

    if (self->builtin_types_seq != NULL) {
        PyObject **items = PySequence_Fast_ITEMS(self->builtin_types_seq);
        Py_ssize_t size = PySequence_Fast_GET_SIZE(self->builtin_types_seq);
        for (Py_ssize_t i = 0; i < size; i++) {
            if (((PyObject *)type) == *(items + i)) goto builtin;
        }
    }

    ms_encode_err_type_unsupported(type);
    return NULL;

builtin:
    Py_INCREF(obj);
    return obj;
}

static int
ms_process_builtin_types(
    StructspecState *mod,
    PyObject *builtin_types,
    uint32_t *mask,
    PyObject **custom_types
) {
    if (builtin_types == NULL || builtin_types == Py_None) return 0;

    bool forward_builtins_seq = false;
    const char *invalid_type_err = "builtin_types must be an iterable of types";

    PyObject *seq = PySequence_Fast(builtin_types, invalid_type_err);
    if (seq == NULL) return -1;

    Py_ssize_t size = PySequence_Fast_GET_SIZE(seq);
    PyObject **items = PySequence_Fast_ITEMS(seq);

    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *type = *(items + i);
        if (type == (PyObject *)(&PyBytes_Type)) {
            *mask |= MS_BUILTIN_BYTES;
        }
        else if (type == (PyObject *)(&PyByteArray_Type)) {
            *mask |= MS_BUILTIN_BYTEARRAY;
        }
        else if (type == (PyObject *)(&PyMemoryView_Type)) {
            *mask |= MS_BUILTIN_MEMORYVIEW;
        }
#if PY315_PLUS
        else if (type == (PyObject *)(&PyFrozenDict_Type)) {
            *mask |= MS_BUILTIN_FROZENDICT;
        }
#endif
        else if (type == (PyObject *)(PyDateTimeAPI->DateTimeType)) {
            *mask |= MS_BUILTIN_DATETIME;
        }
        else if (type == (PyObject *)(PyDateTimeAPI->DateType)) {
            *mask |= MS_BUILTIN_DATE;
        }
        else if (type == (PyObject *)(PyDateTimeAPI->TimeType)) {
            *mask |= MS_BUILTIN_TIME;
        }
        else if (type == (PyObject *)(PyDateTimeAPI->DeltaType)) {
            *mask |= MS_BUILTIN_TIMEDELTA;
        }
        else if (type == mod->UUIDType) {
            *mask |= MS_BUILTIN_UUID;
        }
        else if (type == mod->DecimalType) {
            *mask |= MS_BUILTIN_DECIMAL;
        }
        else if (PyType_Check(type) && Py_TYPE(type) == mod->EnumMetaType) {
            *mask |= MS_BUILTIN_ENUM;
        }
        else if (!PyType_Check(type)) {
            PyErr_SetString(PyExc_TypeError, invalid_type_err);
            goto error;
        }
        else if (custom_types != NULL) {
            forward_builtins_seq = true;
        }
        else {
            PyErr_Format(PyExc_TypeError, "Cannot treat %R as a builtin type", type);
            goto error;
        }
    }

    if (forward_builtins_seq) {
        *custom_types = seq;
    }
    else {
        Py_DECREF(seq);
    }
    return 0;

error:
    Py_DECREF(seq);
    return -1;
}


static PyObject*
structtype_dump(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *obj = NULL, *builtin_types = NULL, *sort_keys = NULL;
    int str_keys = 0;
    DumpState state;

    char *kwlist[] = {"obj", "builtin_types", "str_keys", "sort_keys", NULL};

    /* Parse arguments */
    if (!PyArg_ParseTupleAndKeywords(
        args, kwargs, "O|$OpO", kwlist,
        &obj, &builtin_types, &str_keys, &sort_keys
    )) {
        return NULL;
    }

    state.mod = structtype_get_state(self);
    state.str_keys = str_keys;
    state.builtin_types = 0;
    state.builtin_types_seq = NULL;
    state.codecs = NULL;

    if (parse_sort_keys_arg(sort_keys, &state.sort_keys) < 0) return NULL;

    if (
        ms_process_builtin_types(
            state.mod,
            builtin_types,
            &(state.builtin_types),
            &(state.builtin_types_seq)
        ) < 0
    ) {
        return NULL;
    }

    PyObject *out = dump_obj(&state, obj, false);
    Py_XDECREF(state.builtin_types_seq);
    return out;
}

/*************************************************************************
 * validate                                                               *
 *************************************************************************/

typedef struct ValidateState {
    StructspecState *mod;
    uint32_t builtin_types;
    bool str_keys;
    bool from_attributes;
    bool strict;
} ValidateState;

static PyObject * validate_obj(ValidateState *, PyObject *, TypeNode *, PathNode *);

static PyObject *
validate_int_uncommon(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (!self->strict) {
        uint64_t ux;
        bool neg, overflow;
        overflow = fast_long_extract_parts(obj, &neg, &ux);

        if ((type->types & MS_TYPE_BOOL) && !overflow && !neg) {
            if (ux == 0) {
                Py_RETURN_FALSE;
            }
            else if (ux == 1) {
                Py_RETURN_TRUE;
            }
        }
        if (type->types & (MS_TYPE_DATETIME | MS_TYPE_TIMEDELTA)) {
            int64_t seconds;
            if (overflow || ux > LLONG_MAX) {
                seconds = LLONG_MAX;
            }
            else {
                seconds = ux;
                if (neg) {
                    seconds *= -1;
                }
            }
            if (type->types & MS_TYPE_DATETIME) {
                return datetime_from_epoch(seconds, 0, type, path);
            }
            return ms_decode_timedelta_from_int64(seconds, path);
        }
    }
    return ms_validation_error("int", type, path);
}

static PyObject *
validate_int(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (MS_LIKELY(type->types & MS_TYPE_INT)) {
        return ms_decode_pyint(obj, type, path);
    }
    else if (type->types & (MS_TYPE_INTENUM | MS_TYPE_INTLITERAL)) {
        return ms_decode_int_enum_or_literal_pyint(obj, type, path);
    }
    else if (type->types & MS_TYPE_FLOAT) {
        return ms_decode_float(PyLong_AsDouble(obj), type, path);
    }
    else if (
        type->types & MS_TYPE_DECIMAL
        && !(self->builtin_types & MS_BUILTIN_DECIMAL)
    ) {
        return ms_decode_decimal_from_pyobj(obj, path, self->mod);
    }
    return validate_int_uncommon(self, obj, type, path);
}

static PyObject *
validate_float(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_FLOAT) {
        Py_INCREF(obj);
        return ms_check_float_constraints(obj, type, path);
    }
    else if (
        type->types & MS_TYPE_DECIMAL
        && !(self->builtin_types & MS_BUILTIN_DECIMAL)
    ) {
        return ms_decode_decimal_from_float(
            PyFloat_AS_DOUBLE(obj), path, self->mod
        );
    }
    else if (!self->strict) {
        double val = PyFloat_AS_DOUBLE(obj);
        if (type->types & MS_TYPE_INT) {
            int64_t out;
            if (double_as_int64(val, &out)) {
                return ms_post_decode_int64(out, type, path, self->strict, false);
            }
        }
        if (type->types & MS_TYPE_DATETIME) {
            return ms_decode_datetime_from_float(val, type, path);
        }
        else if (type->types & MS_TYPE_TIMEDELTA) {
            return ms_decode_timedelta_from_float(val, path);
        }
    }
    return ms_validation_error("float", type, path);
}

static PyObject *
validate_bool(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_BOOL) {
        Py_INCREF(obj);
        return obj;
    }
    if (obj == Py_True && (type->types & MS_TYPE_BOOLLITERAL_TRUE)) {
        Py_INCREF(Py_True);
        return Py_True;
    }
    if (obj == Py_False && (type->types & MS_TYPE_BOOLLITERAL_FALSE)) {
        Py_INCREF(Py_False);
        return Py_False;
    }
    if (type->types & (MS_TYPE_BOOLLITERAL_TRUE | MS_TYPE_BOOLLITERAL_FALSE)) {
        ms_raise_validation_error(path, "Invalid enum value %R%U", obj);
        return NULL;
    }
    return ms_validation_error("bool", type, path);
}

static PyObject *
validate_none(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_NONE) {
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error("null", type, path);
}

static PyObject *
validate_str_uncommon(
    ValidateState *self, PyObject *obj, const char *view, Py_ssize_t size,
    bool is_key, TypeNode *type, PathNode *path
) {
    if (is_key && self->str_keys && (
            type->types & (
                MS_TYPE_INT | MS_TYPE_INTENUM | MS_TYPE_INTLITERAL |
                MS_TYPE_FLOAT | MS_TYPE_DECIMAL |
                ((!self->strict) * (MS_TYPE_DATETIME | MS_TYPE_TIMEDELTA))
            )
        )
    ) {
        PyObject *out;
        if (maybe_parse_number(view, size, type, path, self->strict, &out)) {
            return out;
        }
    }

    if (type->types & (MS_TYPE_ENUM | MS_TYPE_STRLITERAL)) {
        return ms_decode_str_enum_or_literal(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_DATETIME)
        && !(self->builtin_types & MS_BUILTIN_DATETIME)
    ) {
        return ms_decode_datetime_from_str(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_DATE)
        && !(self->builtin_types & MS_BUILTIN_DATE)
    ) {
        return ms_decode_date(view, size, path);
    }
    else if (
        (type->types & MS_TYPE_TIME)
        && !(self->builtin_types & MS_BUILTIN_TIME)
    ) {
        return ms_decode_time(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_TIMEDELTA)
        && !(self->builtin_types & MS_BUILTIN_TIMEDELTA)
    ) {
        return ms_decode_timedelta(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_UUID)
        && !(self->builtin_types & MS_BUILTIN_UUID)
    ) {
        return ms_decode_uuid_from_str(view, size, path);
    }
    else if (
        (type->types & MS_TYPE_DECIMAL)
        && !(self->builtin_types & MS_BUILTIN_DECIMAL)
    ) {
        return ms_decode_decimal_from_pystr(obj, path, self->mod);
    }
    else if (
        (type->types & MS_TYPE_BYTES)
        && !(self->builtin_types & MS_BUILTIN_BYTES)
    ) {
        return json_decode_binary(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_BYTEARRAY)
        && !(self->builtin_types & MS_BUILTIN_BYTEARRAY)
    ) {
        return json_decode_binary(view, size, type, path);
    }
    else if (
        (type->types & MS_TYPE_MEMORYVIEW)
        && !(self->builtin_types & MS_BUILTIN_MEMORYVIEW)
    ) {
        return json_decode_binary(view, size, type, path);
    }
    return ms_validation_error("str", type, path);
}

static PyObject *
validate_str(
    ValidateState *self, PyObject *obj,
    bool is_key, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_ANY | MS_TYPE_STR)) {
        Py_INCREF(obj);
        return ms_check_str_constraints(obj, type, path);
    }

    Py_ssize_t size;
    const char* view = unicode_str_and_size(obj, &size);
    if (view == NULL) return NULL;

    if (MS_UNLIKELY(!self->strict)) {
        bool invalid = false;
        PyObject *out = ms_decode_str_lax(view, size, type, path, &invalid);
        if (!invalid) return out;
    }
    return validate_str_uncommon(self, obj, view, size, is_key, type, path);
}

static PyObject *
validate_bytes(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY | MS_TYPE_MEMORYVIEW)) {
        if (!ms_passes_bytes_constraints(PyBytes_GET_SIZE(obj), type, path)) {
            return NULL;
        }
        if (type->types & MS_TYPE_BYTES) {
            return PyBytes_FromObject(obj);
        }
        else if (type->types & MS_TYPE_BYTEARRAY) {
            return PyByteArray_FromObject(obj);
        }
        else {
            return PyMemoryView_FromObject(obj);
        }
    }
    if (
        (type->types & MS_TYPE_UUID) &&
        !(self->builtin_types & MS_BUILTIN_UUID)
    ) {
        return ms_decode_uuid_from_bytes(
            PyBytes_AS_STRING(obj), PyBytes_GET_SIZE(obj), path
        );
    }
    return ms_validation_error("bytes", type, path);
}

static PyObject *
validate_bytearray(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY | MS_TYPE_MEMORYVIEW)) {
        if (!ms_passes_bytes_constraints(PyByteArray_GET_SIZE(obj), type, path)) {
            return NULL;
        }
        if (type->types & MS_TYPE_BYTEARRAY) {
            Py_INCREF(obj);
            return obj;
        }
        else if (type->types & MS_TYPE_BYTES) {
            return PyBytes_FromObject(obj);
        }
        else {
            return PyMemoryView_FromObject(obj);
        }
    }
    if (
        (type->types & MS_TYPE_UUID) &&
        !(self->builtin_types & MS_BUILTIN_UUID)
    ) {
        return ms_decode_uuid_from_bytes(
            PyByteArray_AS_STRING(obj), PyByteArray_GET_SIZE(obj), path
        );
    }
    return ms_validation_error("bytes", type, path);
}

static PyObject *
validate_memoryview(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & (MS_TYPE_BYTES | MS_TYPE_BYTEARRAY | MS_TYPE_MEMORYVIEW)) {
        Py_ssize_t len = PyMemoryView_GET_BUFFER(obj)->len;
        if (!ms_passes_bytes_constraints(len, type, path)) return NULL;
        if (type->types & MS_TYPE_MEMORYVIEW) {
            Py_INCREF(obj);
            return obj;
        }
        else if (type->types & MS_TYPE_BYTES) {
            return PyBytes_FromObject(obj);
        }
        else {
            return PyByteArray_FromObject(obj);
        }
    }
    if (
        (type->types & MS_TYPE_UUID) &&
        !(self->builtin_types & MS_BUILTIN_UUID)
    ) {
        Py_buffer buffer;
        if (PyObject_GetBuffer(obj, &buffer, PyBUF_CONTIG_RO) < 0) return NULL;
        PyObject *out = ms_decode_uuid_from_bytes(
            buffer.buf, buffer.len, path
        );
        PyBuffer_Release(&buffer);
        return out;
    }
    return ms_validation_error("bytes", type, path);
}

static PyObject *
validate_datetime(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_DATETIME) {
        PyObject *tz = MS_DATE_GET_TZINFO(obj);
        if (!ms_passes_tz_constraint(tz, type, path)) return NULL;
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error("datetime", type, path);
}

static PyObject *
validate_time(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_TIME) {
        PyObject *tz = MS_TIME_GET_TZINFO(obj);
        if (!ms_passes_tz_constraint(tz, type, path)) return NULL;
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error("time", type, path);
}

static PyObject *
validate_enum(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_ENUM) {
        StrLookup *lookup = TypeNode_get_str_enum_or_literal(type);

        /* Check that the type matches. Note that enums that are also int or
         * str subclasses will be handled by `validate_int`/`validate_str`, not
         * here */
        if (lookup->common.cls != NULL && Py_TYPE(obj) == (PyTypeObject *)(lookup->common.cls)) {
            Py_INCREF(obj);
            return obj;
        }
    }

    return ms_validation_error(Py_TYPE(obj)->tp_name, type, path);
}

static PyObject *
validate_decimal(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & MS_TYPE_DECIMAL) {
        Py_INCREF(obj);
        return obj;
    }
    else if (type->types & MS_TYPE_FLOAT) {
        PyObject *temp = PyNumber_Float(obj);
        if (temp == NULL) return NULL;
        PyObject *out = validate_float(self, temp, type, path);
        Py_DECREF(temp);
        return out;
    }
    return ms_validation_error("decimal", type, path);
}


static PyObject *
validate_immutable(
    ValidateState *self, uint64_t mask, const char *expected,
    PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types & mask) {
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error(expected, type, path);
}

static PyObject *
validate_raw(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (type->types == 0) {
        Py_INCREF(obj);
        return obj;
    }
    return ms_validation_error("raw", type, path);
}

static PyObject *
validate_seq_to_list(
    ValidateState *self, PyObject **items, Py_ssize_t size,
    TypeNode *item_type, PathNode *path
) {
    PyObject *out = PyList_New(size);
    if (out == NULL) return NULL;
    if (size == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PathNode item_path = {path, i};
        PyObject *val = validate_obj(self, items[i], item_type, &item_path);
        if (val == NULL) {
            Py_CLEAR(out);
            break;
        }
        PyList_SET_ITEM(out, i, val);
    }
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
validate_seq_to_set(
    ValidateState *self, PyObject **items, Py_ssize_t size,
    bool mutable, TypeNode *item_type, PathNode *path
) {
    PyObject *out = mutable ? PySet_New(NULL) : PyFrozenSet_New(NULL);
    if (out == NULL) return NULL;
    if (size == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PathNode item_path = {path, i};
        PyObject *val = validate_obj(self, items[i], item_type, &item_path);
        if (MS_UNLIKELY(val == NULL || PySet_Add(out, val) < 0)) {
            Py_XDECREF(val);
            Py_CLEAR(out);
            break;
        }
        Py_DECREF(val);
    }
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
validate_seq_to_vartuple(
    ValidateState *self, PyObject **items, Py_ssize_t size,
    TypeNode *item_type, PathNode *path
) {
    PyObject *out = PyTuple_New(size);
    if (out == NULL) return NULL;
    if (size == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PathNode item_path = {path, i};
        PyObject *val = validate_obj(self, items[i], item_type, &item_path);
        if (val == NULL) {
            Py_CLEAR(out);
            break;
        }
        PyTuple_SET_ITEM(out, i, val);
    }
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
validate_seq_to_fixtuple(
    ValidateState *self, PyObject **items, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    Py_ssize_t fixtuple_size, offset;
    TypeNode_get_fixtuple(type, &offset, &fixtuple_size);

    if (size != fixtuple_size) {
        /* tuple is the incorrect size, raise and return */
        ms_raise_validation_error(
            path,
            "Expected `array` of length %zd, got %zd%U",
            fixtuple_size,
            size
        );
        return NULL;
    }

    PyObject *out = PyTuple_New(size);
    if (out == NULL) return NULL;
    if (size == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }

    for (Py_ssize_t i = 0; i < fixtuple_size; i++) {
        PathNode item_path = {path, i};
        PyObject *val = validate_obj(
            self, items[i], type->details[offset + i].pointer, &item_path
        );
        if (MS_UNLIKELY(val == NULL)) {
            Py_CLEAR(out);
            break;
        }
        PyTuple_SET_ITEM(out, i, val);
    }
    Py_LeaveRecursiveCall();
    return out;
}

static PyObject *
validate_seq_to_namedtuple(
    ValidateState *self, PyObject **items, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    NamedTupleInfo *info = TypeNode_get_namedtuple_info(type);
    Py_ssize_t nfields = Py_SIZE(info);
    Py_ssize_t ndefaults = info->defaults == NULL ? 0 : PyTuple_GET_SIZE(info->defaults);
    Py_ssize_t nrequired = nfields - ndefaults;

    if (size < nrequired || nfields < size) {
        /* tuple is the incorrect size, raise and return */
        if (ndefaults == 0) {
            ms_raise_validation_error(
                path,
                "Expected `array` of length %zd, got %zd%U",
                nfields,
                size
            );
        }
        else {
            ms_raise_validation_error(
                path,
                "Expected `array` of length %zd to %zd, got %zd%U",
                nrequired,
                nfields,
                size
            );
        }
        return NULL;
    }
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyTypeObject *nt_type = (PyTypeObject *)(info->class);
    PyObject *out = nt_type->tp_alloc(nt_type, nfields);
    if (out == NULL) goto error;
    MS_TUPLE_RESET_HASH(out);
    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyTuple_SET_ITEM(out, i, NULL);
    }
    for (Py_ssize_t i = 0; i < size; i++) {
        PathNode item_path = {path, i};
        PyObject *item = validate_obj(self, items[i], info->types[i], &item_path);
        if (MS_UNLIKELY(item == NULL)) goto error;
        PyTuple_SET_ITEM(out, i, item);
    }
    for (Py_ssize_t i = size; i < nfields; i++) {
        PyObject *item = PyTuple_GET_ITEM(info->defaults, i - nrequired);
        Py_INCREF(item);
        PyTuple_SET_ITEM(out, i, item);
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static bool
validate_tag_matches(
    ValidateState *self, PyObject *tag, PyObject *expected_tag, PathNode *path
) {
    if (PyUnicode_CheckExact(expected_tag)) {
        if (!PyUnicode_CheckExact(tag)) goto wrong_type;
    }
    else if (!PyLong_CheckExact(tag)) {
        goto wrong_type;
    }
    int status = PyObject_RichCompareBool(tag, expected_tag, Py_EQ);
    if (status == 1) return true;
    if (status == 0) {
        ms_raise_validation_error(path, "Invalid value %R%U", tag);
    }
    return false;
wrong_type:
    ms_raise_validation_error(
        path,
        "Expected `%s`, got `%s`%U",
        (PyUnicode_CheckExact(expected_tag) ? "str" : "int"),
        Py_TYPE(tag)->tp_name
    );
    return false;
}

static StructInfo *
validate_lookup_tag(
    ValidateState *self, Lookup *lookup, PyObject *tag, PathNode *path
) {
    StructInfo *out = NULL;
    if (Lookup_IsStrLookup(lookup)) {
        if (!PyUnicode_CheckExact(tag)) goto wrong_type;
        Py_ssize_t size;
        const char *buf = unicode_str_and_size(tag, &size);
        if (buf == NULL) return NULL;
        out = (StructInfo *)StrLookup_Get((StrLookup *)lookup, buf, size);
    }
    else {
        if (!PyLong_CheckExact(tag)) goto wrong_type;
        uint64_t ux;
        bool neg, overflow;
        overflow = fast_long_extract_parts(tag, &neg, &ux);
        if (overflow) goto invalid_value;
        if (neg) {
            out = (StructInfo *)IntLookup_GetInt64((IntLookup *)lookup, -(int64_t)ux);
        }
        else {
            out = (StructInfo *)IntLookup_GetUInt64((IntLookup *)lookup, ux);
        }
    }
    if (out != NULL) return out;
invalid_value:
    ms_raise_validation_error(path, "Invalid value %R%U", tag);
    return NULL;
wrong_type:
    ms_raise_validation_error(
        path,
        "Expected `%s`, got `%s`%U",
        (Lookup_IsStrLookup(lookup) ? "str" : "int"),
        Py_TYPE(tag)->tp_name
    );
    return NULL;
}

static PyObject *
validate_seq_to_struct_array_inner(
    ValidateState *self, PyObject **items, Py_ssize_t size,
    bool tag_already_read, StructInfo *info, PathNode *path
) {
    StructMetaObject *st_type = info->class;
    PathNode item_path = {path, 0};
    bool tagged = st_type->struct_tag_value != NULL;
    Py_ssize_t nfields = PyTuple_GET_SIZE(st_type->struct_encode_fields);
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(st_type->struct_defaults);
    Py_ssize_t nrequired = tagged + nfields - st_type->n_trailing_defaults;
    Py_ssize_t npos = nfields - ndefaults;

    if (size < nrequired) {
        ms_raise_validation_error(
            path,
            "Expected `array` of at least length %zd, got %zd%U",
            nrequired,
            size
        );
        return NULL;
    }

    if (tagged) {
        if (!tag_already_read) {
            if (
                !validate_tag_matches(
                    self, items[item_path.index], st_type->struct_tag_value, &item_path
                )
            ) {
                return NULL;
            }
        }
        size--;
        item_path.index++;
    }

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyObject *out = Struct_alloc((PyTypeObject *)(st_type));
    if (out == NULL) goto error;

    bool is_gc = MS_TYPE_IS_GC(st_type);
    bool should_untrack = is_gc;

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *val;
        if (size > 0) {
            val = validate_obj(
                self, items[item_path.index], info->types[i], &item_path
            );
            if (MS_UNLIKELY(val == NULL)) goto error;
            size--;
            item_path.index++;
        }
        else {
            val = get_default(
                PyTuple_GET_ITEM(st_type->struct_defaults, i - npos)
            );
            if (val == NULL) goto error;
        }
        Struct_set_index(out, i, val);
        if (should_untrack) {
            should_untrack = !MS_MAYBE_TRACKED(val);
        }
    }
    if (MS_UNLIKELY(size > 0)) {
        if (MS_UNLIKELY(st_type->forbid_unknown_fields == OPT_TRUE)) {
            ms_raise_validation_error(
                path,
                "Expected `array` of at most length %zd, got %zd%U",
                nfields,
                nfields + size
            );
            goto error;
        }
    }
    if (Struct_decode_post_init(st_type, out, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    if (is_gc && should_untrack && MS_IS_TRACKED(out))
        PyObject_GC_UnTrack(out);
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
validate_seq_to_struct_array(
    ValidateState *self, PyObject **items, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    return validate_seq_to_struct_array_inner(
        self, items, size, false, TypeNode_get_struct_info(type), path
    );
}

static PyObject *
validate_seq_to_struct_array_union(
    ValidateState *self, PyObject **items, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    Lookup *lookup = TypeNode_get_struct_union(type);
    if (size == 0) {
        return ms_error_with_path(
            "Expected `array` of at least length 1, got 0%U", path
        );
    }

    PathNode tag_path = {path, 0};
    StructInfo *info = validate_lookup_tag(self, lookup, items[0], &tag_path);
    if (info == NULL) return NULL;
    return validate_seq_to_struct_array_inner(self, items, size, true, info, path);
}

static PyObject *
validate_seq(
    ValidateState *self, PyObject **items, Py_ssize_t size,
    TypeNode *type, PathNode *path
) {
    if (!ms_passes_array_constraints(size, type, path)) return NULL;

    if (type->types & MS_TYPE_LIST) {
        return validate_seq_to_list(
            self, items, size, TypeNode_get_array(type), path
        );
    }
    else if (type->types & (MS_TYPE_SET | MS_TYPE_FROZENSET)) {
        return validate_seq_to_set(
            self, items, size,
            (type->types & MS_TYPE_SET), TypeNode_get_array(type), path
        );
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        return validate_seq_to_vartuple(
            self, items, size, TypeNode_get_array(type), path
        );
    }
    else if (type->types & MS_TYPE_FIXTUPLE) {
        return validate_seq_to_fixtuple(self, items, size, type, path);
    }
    else if (type->types & MS_TYPE_NAMEDTUPLE) {
        return validate_seq_to_namedtuple(self, items, size, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY) {
        return validate_seq_to_struct_array(self, items, size, type, path);
    }
    else if (type->types & MS_TYPE_STRUCT_ARRAY_UNION) {
        return validate_seq_to_struct_array_union(self, items, size, type, path);
    }
    return ms_validation_error("array", type, path);
}

static PyObject *
validate_any_set(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    PyObject *seq = PySequence_Tuple(obj);
    if (seq == NULL) return NULL;

    PyObject **items = PySequence_Fast_ITEMS(seq);
    Py_ssize_t size = PySequence_Fast_GET_SIZE(seq);

    PyObject *out = NULL;

    if (!ms_passes_array_constraints(size, type, path)) goto done;

    if (type->types & MS_TYPE_LIST) {
        out = validate_seq_to_list(
            self, items, size, TypeNode_get_array(type), path
        );
    }
    else if (type->types & (MS_TYPE_SET | MS_TYPE_FROZENSET)) {
        out = validate_seq_to_set(
            self, items, size,
            (type->types & MS_TYPE_SET),
            TypeNode_get_array(type), path
        );
    }
    else if (type->types & MS_TYPE_VARTUPLE) {
        out = validate_seq_to_vartuple(
            self, items, size, TypeNode_get_array(type), path
        );
    }
    else {
        ms_validation_error("set", type, path);
    }

done:
    Py_DECREF(seq);
    return out;
}

static PyObject *
validate_dict_to_dict(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    Py_ssize_t size = PyDict_GET_SIZE(obj);
    if (!ms_passes_map_constraints(size, type, path)) return NULL;
    TypeNode *key_type, *val_type;
    TypeNode_get_dict(type, &key_type, &val_type);

    PathNode key_path = {path, PATH_KEY, NULL};
    PathNode val_path = {path, PATH_STR_BRACKET, NULL};

    PyObject *out = PyDict_New();
    if (out == NULL) return NULL;
    if (PyDict_GET_SIZE(obj) == 0) return out;

    if (Py_EnterRecursiveCall(" while deserializing an object")) {
        Py_DECREF(out);
        return NULL; /* cpylint-ignore */
    }
    PyObject *key_obj = NULL, *val_obj = NULL;
    Py_ssize_t pos = 0;
    while (PyDict_Next(obj, &pos, &key_obj, &val_obj)) {
        PyObject *key;
        if (PyUnicode_CheckExact(key_obj)) {
            key = validate_str(self, key_obj, true, key_type, &key_path);
        }
        else {
            key = validate_obj(self, key_obj, key_type, &key_path);
        }
        if (MS_UNLIKELY(key == NULL)) goto error;
        val_path.object = key_obj;
        PyObject *val = validate_obj(self, val_obj, val_type, &val_path);
        if (MS_UNLIKELY(val == NULL)) {
            Py_DECREF(key);
            goto error;
        }
        int status = PyDict_SetItem(out, key, val);
        Py_DECREF(key);
        Py_DECREF(val);
        if (status < 0) goto error;
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_DECREF(out);
    return NULL;
}

#if PY315_PLUS
static PyObject *
validate_dict_to_frozendict(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    PyObject *dict = validate_dict_to_dict(self, obj, type, path);
    return _PyFrozenDict_NewSteal(dict);
}
#endif

static PyObject *
validate_mapping_to_dict(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    PyObject *out = NULL;
    PyObject *temp = PyDict_New();
    if (temp == NULL) return NULL;
    if (PyDict_Merge(temp, obj, 1) == 0) {
        out = validate_dict_to_dict(self, temp, type, path);
    }
    Py_DECREF(temp);
    return out;
}

static bool
validate_is_str_key(PyObject *key, PathNode *path) {
    if (PyUnicode_CheckExact(key)) return true;
    PathNode key_path = {path, PATH_KEY, NULL};
    ms_error_with_path("Expected `str`%U", &key_path);
    return false;
}


static PyObject *
validate_dict_to_struct(
    ValidateState *self, PyObject *obj, StructInfo *info, PathNode *path,
    bool tag_already_read
) {
    StructMetaObject *struct_type = info->class;

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyObject *out = Struct_alloc((PyTypeObject *)(struct_type));
    if (out == NULL) goto error;

    Py_ssize_t pos = 0, pos_obj = 0;
    PyObject *key_obj, *val_obj;
    while (PyDict_Next(obj, &pos_obj, &key_obj, &val_obj)) {
        if (!validate_is_str_key(key_obj, path)) goto error;

        Py_ssize_t key_size;
        const char *key = unicode_str_and_size(key_obj, &key_size);
        if (key == NULL) goto error;

        Py_ssize_t field_index = StructMeta_get_field_index(struct_type, key, key_size, &pos);
        if (field_index < 0) {
            if (MS_UNLIKELY(field_index == -2)) {
                if (tag_already_read) continue;
                PathNode tag_path = {path, PATH_STR, struct_type->struct_tag_field};
                if (
                    !validate_tag_matches(
                        self, val_obj, struct_type->struct_tag_value, &tag_path
                    )
                ) {
                    goto error;
                }
            }
            else {
                /* Unknown field */
                if (MS_UNLIKELY(struct_type->forbid_unknown_fields == OPT_TRUE)) {
                    ms_error_unknown_field(key, key_size, path);
                    goto error;
                }
            }
        }
        else {
            PathNode field_path = {path, field_index, (PyObject *)struct_type};
            PyObject *val = validate_obj(
                self, val_obj, info->types[field_index], &field_path
            );
            if (val == NULL) goto error;
            Struct_set_index(out, field_index, val);
        }
    }

    if (Struct_fill_in_defaults(struct_type, out, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
validate_dict_to_struct_union(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    Lookup *lookup = TypeNode_get_struct_union(type);
    PyObject *tag_field = Lookup_tag_field(lookup);
    PyObject *value = PyDict_GetItem(obj, tag_field);
    if (value != NULL) {
        PathNode tag_path = {path, PATH_STR, tag_field};
        StructInfo *info = validate_lookup_tag(
            self, lookup, value, &tag_path
        );
        if (info == NULL) return NULL;
        return validate_dict_to_struct(self, obj, info, path, true);
    }
    ms_missing_required_field(tag_field, path);
    return NULL;
}

static PyObject *
validate_dict_to_typeddict(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyObject *out = PyDict_New();
    if (out == NULL) goto error;

    TypedDictInfo *info = TypeNode_get_typeddict_info(type);
    Py_ssize_t nrequired = 0, pos = 0, pos_obj = 0;
    PyObject *key_obj, *val_obj;
    while (PyDict_Next(obj, &pos_obj, &key_obj, &val_obj)) {
        if (!validate_is_str_key(key_obj, path)) goto error;

        Py_ssize_t key_size;
        const char *key = unicode_str_and_size(key_obj, &key_size);
        if (key == NULL) goto error;

        TypeNode *field_type;
        PyObject *field = TypedDictInfo_lookup_key(
            info, key, key_size, &field_type, &pos
        );
        if (field != NULL) {
            if (field_type->types & MS_EXTRA_FLAG) nrequired++;
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = validate_obj(self, val_obj, field_type, &field_path);
            if (val == NULL) goto error;
            int status = PyDict_SetItem(out, field, val);
            Py_DECREF(val);
            if (status < 0) goto error;
        }
    }
    if (nrequired < info->nrequired) {
        /* A required field is missing, determine which one and raise */
        TypedDictInfo_error_missing(info, out, path);
        goto error;
    }
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
validate_dict_to_dataclass(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    DataclassInfo *info = TypeNode_get_dataclass_info(type);

    PyTypeObject *dataclass_type = (PyTypeObject *)(info->class);
    PyObject *out = dataclass_type->tp_alloc(dataclass_type, 0);
    if (out == NULL) goto error;
    if (info->pre_init != NULL) {
        PyObject *res = PyObject_CallOneArg(info->pre_init, out);
        if (res == NULL) goto error;
        Py_DECREF(res);
    }

    Py_ssize_t pos = 0, pos_obj = 0;
    PyObject *key_obj = NULL, *val_obj = NULL;
    while (PyDict_Next(obj, &pos_obj, &key_obj, &val_obj)) {
        if (!validate_is_str_key(key_obj, path)) goto error;
        Py_ssize_t key_size;
        const char *key = unicode_str_and_size(key_obj, &key_size);
        if (MS_UNLIKELY(key == NULL)) goto error;

        TypeNode *field_type;
        PyObject *field = DataclassInfo_lookup_key(
            info, key, key_size, &field_type, &pos
        );
        if (field != NULL) {
            PathNode field_path = {path, PATH_STR, field};
            PyObject *val = validate_obj(self, val_obj, field_type, &field_path);
            if (val == NULL) goto error;
            int status = PyObject_GenericSetAttr(out, field, val);
            Py_DECREF(val);
            if (status < 0) goto error;
        }
    }
    if (DataclassInfo_post_decode(info, out, path) < 0) goto error;
    Py_LeaveRecursiveCall();
    return out;
error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
validate_dict(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    PyObject *res = NULL;
    Py_BEGIN_CRITICAL_SECTION(obj);
    if (type->types & MS_TYPE_DICT) {
        res = validate_dict_to_dict(self, obj, type, path);
    }
#if PY315_PLUS
    else if (type->types & MS_TYPE_FROZENDICT) {
        res = validate_dict_to_frozendict(self, obj, type, path);
    }
#endif
    else if (type->types & MS_TYPE_STRUCT) {
        StructInfo *info = TypeNode_get_struct_info(type);
        res = validate_dict_to_struct(self, obj, info, path, false);
    }
    else if (type->types & MS_TYPE_STRUCT_UNION) {
        res = validate_dict_to_struct_union(self, obj, type, path);
    }
    else if (type->types & MS_TYPE_TYPEDDICT) {
        res = validate_dict_to_typeddict(self, obj, type, path);
    }
    else if (type->types & MS_TYPE_DATACLASS) {
        res = validate_dict_to_dataclass(self, obj, type, path);
    } else {
        res = ms_validation_error("object", type, path);
    }
    Py_END_CRITICAL_SECTION();
    return res;
}

static PyObject *
validate_object_to_struct(
    ValidateState *self, PyObject *obj, StructInfo *info, PathNode *path,
    PyObject* (*getter)(PyObject *, PyObject *), bool tag_already_read
) {
    StructMetaObject *struct_type = info->class;

    Py_ssize_t nfields = PyTuple_GET_SIZE(struct_type->struct_encode_fields);
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(struct_type->struct_defaults);

    if (struct_type->struct_tag_value != NULL && !tag_already_read) {
        PyObject *attr = getter(obj, struct_type->struct_tag_field);
        if (attr != NULL) {
            PathNode tag_path = {path, PATH_STR, struct_type->struct_tag_field};
            bool ok = validate_tag_matches(
                self, attr, struct_type->struct_tag_value, &tag_path
            );
            Py_DECREF(attr);
            if (!ok) return NULL;
        }
        else {
            /* Tag not present, ignore and continue */
            PyErr_Clear();
        }
    }

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyObject *out = Struct_alloc((PyTypeObject *)(struct_type));
    if (out == NULL) goto error;

    bool is_gc = MS_TYPE_IS_GC(struct_type);
    bool should_untrack = is_gc;

    /* If no fields are renamed we only have one fields tuple to choose from */
    PyObject *fields = NULL;
    if (struct_type->struct_fields == struct_type->struct_encode_fields) {
        fields = struct_type->struct_fields;
    }

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field, *attr, *val;

        if (MS_LIKELY(fields != NULL)) {
            /* fields tuple already determined, just get the next field name */
            field = PyTuple_GET_ITEM(fields, i);
            attr = getter(obj, field);
        }
        else {
            /* fields tuple undetermined. Try the attribute name first */
            PyObject *encode_field;
            field = PyTuple_GET_ITEM(struct_type->struct_fields, i);
            encode_field = PyTuple_GET_ITEM(struct_type->struct_encode_fields, i);
            attr = getter(obj, field);
            if (field != encode_field) {
                /* The field _was_ renamed */
                if (attr != NULL) {
                    /* Got a match, lock-in using attribute names */
                    fields = struct_type->struct_fields;
                }
                else {
                    /* No match. Try using the renamed name */
                    PyErr_Clear();
                    attr = getter(obj, encode_field);
                    if (attr != NULL) {
                        /* Got a match, lock-in using renamed names */
                        field = encode_field;
                        fields = struct_type->struct_encode_fields;
                    }
                }
            }
        }

        if (attr != NULL) {
            PathNode field_path = {path, PATH_STR, field};
            val = validate_obj(self, attr, info->types[i], &field_path);
            Py_DECREF(attr);
        }
        else {
            PyErr_Clear();
            PyObject *default_val = NULL;
            if (MS_LIKELY(i >= (nfields - ndefaults))) {
                default_val = PyTuple_GET_ITEM(
                    struct_type->struct_defaults, i - (nfields - ndefaults)
                );
                if (MS_UNLIKELY(default_val == NODEFAULT)) {
                    default_val = NULL;
                }
            }
            if (default_val == NULL) {
                ms_missing_required_field(field, path);
                goto error;
            }
            val = get_default(default_val);
        }
        if (val == NULL) goto error;
        Struct_set_index(out, i, val);
        if (should_untrack) {
            should_untrack = !MS_MAYBE_TRACKED(val);
        }
    }

    if (Struct_decode_post_init(struct_type, out, path) < 0) goto error;

    Py_LeaveRecursiveCall();
    if (is_gc && should_untrack && MS_IS_TRACKED(out))
        PyObject_GC_UnTrack(out);
    return out;

error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static PyObject *
validate_object_to_struct_union(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path,
    PyObject* (*getter)(PyObject *, PyObject *)
) {
    Lookup *lookup = TypeNode_get_struct_union(type);
    PyObject *tag_field = Lookup_tag_field(lookup);
    PyObject *value = getter(obj, tag_field);
    if (value != NULL) {
        PathNode tag_path = {path, PATH_STR, tag_field};
        StructInfo *info = validate_lookup_tag(
            self, lookup, value, &tag_path
        );
        Py_DECREF(value);
        if (info == NULL) return NULL;
        return validate_object_to_struct(self, obj, info, path, getter, true);
    }
    ms_missing_required_field(tag_field, path);
    return NULL;
}

static PyObject *
validate_object_to_dataclass(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path,
    PyObject* (*getter)(PyObject *, PyObject *)
) {
    DataclassInfo *info = TypeNode_get_dataclass_info(type);

    Py_ssize_t nfields = Py_SIZE(info);
    Py_ssize_t ndefaults = PyTuple_GET_SIZE(info->defaults);

    if (Py_EnterRecursiveCall(" while deserializing an object")) return NULL;

    PyTypeObject *dataclass_type = (PyTypeObject *)(info->class);
    PyObject *out = dataclass_type->tp_alloc(dataclass_type, 0);
    if (out == NULL) goto error;
    if (info->pre_init != NULL) {
        PyObject *res = PyObject_CallOneArg(info->pre_init, out);
        if (res == NULL) goto error;
        Py_DECREF(res);
    }

    for (Py_ssize_t i = 0; i < nfields; i++) {
        PyObject *field = info->fields[i].key;
        PyObject *attr = getter(obj, field);
        PyObject *val;
        if (attr == NULL) {
            PyErr_Clear();
            if (MS_LIKELY(i >= (nfields - ndefaults))) {
                PyObject *default_val = PyTuple_GET_ITEM(
                    info->defaults, i - (nfields - ndefaults)
                );
                bool is_factory = info->fields[i].type->types & MS_EXTRA_FLAG;
                if (is_factory) {
                    val = PyObject_CallNoArgs(default_val);
                }
                else {
                    Py_INCREF(default_val);
                    val = default_val;
                }
            }
            else {
                ms_missing_required_field(field, path);
                goto error;
            }
        }
        else {
            PathNode field_path = {path, PATH_STR, field};
            val = validate_obj(self, attr, info->fields[i].type, &field_path);
            Py_DECREF(attr);
        }
        if (val == NULL) goto error;
        int status = PyObject_GenericSetAttr(out, field, val);
        Py_DECREF(val);
        if (status < 0) goto error;
    }
    if (info->post_init != NULL) {
        PyObject *res = PyObject_CallOneArg(info->post_init, out);
        if (res == NULL) {
            ms_maybe_wrap_validation_error(path);
            goto error;
        }
        Py_DECREF(res);
    }
    Py_LeaveRecursiveCall();
    return out;

error:
    Py_LeaveRecursiveCall();
    Py_XDECREF(out);
    return NULL;
}

static bool
Lookup_union_contains_type(Lookup *lookup, PyTypeObject *cls) {
    if (Lookup_IsStrLookup(lookup)) {
        StrLookup *lk = (StrLookup *)lookup;
        for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
            StructInfo *info = (StructInfo *)(lk->table[i].value);
            if (info != NULL && ((PyTypeObject *)(info->class) == cls)) {
                return true;
            }
        }
    }
    else {
        if (((IntLookup *)lookup)->compact) {
            IntLookupCompact *lk = (IntLookupCompact *)lookup;
            for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
                StructInfo *info = (StructInfo *)(lk->table[i]);
                if (info != NULL && ((PyTypeObject *)(info->class) == cls)) {
                    return true;
                }
            }
        }
        else {
            IntLookupHashmap *lk = (IntLookupHashmap *)lookup;
            for (Py_ssize_t i = 0; i < Py_SIZE(lk); i++) {
                StructInfo *info = (StructInfo *)(lk->table[i].value);
                if (info != NULL && ((PyTypeObject *)(info->class) == cls)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static PyObject *
getattr_then_getitem(PyObject *obj, PyObject *key) {
    PyObject *out = PyObject_GetAttr(obj, key);
    if (out == NULL) {
        PyErr_Clear();
        out = PyObject_GetItem(obj, key);
    }
    return out;
}

static PyObject *
validate_other(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    PyTypeObject *pytype = Py_TYPE(obj);

    /* First check if instance matches requested type for builtin user-defined
     * collection types. */
    if (type->types & (MS_TYPE_STRUCT | MS_TYPE_STRUCT_ARRAY)) {
        StructInfo *info = TypeNode_get_struct_info(type);
        if (pytype == (PyTypeObject *)(info->class)) {
            Py_INCREF(obj);
            return obj;
        }
    }
    else if (type->types & (MS_TYPE_STRUCT_UNION | MS_TYPE_STRUCT_ARRAY_UNION)) {
        Lookup *lookup = TypeNode_get_struct_union(type);
        if (Lookup_union_contains_type(lookup, pytype)) {
            Py_INCREF(obj);
            return obj;
        }
    }
    else if (type->types & MS_TYPE_DATACLASS) {
        DataclassInfo *info = TypeNode_get_dataclass_info(type);
        if (pytype == (PyTypeObject *)(info->class)) {
            Py_INCREF(obj);
            return obj;
        }
    }
    else if (type->types & MS_TYPE_NAMEDTUPLE) {
        NamedTupleInfo *info = TypeNode_get_namedtuple_info(type);
        if (pytype == (PyTypeObject *)(info->class)) {
            Py_INCREF(obj);
            return obj;
        }
    }

    /* No luck. Next check if it's a tuple subclass (standard tuples are
     * handled earlier), and if so try converting it as a sequence */
    if (PyTuple_Check(obj)) {
        return validate_seq(self, TUPLE_ITEMS(obj), PyTuple_GET_SIZE(obj), type, path);
    }

    /* Next try converting from a mapping or by attribute */
    bool is_mapping = PyMapping_Check(obj);
    if (is_mapping) {
        if (type->types & MS_TYPE_DICT) {
            return validate_mapping_to_dict(self, obj, type, path);
        }
#if PY315_PLUS
        if (type->types & MS_TYPE_FROZENDICT) {
            PyObject *dict = validate_mapping_to_dict(self, obj, type, path);
            return _PyFrozenDict_NewSteal(dict);
        }
#endif
    }

    if (is_mapping || self->from_attributes) {
        PyObject* (*getter)(PyObject *, PyObject *);
        /* We want to exclude array_like structs when converting from a
         * mapping, but include them when converting by attribute */
        bool matches_struct, matches_struct_union;
        if (self->from_attributes) {
            getter = (is_mapping) ? getattr_then_getitem : PyObject_GetAttr;
            matches_struct = type->types & (MS_TYPE_STRUCT | MS_TYPE_STRUCT_ARRAY);
            matches_struct_union = type->types & (MS_TYPE_STRUCT_UNION | MS_TYPE_STRUCT_ARRAY_UNION);
        }
        else {
            getter = getattr_then_getitem;
            matches_struct = type->types & MS_TYPE_STRUCT;
            matches_struct_union = type->types & MS_TYPE_STRUCT_UNION;
        }

        if (matches_struct) {
            StructInfo *info = TypeNode_get_struct_info(type);
            return validate_object_to_struct(self, obj, info, path, getter, false);
        }
        else if (matches_struct_union) {
            return validate_object_to_struct_union(self, obj, type, path, getter);
        }
        else if (type->types & MS_TYPE_DATACLASS) {
            return validate_object_to_dataclass(self, obj, type, path, getter);
        }
    }

    return ms_validation_error(Py_TYPE(obj)->tp_name, type, path);
}

static PyObject *
validate_obj(
    ValidateState *self, PyObject *obj, TypeNode *type, PathNode *path
) {
    if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC | MS_TYPE_ANY))) {
        Py_INCREF(obj);
        if (MS_UNLIKELY(type->types & (MS_TYPE_CUSTOM | MS_TYPE_CUSTOM_GENERIC))) {
            return ms_decode_custom(obj, type, path);
        }
        return obj;
    }

    PyTypeObject *pytype = Py_TYPE(obj);
    if (PyUnicode_Check(obj)) {
        return validate_str(self, obj, false, type, path);
    }
    else if (pytype == &PyBool_Type) {
        return validate_bool(self, obj, type, path);
    }
    else if (PyLong_Check(obj)) {
        return validate_int(self, obj, type, path);
    }
    else if (pytype == &PyFloat_Type) {
        return validate_float(self, obj, type, path);
    }
    else if (PyList_Check(obj)) {
        return validate_seq(self, LIST_ITEMS(obj), PyList_GET_SIZE(obj), type, path);
    }
    else if (pytype == &PyTuple_Type) {
        /* Tuple subclasses are handled later on */
        return validate_seq(self, TUPLE_ITEMS(obj), PyTuple_GET_SIZE(obj), type, path);
    }
    else if (PyDict_Check(obj)) {
        return validate_dict(self, obj, type, path);
    }
#if PY315_PLUS
    else if (PyFrozenDict_Check(obj)) {
        return validate_dict(self, obj, type, path);
    }
#endif
    else if (obj == Py_None) {
        return validate_none(self, obj, type, path);
    }
    else if (PyBytes_Check(obj)) {
        return validate_bytes(self, obj, type, path);
    }
    else if (pytype == &PyByteArray_Type) {
        return validate_bytearray(self, obj, type, path);
    }
    else if (pytype == &PyMemoryView_Type) {
        return validate_memoryview(self, obj, type, path);
    }
    else if (pytype == PyDateTimeAPI->DateTimeType) {
        return validate_datetime(self, obj, type, path);
    }
    else if (pytype == PyDateTimeAPI->TimeType) {
        return validate_time(self, obj, type, path);
    }
    else if (pytype == PyDateTimeAPI->DateType) {
        return validate_immutable(self, MS_TYPE_DATE, "date", obj, type, path);
    }
    else if (pytype == PyDateTimeAPI->DeltaType) {
        return validate_immutable(self, MS_TYPE_TIMEDELTA, "duration", obj, type, path);
    }
    else if (PyType_IsSubtype(pytype, (PyTypeObject *)(self->mod->UUIDType))) {
        return validate_immutable(self, MS_TYPE_UUID, "uuid", obj, type, path);
    }
    else if (pytype == (PyTypeObject *)self->mod->DecimalType) {
        return validate_decimal(self, obj, type, path);
    }
    else if (PyType_IsSubtype(Py_TYPE(pytype), self->mod->EnumMetaType)) {
        return validate_enum(self, obj, type, path);
    }
    else if (pytype == &Raw_Type) {
        return validate_raw(self, obj, type, path);
    }
    else if (PyAnySet_Check(obj)) {
        return validate_any_set(self, obj, type, path);
    }
    else {
        return validate_other(self, obj, type, path);
    }
}

static PyObject*
structtype_validate(PyObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *obj = NULL, *pytype = NULL, *builtin_types = NULL;
    int str_keys = false, strict = true, from_attributes = false;
    ValidateState state;

    char *kwlist[] = {
        "obj", "type", "strict", "from_attributes", "builtin_types",
        "str_keys", NULL
    };

    /* Parse arguments */
    if (!PyArg_ParseTupleAndKeywords(
        args, kwargs, "OO|$ppOp", kwlist,
        &obj, &pytype, &strict, &from_attributes, &builtin_types, &str_keys
    )) {
        return NULL;
    }

    state.mod = structtype_get_state(self);
    state.builtin_types = 0;
    state.from_attributes = from_attributes;
    state.strict = strict;
    if (strict) {
        state.str_keys = str_keys;
        if (ms_process_builtin_types(state.mod, builtin_types, &(state.builtin_types), NULL) < 0) {
            return NULL;
        }
    }
    else {
        state.str_keys = true;
    }

    /* Avoid allocating a new TypeNode for struct types */
    if (ms_is_struct_cls(pytype)) {
        PyObject *info = StructInfo_Convert(pytype);
        if (info == NULL) return NULL;
        bool array_like = ((StructMetaObject *)pytype)->array_like == OPT_TRUE;
        TypeNodeSimple type;
        type.types = array_like ? MS_TYPE_STRUCT_ARRAY : MS_TYPE_STRUCT;
        type.details[0].pointer = info;
        PyObject *out = validate_obj(&state, obj, (TypeNode *)(&type), NULL);
        Py_DECREF(info);
        return out;
    }

    TypeNode *type = TypeNode_Convert(pytype);
    if (type == NULL) return NULL;
    PyObject *out = validate_obj(&state, obj, type, NULL);
    TypeNode_Free(type);
    return out;
}
/*************************************************************************
 * Struct Method Implementations                                         *
 *************************************************************************/

static PyObject *
Struct_dump_json(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    if (nargs > 0) {
        return PyErr_Format(PyExc_TypeError,
            "struct_dump_json() takes no positional arguments (%zd given)", nargs);
    }
    if (kwnames != NULL) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
            PyObject *name = PyTuple_GET_ITEM(kwnames, i);
            if (PyUnicode_CompareWithASCIIString(name, "enc_hook") == 0) {
                return PyErr_Format(PyExc_TypeError,
                    "`enc_hook` has been removed; use "
                    "`Annotated[T, Field(dump=...)]` or a `struct_dump` method "
                    "on the custom type instead");
            }
        }
    }
    PyObject *mod = ((StructMetaObject *)Py_TYPE(self))->struct_module;
    if (mod == NULL) {
        mod = PyState_FindModule(&structtypemodule);
    }
    if (mod == NULL) return NULL;
    Py_ssize_t nkwargs = (kwnames != NULL) ? PyTuple_GET_SIZE(kwnames) : 0;
    PyObject *stack_args[8], **new_args;
    if (nkwargs < 8) {
        new_args = stack_args;
    }
    else {
        new_args = PyMem_Malloc((1 + nkwargs) * sizeof(PyObject *));
        if (new_args == NULL) return PyErr_NoMemory();
    }
    new_args[0] = self;
    for (Py_ssize_t i = 0; i < nkwargs; i++) {
        new_args[1 + i] = args[nargs + i];
    }
    PyObject *result = structtype_json_encode(mod, new_args, 1, kwnames);
    if (new_args != stack_args) PyMem_Free(new_args);
    return result;
}

static PyObject *
Struct_validate_json(PyObject *cls, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    if (nargs != 1) {
        return PyErr_Format(PyExc_TypeError,
            "struct_validate_json() takes exactly 1 positional argument (%zd given)", nargs);
    }
    PyObject *mod = ((StructMetaObject *)cls)->struct_module;
    if (mod == NULL) {
        mod = PyState_FindModule(&structtypemodule);
    }
    if (mod == NULL) return NULL;
    StructspecState *state = structtype_get_state(mod);

    /* Parse the strict keyword argument directly, avoiding the
     * kwnames re-marshaling that a call through structtype_json_decode
     * would require. `type` is always cls. */
    PyObject *strict_obj = NULL;
    int strict = 1;
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        if ((strict_obj = find_keyword(kwnames, args + nargs, state->str_strict)) != NULL) nkwargs--;
        if (nkwargs > 0) {
            PyErr_SetString(
                PyExc_TypeError,
                "Extra keyword arguments provided"
            );
            return NULL;
        }
    }

    if (strict_obj != NULL) {
        strict = PyObject_IsTrue(strict_obj);
        if (strict < 0) return NULL;
    }

    return json_decode_common(state, args[0], cls, strict);
}

static PyObject *
Struct_dump(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    if (nargs > 0) {
        return PyErr_Format(PyExc_TypeError,
            "struct_dump() takes no positional arguments (%zd given)", nargs);
    }
    StructspecState *mod = NULL;
    PyObject *cached = ((StructMetaObject *)Py_TYPE(self))->struct_module;
    if (cached != NULL) {
        mod = structtype_get_state(cached);
    }
    if (mod == NULL) {
        mod = structtype_get_global_state();
    }
    if (mod == NULL) return NULL;
    PyObject *sort_keys = NULL, *builtin_types = NULL;
    int str_keys = 0;
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < nkwargs; i++) {
            PyObject *name = PyTuple_GET_ITEM(kwnames, i);
            PyObject *val = args[nargs + i];
            if (PyUnicode_CompareWithASCIIString(name, "sort_keys") == 0) {
                sort_keys = val;
            } else if (PyUnicode_CompareWithASCIIString(name, "str_keys") == 0) {
                str_keys = PyObject_IsTrue(val);
            } else if (PyUnicode_CompareWithASCIIString(name, "builtin_types") == 0) {
                builtin_types = val;
            } else {
                return PyErr_Format(PyExc_TypeError,
                    "struct_dump() got an unexpected keyword argument '%U'", name);
            }
        }
    }
    DumpState state;
    state.mod = mod;
    state.str_keys = str_keys;
    state.builtin_types = 0;
    state.builtin_types_seq = NULL;
    if (parse_sort_keys_arg(sort_keys, &state.sort_keys) < 0) return NULL;
    state.codecs = NULL;
    if (ms_process_builtin_types(mod, builtin_types,
            &(state.builtin_types), &(state.builtin_types_seq)) < 0) {
        return NULL;
    }
    PyObject *out = dump_obj(&state, self, false);
    Py_XDECREF(state.builtin_types_seq);
    return out;
}

static PyObject *
Struct_validate(PyObject *cls, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    if (nargs != 1) {
        return PyErr_Format(PyExc_TypeError,
            "struct_validate() takes exactly 1 positional argument (%zd given)", nargs);
    }
    PyObject *obj = args[0];

    StructspecState *mod = structtype_get_global_state();
    if (mod == NULL) return NULL;
    int strict = 1, from_attributes = 0;
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < nkwargs; i++) {
            PyObject *name = PyTuple_GET_ITEM(kwnames, i);
            PyObject *val = args[nargs + i];
            if (PyUnicode_CompareWithASCIIString(name, "strict") == 0) {
                strict = PyObject_IsTrue(val);
            } else if (PyUnicode_CompareWithASCIIString(name, "from_attributes") == 0) {
                from_attributes = PyObject_IsTrue(val);
            } else {
                return PyErr_Format(PyExc_TypeError,
                    "struct_validate() got an unexpected keyword argument '%U'", name);
            }
        }
    }

    ValidateState state;
    state.mod = mod;
    state.strict = strict;
    state.from_attributes = from_attributes;
    state.str_keys = strict ? false : true;
    state.builtin_types = 0;

    if (ms_is_struct_cls(cls)) {
        PyObject *info = StructInfo_Convert(cls);
        if (info == NULL) return NULL;
        bool array_like = ((StructMetaObject *)cls)->array_like == OPT_TRUE;
        TypeNodeSimple type;
        type.types = array_like ? MS_TYPE_STRUCT_ARRAY : MS_TYPE_STRUCT;
        type.details[0].pointer = info;
        PyObject *out = validate_obj(&state, obj, (TypeNode *)(&type), NULL);
        Py_DECREF(info);
        return out;
    }

    TypeNode *type = TypeNode_Convert(cls);
    if (type == NULL) return NULL;
    PyObject *out = validate_obj(&state, obj, type, NULL);
    TypeNode_Free(type);
    return out;
}

/* Recursive helper for struct_validate_self — validates each field against its type
 * annotation, recursing into nested struct fields without creating intermediate
 * objects (no dump roundtrip). */
static int
struct_check_recursive(
    PyObject *self,
    StructspecState *mod
) {
    StructMetaObject *st_type = (StructMetaObject *)Py_TYPE(self);

    PyObject *info_obj = StructInfo_Convert((PyObject *)st_type);
    if (info_obj == NULL) return -1;
    StructInfo *info = (StructInfo *)info_obj;

    Py_ssize_t nfields = PyTuple_GET_SIZE(st_type->struct_fields);

    int ret = 0;
    for (Py_ssize_t i = 0; i < nfields && ret == 0; i++) {
        PyObject *val = Struct_get_index(self, i);
        if (val == NULL) continue;

        TypeNode *field_type = info->types[i];
        if (field_type == NULL) continue;

        /* Direct struct fields: recurse into them */
        if (field_type->types & (MS_TYPE_STRUCT | MS_TYPE_STRUCT_ARRAY)) {
            StructInfo *nested_info = TypeNode_get_struct_info(field_type);
            if (Py_TYPE(val) == (PyTypeObject *)nested_info->class) {
                ret = struct_check_recursive(val, mod);
                continue;
            }
        }
        /* Struct union fields (e.g. Point | None, A | B): recurse if value is a struct */
        else if (
            field_type->types & (MS_TYPE_STRUCT_UNION | MS_TYPE_STRUCT_ARRAY_UNION)
        ) {
            if (ms_is_struct_inst(val)) {
                ret = struct_check_recursive(val, mod);
                continue;
            }
        }

        /* All other types: validate via validate_obj() */
        ValidateState state;
        state.mod = mod;
        state.strict = 1;
        state.from_attributes = 0;
        state.str_keys = 0;
        state.builtin_types = 0;
        PyObject *out = validate_obj(&state, val, field_type, NULL);
        if (out == NULL) {
            ret = -1;
        } else {
            Py_DECREF(out);
        }
    }

    Py_DECREF(info_obj);
    return ret;
}

static PyObject *
Struct_check(PyObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    if (nargs > 0) {
        return PyErr_Format(PyExc_TypeError,
            "struct_validate_self() takes no positional arguments (%zd given)", nargs);
    }
    if (kwnames != NULL) {
        Py_ssize_t nkwargs = PyTuple_GET_SIZE(kwnames);
        if (nkwargs > 0) {
            PyObject *name = PyTuple_GET_ITEM(kwnames, 0);
            return PyErr_Format(PyExc_TypeError,
                "struct_validate_self() got an unexpected keyword argument '%U'", name);
        }
    }

    StructspecState *mod = structtype_get_global_state();
    if (mod == NULL) return NULL;

    if (!ms_is_struct_inst(self)) {
        PyErr_SetString(PyExc_TypeError, "struct_validate_self() requires a Struct instance");
        return NULL;
    }

    if (struct_check_recursive(self, mod) < 0)
        return NULL;

    Py_RETURN_NONE;
}

/*************************************************************************
 * Module Setup                                                          *
 *************************************************************************/

static struct PyMethodDef structtype_methods[] = {
    {"_json_encode", (PyCFunction) structtype_json_encode, METH_FASTCALL | METH_KEYWORDS, ""},
    {"_json_decode", (PyCFunction) structtype_json_decode, METH_FASTCALL | METH_KEYWORDS, ""},
    {"_dump", (PyCFunction) structtype_dump, METH_VARARGS | METH_KEYWORDS, ""},
    {"_validate", (PyCFunction) structtype_validate, METH_VARARGS | METH_KEYWORDS, ""},
    {NULL, NULL} /* sentinel */
};

static int
structtype_clear(PyObject *m)
{
    StructspecState *st = structtype_get_state(m);
    Py_CLEAR(st->EncodeError);
    Py_CLEAR(st->DecodeError);
    Py_CLEAR(st->ValidationError);
    Py_CLEAR(st->StructType);
    Py_CLEAR(st->EnumMetaType);
    Py_CLEAR(st->ABCMetaType);
    Py_CLEAR(st->_abc_init);
    Py_CLEAR(st->struct_lookup_cache);
    Py_CLEAR(st->str___weakref__);
    Py_CLEAR(st->str___dict__);
    Py_CLEAR(st->str___structtype_cached_hash__);
    Py_CLEAR(st->str__value2member_map_);
    Py_CLEAR(st->str___structtype_cache__);
    Py_CLEAR(st->str__value_);
    Py_CLEAR(st->str__missing_);
    Py_CLEAR(st->str_type);
    Py_CLEAR(st->str_strict);
    Py_CLEAR(st->str_sort_keys);
    Py_CLEAR(st->str_decimal_format);
    Py_CLEAR(st->str_uuid_format);
    Py_CLEAR(st->str_utcoffset);
    Py_CLEAR(st->str___origin__);
    Py_CLEAR(st->str___args__);
    Py_CLEAR(st->str___metadata__);
    Py_CLEAR(st->str___total__);
    Py_CLEAR(st->str___required_keys__);
    Py_CLEAR(st->str__fields);
    Py_CLEAR(st->str__field_defaults);
    Py_CLEAR(st->str___post_init__);
    Py_CLEAR(st->str___dataclass_fields__);
    Py_CLEAR(st->str___attrs_attrs__);
    Py_CLEAR(st->str___supertype__);
#if PY312_PLUS
    Py_CLEAR(st->str___value__);
#endif
    Py_CLEAR(st->str___bound__);
    Py_CLEAR(st->str___constraints__);
    Py_CLEAR(st->str_int);
    Py_CLEAR(st->str_is_safe);
    Py_CLEAR(st->UUIDType);
    Py_CLEAR(st->uuid_safeuuid_unknown);
    Py_CLEAR(st->DecimalType);
    Py_CLEAR(st->EnumType);
    Py_CLEAR(st->typing_union);
    Py_CLEAR(st->typing_any);
    Py_CLEAR(st->typing_literal);
    Py_CLEAR(st->typing_classvar);
    Py_CLEAR(st->typing_typevar);
    Py_CLEAR(st->typing_final);
    Py_CLEAR(st->typing_generic);
    Py_CLEAR(st->typing_generic_alias);
    Py_CLEAR(st->typing_annotated_alias);
    Py_CLEAR(st->concrete_types);
    Py_CLEAR(st->get_type_hints);
    Py_CLEAR(st->get_class_annotations);
    Py_CLEAR(st->get_typeddict_info);
    Py_CLEAR(st->get_dataclass_info);
    Py_CLEAR(st->rebuild);
    Py_CLEAR(st->types_uniontype);
    Py_CLEAR(st->convert_generic_alias);
#if PY312_PLUS
    Py_CLEAR(st->typing_typealiastype);
#endif
    Py_CLEAR(st->astimezone);
    Py_CLEAR(st->re_compile);
    return 0;
}

static void
structtype_free(PyObject *m)
{
    structtype_clear(m);
}

static int
structtype_traverse(PyObject *m, visitproc visit, void *arg)
{
    StructspecState *st = structtype_get_state(m);

    Py_VISIT(st->EncodeError);
    Py_VISIT(st->DecodeError);
    Py_VISIT(st->ValidationError);
    Py_VISIT(st->StructType);
    Py_VISIT(st->EnumMetaType);
    Py_VISIT(st->ABCMetaType);
    Py_VISIT(st->_abc_init);
    Py_VISIT(st->struct_lookup_cache);
    Py_VISIT(st->typing_union);
    Py_VISIT(st->typing_any);
    Py_VISIT(st->typing_literal);
    Py_VISIT(st->typing_classvar);
    Py_VISIT(st->typing_typevar);
    Py_VISIT(st->typing_final);
    Py_VISIT(st->typing_generic);
    Py_VISIT(st->typing_generic_alias);
    Py_VISIT(st->typing_annotated_alias);
    Py_VISIT(st->concrete_types);
    Py_VISIT(st->get_type_hints);
    Py_VISIT(st->get_class_annotations);
    Py_VISIT(st->get_typeddict_info);
    Py_VISIT(st->get_dataclass_info);
    Py_VISIT(st->rebuild);
    Py_VISIT(st->types_uniontype);
    Py_VISIT(st->convert_generic_alias);
#if PY312_PLUS
    Py_VISIT(st->typing_typealiastype);
#endif
    Py_VISIT(st->UUIDType);
    Py_VISIT(st->uuid_safeuuid_unknown);
    Py_VISIT(st->DecimalType);
    Py_VISIT(st->EnumType);
    Py_VISIT(st->astimezone);
    Py_VISIT(st->re_compile);
    return 0;
}

static struct PyModuleDef structtypemodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "structtype._core",
    .m_size = sizeof(StructspecState),
    .m_methods = structtype_methods,
    .m_traverse = structtype_traverse,
    .m_clear = structtype_clear,
    .m_free =(freefunc)structtype_free
};

PyMODINIT_FUNC
PyInit__core(void)
{
    PyObject *m, *temp_module, *temp_obj;
    StructspecState *st;

    PyDateTime_IMPORT;

    m = PyState_FindModule(&structtypemodule);
    if (m) {
        Py_INCREF(m);
        return m;
    }

    StructMetaType.tp_base = &PyType_Type;
    if (PyType_Ready(&NoDefault_Type) < 0)
        return NULL;
    if (PyType_Ready(&Unset_Type) < 0)
        return NULL;
    if (PyType_Ready(&Factory_Type) < 0)
        return NULL;
    if (PyType_Ready(&Field_Type) < 0)
        return NULL;
    if (PyType_Ready(&IntLookup_Type) < 0)
        return NULL;
    if (PyType_Ready(&StrLookup_Type) < 0)
        return NULL;
    if (PyType_Ready(&LiteralInfo_Type) < 0)
        return NULL;
    if (PyType_Ready(&TypedDictInfo_Type) < 0)
        return NULL;
    if (PyType_Ready(&DataclassInfo_Type) < 0)
        return NULL;
    if (PyType_Ready(&NamedTupleInfo_Type) < 0)
        return NULL;
    if (PyType_Ready(&StructInfo_Type) < 0)
        return NULL;

    if (PyType_Ready(&StructMetaType) < 0)
        return NULL;
    if (PyType_Ready(&StructMixinType) < 0)
        return NULL;
    if (PyType_Ready(&StructConfig_Type) < 0)
        return NULL;
    if (PyType_Ready(&Raw_Type) < 0)
        return NULL;
    if (PyType_Ready(&JSONEncoder_Type) < 0)
        return NULL;
    if (PyType_Ready(&JSONDecoder_Type) < 0)
        return NULL;

    /* Create the module */
    m = PyModule_Create(&structtypemodule);
    if (m == NULL)
        return NULL;

    /* Add types */
    if (PyModule_AddObjectRef(m, "Factory", (PyObject *)&Factory_Type) < 0)
        return NULL;
    if (PyModule_AddObjectRef(m, "Field", (PyObject *)&Field_Type) < 0)
        return NULL;

    if (PyModule_AddObjectRef(m, "StructConfig", (PyObject *)&StructConfig_Type) < 0)
        return NULL;
    if (PyModule_AddObjectRef(m, "Raw", (PyObject *)&Raw_Type) < 0)
        return NULL;
    if (PyModule_AddObjectRef(m, "JSONEncoder", (PyObject *)&JSONEncoder_Type) < 0)
        return NULL;
    if (PyModule_AddObjectRef(m, "JSONDecoder", (PyObject *)&JSONDecoder_Type) < 0)
        return NULL;
    if (PyModule_AddObjectRef(m, "UnsetType", (PyObject *)&Unset_Type) < 0)
        return NULL;
    if (PyModule_AddObjectRef(m, "StructMeta", (PyObject *)&StructMetaType) < 0) {
        return NULL;
    }

    st = structtype_get_state(m);

    /* Add NODEFAULT singleton */
    if (PyModule_AddObjectRef(m, "NODEFAULT", NODEFAULT) < 0)
        return NULL;

    /* Add UNSET singleton */
    if (PyModule_AddObjectRef(m, "UNSET", UNSET) < 0)
        return NULL;

    /* Initialize the exceptions. */
    st->EncodeError = PyErr_NewExceptionWithDoc(
        "structtype.EncodeError",
        "An error occurred while encoding an object",
        PyExc_ValueError, NULL
    );
    if (st->EncodeError == NULL) return NULL;

    st->DecodeError = PyErr_NewExceptionWithDoc(
        "structtype.DecodeError",
        "An error occurred while decoding an object",
        PyExc_ValueError, NULL
    );
    if (st->DecodeError == NULL) return NULL;

    st->ValidationError = PyErr_NewExceptionWithDoc(
        "structtype.ValidationError",
        "The message didn't match the expected schema",
         PyExc_ValueError, NULL
    );
    if (st->ValidationError == NULL) return NULL;

    if (PyModule_AddObjectRef(m, "EncodeError", st->EncodeError) < 0)
        return NULL;
    if (PyModule_AddObjectRef(m, "DecodeError", st->DecodeError) < 0)
        return NULL;
    if (PyModule_AddObjectRef(m, "ValidationError", st->ValidationError) < 0)
        return NULL;

    /* Initialize the struct_lookup_cache */
    st->struct_lookup_cache = PyDict_New();
    if (PyModule_AddObjectRef(m, "_struct_lookup_cache", st->struct_lookup_cache) < 0)
        return NULL;

#define SET_REF(attr, name) \
    do { \
    st->attr = PyObject_GetAttrString(temp_module, name); \
    if (st->attr == NULL) return NULL; \
    } while (0)

    /* Get all imports from the typing module */
    temp_module = PyImport_ImportModule("typing");
    if (temp_module == NULL) return NULL;
    SET_REF(typing_union, "Union");
    SET_REF(typing_any, "Any");
    SET_REF(typing_literal, "Literal");
    SET_REF(typing_classvar, "ClassVar");
    SET_REF(typing_typevar, "TypeVar");
    SET_REF(typing_final, "Final");
    SET_REF(typing_generic, "Generic");
    SET_REF(typing_generic_alias, "_GenericAlias");
#if PY312_PLUS
    SET_REF(typing_typealiastype, "TypeAliasType");
#endif
    Py_DECREF(temp_module);

    temp_module = PyImport_ImportModule("structtype._utils");
    if (temp_module == NULL) return NULL;
    SET_REF(concrete_types, "_CONCRETE_TYPES");
    SET_REF(get_type_hints, "get_type_hints");
    SET_REF(get_class_annotations, "get_class_annotations");
    SET_REF(get_typeddict_info, "get_typeddict_info");
    SET_REF(get_dataclass_info, "get_dataclass_info");
    SET_REF(typing_annotated_alias, "_AnnotatedAlias");
    SET_REF(rebuild, "rebuild");
    SET_REF(convert_generic_alias, "convert_generic_alias");
    Py_DECREF(temp_module);

    temp_module = PyImport_ImportModule("types");
    if (temp_module == NULL) return NULL;
    SET_REF(types_uniontype, "UnionType");
    Py_DECREF(temp_module);

    /* Get the EnumMeta type and Enum base class */
    temp_module = PyImport_ImportModule("enum");
    if (temp_module == NULL)
        return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "EnumMeta");
    if (temp_obj == NULL) {
        Py_DECREF(temp_module);
        return NULL;
    }
    if (!PyType_Check(temp_obj)) {
        Py_DECREF(temp_obj);
        Py_DECREF(temp_module);
        PyErr_SetString(PyExc_TypeError, "enum.EnumMeta should be a type");
        return NULL;
    }
    st->EnumMetaType = (PyTypeObject *)temp_obj;
    st->EnumType = PyObject_GetAttrString(temp_module, "Enum");
    Py_DECREF(temp_module);
    if (st->EnumType == NULL) return NULL;

    /* Get the abc.ABCMeta type and _abc_init helper */
    temp_module = PyImport_ImportModule("abc");
    if (temp_module == NULL)
        return NULL;

    temp_obj = PyObject_GetAttrString(temp_module, "ABCMeta");
    if (temp_obj == NULL) {
        Py_DECREF(temp_module);
        return NULL;
    }
    if (!PyType_Check(temp_obj)) {
        Py_DECREF(temp_obj);
        Py_DECREF(temp_module);
        PyErr_SetString(PyExc_TypeError, "abc.ABCMeta should be a type");
        return NULL;
    }
    st->ABCMetaType = (PyTypeObject *)temp_obj;

    temp_obj = PyObject_GetAttrString(temp_module, "_abc_init");
    Py_DECREF(temp_module);
    if (temp_obj == NULL)
        return NULL;
    st->_abc_init = temp_obj;

    /* Get the datetime.datetime.astimezone method */
    temp_module = PyImport_ImportModule("datetime");
    if (temp_module == NULL) return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "datetime");
    Py_DECREF(temp_module);
    if (temp_obj == NULL) return NULL;
    st->astimezone = PyObject_GetAttrString(temp_obj, "astimezone");
    Py_DECREF(temp_obj);
    if (st->astimezone == NULL) return NULL;

    /* uuid module imports */
    temp_module = PyImport_ImportModule("uuid");
    if (temp_module == NULL) return NULL;
    st->UUIDType = PyObject_GetAttrString(temp_module, "UUID");
    if (st->UUIDType == NULL) return NULL;
    temp_obj = PyObject_GetAttrString(temp_module, "SafeUUID");
    if (temp_obj == NULL) return NULL;
    st->uuid_safeuuid_unknown = PyObject_GetAttrString(temp_obj, "unknown");
    Py_DECREF(temp_obj);
    if (st->uuid_safeuuid_unknown == NULL) return NULL;

    /* decimal module imports */
    temp_module = PyImport_ImportModule("decimal");
    if (temp_module == NULL) return NULL;
    st->DecimalType = PyObject_GetAttrString(temp_module, "Decimal");
    if (st->DecimalType == NULL) return NULL;

    /* Build ALL_BUILTIN_TYPES convenience tuple */
    {
        Py_ssize_t n = 10;
#if PY315_PLUS
        n++;
#endif
        PyObject *all_types = PyTuple_New(n);
        if (all_types == NULL) return NULL;
        Py_ssize_t idx = 0;
        Py_INCREF(&PyBytes_Type);
        PyTuple_SET_ITEM(all_types, idx++, (PyObject *)&PyBytes_Type);
        Py_INCREF(&PyByteArray_Type);
        PyTuple_SET_ITEM(all_types, idx++, (PyObject *)&PyByteArray_Type);
        Py_INCREF(&PyMemoryView_Type);
        PyTuple_SET_ITEM(all_types, idx++, (PyObject *)&PyMemoryView_Type);
        Py_INCREF(PyDateTimeAPI->DateTimeType);
        PyTuple_SET_ITEM(all_types, idx++, (PyObject *)PyDateTimeAPI->DateTimeType);
        Py_INCREF(PyDateTimeAPI->DateType);
        PyTuple_SET_ITEM(all_types, idx++, (PyObject *)PyDateTimeAPI->DateType);
        Py_INCREF(PyDateTimeAPI->TimeType);
        PyTuple_SET_ITEM(all_types, idx++, (PyObject *)PyDateTimeAPI->TimeType);
        Py_INCREF(PyDateTimeAPI->DeltaType);
        PyTuple_SET_ITEM(all_types, idx++, (PyObject *)PyDateTimeAPI->DeltaType);
        Py_INCREF(st->UUIDType);
        PyTuple_SET_ITEM(all_types, idx++, st->UUIDType);
        Py_INCREF(st->DecimalType);
        PyTuple_SET_ITEM(all_types, idx++, st->DecimalType);
        Py_INCREF(st->EnumType);
        PyTuple_SET_ITEM(all_types, idx++, st->EnumType);
#if PY315_PLUS
        Py_INCREF(&PyFrozenDict_Type);
        PyTuple_SET_ITEM(all_types, idx++, (PyObject *)&PyFrozenDict_Type);
#endif
        if (PyModule_AddObjectRef(m, "ALL_BUILTIN_TYPES", all_types) < 0) {
            Py_DECREF(all_types);
            return NULL;
        }
        Py_DECREF(all_types);
    }

    /* Get the re.compile function */
    temp_module = PyImport_ImportModule("re");
    if (temp_module == NULL) return NULL;
    st->re_compile = PyObject_GetAttrString(temp_module, "compile");
    Py_DECREF(temp_module);
    if (st->re_compile == NULL) return NULL;

    /* Initialize cached constant strings */
#define CACHED_STRING(attr, str) \
    if ((st->attr = PyUnicode_InternFromString(str)) == NULL) return NULL
    CACHED_STRING(str___weakref__, "__weakref__");
    CACHED_STRING(str___dict__, "__dict__");
    CACHED_STRING(str___structtype_cached_hash__, "__structtype_cached_hash__");
    CACHED_STRING(str__value2member_map_, "_value2member_map_");
    CACHED_STRING(str___structtype_cache__, "__structtype_cache__");
    CACHED_STRING(str__value_, "_value_");
    CACHED_STRING(str__missing_, "_missing_");
    CACHED_STRING(str_type, "type");
    CACHED_STRING(str_strict, "strict");
    CACHED_STRING(str_sort_keys, "sort_keys");
    CACHED_STRING(str_decimal_format, "decimal_format");
    CACHED_STRING(str_uuid_format, "uuid_format");
    CACHED_STRING(str_utcoffset, "utcoffset");
    CACHED_STRING(str___origin__, "__origin__");
    CACHED_STRING(str___args__, "__args__");
    CACHED_STRING(str___metadata__, "__metadata__");
    CACHED_STRING(str___total__, "__total__");
    CACHED_STRING(str___required_keys__, "__required_keys__");
    CACHED_STRING(str__fields, "_fields");
    CACHED_STRING(str__field_defaults, "_field_defaults");
    CACHED_STRING(str___post_init__, "__post_init__");
    CACHED_STRING(str___dataclass_fields__, "__dataclass_fields__");
    CACHED_STRING(str___attrs_attrs__, "__attrs_attrs__");
    CACHED_STRING(str___supertype__, "__supertype__");
#if PY312_PLUS
    CACHED_STRING(str___value__, "__value__");
#endif
    CACHED_STRING(str___bound__, "__bound__");
    CACHED_STRING(str___constraints__, "__constraints__");
    CACHED_STRING(str_int, "int");
    CACHED_STRING(str_is_safe, "is_safe");

    /* Initialize the Struct Type */
    PyState_AddModule(m, &structtypemodule);
    st->StructType = PyObject_CallFunction(
        (PyObject *)&StructMetaType, "s(O){ssss}", "Struct", &StructMixinType,
        "__module__", "structtype", "__doc__", Struct__doc__
    );
    if (PyModule_AddObjectRef(m, "Struct", st->StructType) < 0) return NULL;
#ifdef Py_GIL_DISABLED
    PyUnstable_Module_SetGIL(m, Py_MOD_GIL_NOT_USED);
#endif
    return m;
}
