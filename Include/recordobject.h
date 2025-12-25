/* Record object interface - immutable named container */

#ifndef Py_RECORDOBJECT_H
#define Py_RECORDOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#include "pyport.h"
#include "object.h"

/*
 * RecordObject: An immutable container with named fields.
 * Similar to a simplified namedtuple implemented in C.
 *
 * Features:
 *   - Attribute access by name: r.field_name
 *   - Indexing by position: r[0], r[1]
 *   - Hashable (can be used as dict key)
 *   - Equality comparison
 *   - Nice repr: Record(x=10, y=20)
 */

typedef struct {
    PyObject_VAR_HEAD
    Py_hash_t r_hash;           /* Cached hash, -1 if not yet computed */
    PyObject *r_names;          /* Tuple of field names (strings) */
    PyObject *r_values[1];      /* Flexible array of field values */
} RecordObject;

PyAPI_DATA(PyTypeObject) PyRecord_Type;

#define PyRecord_Check(op) PyObject_TypeCheck(op, &PyRecord_Type)
#define PyRecord_CheckExact(op) Py_IS_TYPE(op, &PyRecord_Type)

/* Create a new Record from names tuple and values array.
 * names: tuple of strings (field names) - reference is stolen
 * values: array of PyObject* (field values) - references are stolen
 * n: number of fields
 * Returns: new Record object, or NULL on error
 */
PyAPI_FUNC(PyObject *) PyRecord_New(PyObject *names, PyObject **values, Py_ssize_t n);

/* Get field by index (returns borrowed reference) */
PyAPI_FUNC(PyObject *) PyRecord_GetItem(PyObject *record, Py_ssize_t index);

/* Get field by name (returns new reference) */
PyAPI_FUNC(PyObject *) PyRecord_GetFieldByName(PyObject *record, PyObject *name);

/* Get number of fields */
#define PyRecord_GET_SIZE(op) Py_SIZE(op)

#ifdef __cplusplus
}
#endif
#endif /* !Py_RECORDOBJECT_H */
