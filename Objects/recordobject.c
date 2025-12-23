/* Record object implementation - immutable named container */

#include "Python.h"
#include "pycore_object.h"        // _PyObject_GC_TRACK
#include "structmember.h"         // PyMemberDef
#include "recordobject.h"

/* Hash constants - same as used by tupleobject.c */
#if SIZEOF_PY_UHASH_T > 4
#define _PyHASH_XXPRIME_1 ((Py_uhash_t)11400714785074694791ULL)
#define _PyHASH_XXPRIME_2 ((Py_uhash_t)14029467366897019727ULL)
#define _PyHASH_XXPRIME_5 ((Py_uhash_t)2870177450012600261ULL)
#define _PyHASH_XXROTATE(x) ((x << 31) | (x >> 33))  /* Rotate left 31 bits */
#else
#define _PyHASH_XXPRIME_1 ((Py_uhash_t)2654435761UL)
#define _PyHASH_XXPRIME_2 ((Py_uhash_t)2246822519UL)
#define _PyHASH_XXPRIME_5 ((Py_uhash_t)374761393UL)
#define _PyHASH_XXROTATE(x) ((x << 13) | (x >> 19))  /* Rotate left 13 bits */
#endif

/*
 * RecordObject implementation
 *
 * A Record is an immutable container with named fields, similar to
 * a simplified namedtuple but implemented entirely in C.
 *
 * Memory layout:
 *   PyObject_VAR_HEAD  (includes ob_size = number of fields)
 *   r_hash             (cached hash value, -1 if not computed)
 *   r_names            (tuple of field name strings)
 *   r_values[n]        (array of field values)
 */

/* Forward declarations */
static PyObject *record_repr(RecordObject *r);
static Py_hash_t record_hash(RecordObject *r);
static PyObject *record_richcompare(PyObject *v, PyObject *w, int op);
static Py_ssize_t record_length(RecordObject *r);
static PyObject *record_item(RecordObject *r, Py_ssize_t i);
static PyObject *record_getattro(RecordObject *r, PyObject *name);
static int record_traverse(RecordObject *r, visitproc visit, void *arg);
static void record_dealloc(RecordObject *r);


/* ----------------- Construction ----------------- */

PyObject *
PyRecord_New(PyObject *names, PyObject **values, Py_ssize_t n)
{
    RecordObject *record;
    Py_ssize_t i;

    /* Validate names is a tuple of strings */
    if (!PyTuple_CheckExact(names) || PyTuple_GET_SIZE(names) != n) {
        PyErr_SetString(PyExc_TypeError,
                        "names must be a tuple with correct size");
        return NULL;
    }

    for (i = 0; i < n; i++) {
        if (!PyUnicode_Check(PyTuple_GET_ITEM(names, i))) {
            PyErr_SetString(PyExc_TypeError,
                            "all field names must be strings");
            return NULL;
        }
    }

    /* Allocate the record object */
    record = PyObject_GC_NewVar(RecordObject, &PyRecord_Type, n);
    if (record == NULL) {
        return NULL;
    }

    /* Initialize fields */
    record->r_hash = -1;  /* Not yet computed */
    Py_INCREF(names);
    record->r_names = names;

    /* Copy values (stealing references) */
    for (i = 0; i < n; i++) {
        record->r_values[i] = values[i];
    }

    _PyObject_GC_TRACK(record);
    return (PyObject *)record;
}


/* ----------------- Deallocation ----------------- */

static void
record_dealloc(RecordObject *r)
{
    Py_ssize_t i, n = Py_SIZE(r);

    PyObject_GC_UnTrack(r);
    Py_TRASHCAN_BEGIN(r, record_dealloc)

    Py_XDECREF(r->r_names);
    for (i = 0; i < n; i++) {
        Py_XDECREF(r->r_values[i]);
    }

    Py_TYPE(r)->tp_free((PyObject *)r);

    Py_TRASHCAN_END
}


/* ----------------- GC Traversal ----------------- */

static int
record_traverse(RecordObject *r, visitproc visit, void *arg)
{
    Py_ssize_t i, n = Py_SIZE(r);

    Py_VISIT(r->r_names);
    for (i = 0; i < n; i++) {
        Py_VISIT(r->r_values[i]);
    }
    return 0;
}


/* ----------------- Repr ----------------- */

static PyObject *
record_repr(RecordObject *r)
{
    Py_ssize_t i, n = Py_SIZE(r);
    _PyUnicodeWriter writer;
    int first = 1;

    i = Py_ReprEnter((PyObject *)r);
    if (i != 0) {
        return i > 0 ? PyUnicode_FromString("Record(...)") : NULL;
    }

    _PyUnicodeWriter_Init(&writer);
    writer.overallocate = 1;
    writer.min_length = 8 + n * 10;  /* "Record(" + fields + ")" */

    if (_PyUnicodeWriter_WriteASCIIString(&writer, "Record(", 7) < 0)
        goto error;

    for (i = 0; i < n; i++) {
        PyObject *name = PyTuple_GET_ITEM(r->r_names, i);
        PyObject *value = r->r_values[i];
        PyObject *value_repr;

        if (!first) {
            if (_PyUnicodeWriter_WriteASCIIString(&writer, ", ", 2) < 0)
                goto error;
        }
        first = 0;

        /* Write "name=" */
        if (_PyUnicodeWriter_WriteStr(&writer, name) < 0)
            goto error;
        if (_PyUnicodeWriter_WriteChar(&writer, '=') < 0)
            goto error;

        /* Write repr(value) */
        value_repr = PyObject_Repr(value);
        if (value_repr == NULL)
            goto error;
        if (_PyUnicodeWriter_WriteStr(&writer, value_repr) < 0) {
            Py_DECREF(value_repr);
            goto error;
        }
        Py_DECREF(value_repr);
    }

    if (_PyUnicodeWriter_WriteChar(&writer, ')') < 0)
        goto error;

    Py_ReprLeave((PyObject *)r);
    return _PyUnicodeWriter_Finish(&writer);

error:
    _PyUnicodeWriter_Dealloc(&writer);
    Py_ReprLeave((PyObject *)r);
    return NULL;
}


/* ----------------- Hash ----------------- */

static Py_hash_t
record_hash(RecordObject *r)
{
    Py_ssize_t i, n = Py_SIZE(r);
    Py_uhash_t acc;

    if (r->r_hash != -1) {
        return r->r_hash;
    }

    /* Use the same XXH3-inspired algorithm as tuple */
    acc = _PyHASH_XXPRIME_5;
    for (i = 0; i < n; i++) {
        Py_uhash_t lane = PyObject_Hash(r->r_values[i]);
        if (lane == (Py_uhash_t)-1) {
            return -1;
        }
        acc += lane * _PyHASH_XXPRIME_2;
        acc = _PyHASH_XXROTATE(acc);
        acc *= _PyHASH_XXPRIME_1;
    }

    /* Also incorporate the field names into the hash */
    Py_uhash_t names_hash = PyObject_Hash(r->r_names);
    if (names_hash == (Py_uhash_t)-1) {
        return -1;
    }
    acc ^= names_hash;

    acc += n ^ (_PyHASH_XXPRIME_5 ^ 3527539UL);

    if (acc == (Py_uhash_t)-1) {
        acc = 1546275796;
    }

    r->r_hash = acc;
    return acc;
}


/* ----------------- Comparison ----------------- */

static PyObject *
record_richcompare(PyObject *v, PyObject *w, int op)
{
    RecordObject *vr, *wr;
    Py_ssize_t i, n;
    int names_equal;

    if (!PyRecord_Check(v) || !PyRecord_Check(w)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    vr = (RecordObject *)v;
    wr = (RecordObject *)w;

    /* Records must have same size */
    if (Py_SIZE(vr) != Py_SIZE(wr)) {
        if (op == Py_EQ) Py_RETURN_FALSE;
        if (op == Py_NE) Py_RETURN_TRUE;
        Py_RETURN_NOTIMPLEMENTED;
    }

    n = Py_SIZE(vr);

    /* For equality, field names must also match */
    if (op == Py_EQ || op == Py_NE) {
        names_equal = PyObject_RichCompareBool(vr->r_names, wr->r_names, Py_EQ);
        if (names_equal < 0) {
            return NULL;
        }
        if (!names_equal) {
            if (op == Py_EQ) Py_RETURN_FALSE;
            if (op == Py_NE) Py_RETURN_TRUE;
        }

        /* Compare all values */
        for (i = 0; i < n; i++) {
            int cmp = PyObject_RichCompareBool(vr->r_values[i],
                                               wr->r_values[i], Py_EQ);
            if (cmp < 0) return NULL;
            if (!cmp) {
                if (op == Py_EQ) Py_RETURN_FALSE;
                if (op == Py_NE) Py_RETURN_TRUE;
            }
        }

        if (op == Py_EQ) Py_RETURN_TRUE;
        if (op == Py_NE) Py_RETURN_FALSE;
    }

    /* For ordering comparisons, we don't support them */
    Py_RETURN_NOTIMPLEMENTED;
}


/* ----------------- Sequence Protocol ----------------- */

static Py_ssize_t
record_length(RecordObject *r)
{
    return Py_SIZE(r);
}

static PyObject *
record_item(RecordObject *r, Py_ssize_t i)
{
    Py_ssize_t n = Py_SIZE(r);

    /* Handle negative indices */
    if (i < 0) {
        i += n;
    }

    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "record index out of range");
        return NULL;
    }

    PyObject *value = r->r_values[i];
    Py_INCREF(value);
    return value;
}

static PySequenceMethods record_as_sequence = {
    (lenfunc)record_length,         /* sq_length */
    0,                              /* sq_concat */
    0,                              /* sq_repeat */
    (ssizeargfunc)record_item,      /* sq_item */
    0,                              /* sq_slice (deprecated) */
    0,                              /* sq_ass_item (immutable) */
    0,                              /* sq_ass_slice (deprecated) */
    0,                              /* sq_contains */
    0,                              /* sq_inplace_concat */
    0,                              /* sq_inplace_repeat */
};


/* ----------------- Attribute Access ----------------- */

static PyObject *
record_getattro(RecordObject *r, PyObject *name)
{
    Py_ssize_t i, n = Py_SIZE(r);

    /* First check if it's a field name */
    if (PyUnicode_Check(name)) {
        for (i = 0; i < n; i++) {
            PyObject *field = PyTuple_GET_ITEM(r->r_names, i);
            int cmp = PyUnicode_Compare(name, field);
            if (cmp == 0) {
                PyObject *value = r->r_values[i];
                Py_INCREF(value);
                return value;
            }
            if (PyErr_Occurred()) {
                return NULL;
            }
        }
    }

    /* Fall back to generic attribute lookup (for methods, __class__, etc.) */
    return PyObject_GenericGetAttr((PyObject *)r, name);
}


/* ----------------- Public API ----------------- */

PyObject *
PyRecord_GetItem(PyObject *record, Py_ssize_t index)
{
    if (!PyRecord_Check(record)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    RecordObject *r = (RecordObject *)record;
    if (index < 0 || index >= Py_SIZE(r)) {
        PyErr_SetString(PyExc_IndexError, "record index out of range");
        return NULL;
    }
    /* Return borrowed reference */
    return r->r_values[index];
}

PyObject *
PyRecord_GetFieldByName(PyObject *record, PyObject *name)
{
    if (!PyRecord_Check(record)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return record_getattro((RecordObject *)record, name);
}


/* ----------------- Python Constructor ----------------- */

static PyObject *
record_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    /* Record() only accepts keyword arguments */
    if (PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError,
                        "Record() takes no positional arguments");
        return NULL;
    }

    if (kwds == NULL || !PyDict_Check(kwds) || PyDict_Size(kwds) == 0) {
        PyErr_SetString(PyExc_TypeError,
                        "Record() requires at least one keyword argument");
        return NULL;
    }

    Py_ssize_t n = PyDict_Size(kwds);
    PyObject *names = PyTuple_New(n);
    if (names == NULL) {
        return NULL;
    }

    PyObject **values = PyMem_Malloc(n * sizeof(PyObject *));
    if (values == NULL) {
        Py_DECREF(names);
        return PyErr_NoMemory();
    }

    /* Iterate through kwargs */
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    Py_ssize_t i = 0;
    while (PyDict_Next(kwds, &pos, &key, &value)) {
        Py_INCREF(key);
        Py_INCREF(value);
        PyTuple_SET_ITEM(names, i, key);
        values[i] = value;
        i++;
    }

    PyObject *record = PyRecord_New(names, values, n);
    PyMem_Free(values);

    if (record == NULL) {
        Py_DECREF(names);
        return NULL;
    }

    return record;
}


/* ----------------- Type Object ----------------- */

PyDoc_STRVAR(record_doc,
"Record(name=value, ...)\n\
\n\
Immutable container with named fields.\n\
\n\
Records support:\n\
  - Attribute access: r.field_name\n\
  - Indexing: r[0], r[1], len(r)\n\
  - Hashing: hash(r) (usable as dict key)\n\
  - Equality: r1 == r2\n\
  - Nice repr: Record(x=10, y=20)");

PyTypeObject PyRecord_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "Record",                               /* tp_name */
    sizeof(RecordObject) - sizeof(PyObject *),  /* tp_basicsize */
    sizeof(PyObject *),                     /* tp_itemsize */
    (destructor)record_dealloc,             /* tp_dealloc */
    0,                                      /* tp_vectorcall_offset */
    0,                                      /* tp_getattr */
    0,                                      /* tp_setattr */
    0,                                      /* tp_as_async */
    (reprfunc)record_repr,                  /* tp_repr */
    0,                                      /* tp_as_number */
    &record_as_sequence,                    /* tp_as_sequence */
    0,                                      /* tp_as_mapping */
    (hashfunc)record_hash,                  /* tp_hash */
    0,                                      /* tp_call */
    0,                                      /* tp_str */
    (getattrofunc)record_getattro,          /* tp_getattro */
    0,                                      /* tp_setattro */
    0,                                      /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_SEQUENCE,                /* tp_flags */
    record_doc,                             /* tp_doc */
    (traverseproc)record_traverse,          /* tp_traverse */
    0,                                      /* tp_clear */
    record_richcompare,                     /* tp_richcompare */
    0,                                      /* tp_weaklistoffset */
    0,                                      /* tp_iter */
    0,                                      /* tp_iternext */
    0,                                      /* tp_methods */
    0,                                      /* tp_members */
    0,                                      /* tp_getset */
    0,                                      /* tp_base */
    0,                                      /* tp_dict */
    0,                                      /* tp_descr_get */
    0,                                      /* tp_descr_set */
    0,                                      /* tp_dictoffset */
    0,                                      /* tp_init */
    PyType_GenericAlloc,                    /* tp_alloc */
    record_new,                             /* tp_new */
    PyObject_GC_Del,                        /* tp_free */
};
