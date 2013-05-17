//
// Copyright (C) 2011-13, DyND Developers
// BSD 2-Clause License, see LICENSE.txt
//

#include "utility_functions.hpp"
#include "dtype_functions.hpp"
#include "ndobject_functions.hpp"

#include <dynd/exceptions.hpp>
#include <dynd/ndobject.hpp>

#include <Python.h>

using namespace std;
using namespace dynd;
using namespace pydynd;

void pydynd::py_decref_function(void* obj)
{
    // Because dynd in general is intended to do things multi-threaded (eventually),
    // the decref function needs to be threadsafe. The way to do that is to ensure
    // we're holding the GIL. This is slower than normal DECREF, but because the
    // reference count isn't an atomic variable, this appears to be the best we
    // can do.
    if (obj != NULL) {
        PyGILState_STATE gstate;
        gstate = PyGILState_Ensure();

        Py_DECREF((PyObject *)obj);

        PyGILState_Release(gstate);
    }
}

size_t pydynd::pyobject_as_size_t(PyObject *obj)
{
    pyobject_ownref ind_obj(PyNumber_Index(obj));
#if PY_VERSION_HEX >= 0x03000000
    size_t result = PyLong_AsSize_t(ind_obj);
#else
    size_t result = (size_t)PyLong_AsUnsignedLongLong(ind_obj);
#endif
    if (result == (size_t)-1 && PyErr_Occurred()) {
        throw exception();
    }
    return result;
}

intptr_t pydynd::pyobject_as_index(PyObject *index)
{
    pyobject_ownref start_obj(PyNumber_Index(index));
    intptr_t result;
    if (PyLong_Check(start_obj.get())) {
        result = PyLong_AsSsize_t(start_obj.get());
#if PY_VERSION_HEX < 0x03000000
    } else if (PyInt_Check(start_obj.get())) {
        result = PyInt_AS_LONG(start_obj.get());
#endif
    } else {
        throw runtime_error("Value returned from PyNumber_Index is not an int or long");
    }
    if (result == -1 && PyErr_Occurred()) {
        throw exception();
    }
    return result;
}

int pydynd::pyobject_as_int_index(PyObject *index)
{
    pyobject_ownref start_obj(PyNumber_Index(index));
#if PY_VERSION_HEX >= 0x03000000
    long result = PyLong_AsLong(start_obj);
#else
    long result = PyInt_AsLong(start_obj);
#endif
    if (result == -1 && PyErr_Occurred()) {
        throw exception();
    }
    if (((unsigned long)result & 0xffffffffu) != (unsigned long)result) {
        throw overflow_error("overflow converting Python integer to 32-bit int");
    }
    return (int)result;
}

irange pydynd::pyobject_as_irange(PyObject *index)
{
    if (PySlice_Check(index)) {
        irange result;
        PySliceObject *slice = (PySliceObject *)index;
        if (slice->start != Py_None) {
            result.set_start(pyobject_as_index(slice->start));
        }
        if (slice->stop != Py_None) {
            result.set_finish(pyobject_as_index(slice->stop));
        }
        if (slice->step != Py_None) {
            result.set_step(pyobject_as_index(slice->step));
        }
        return result;
    } else {
        return irange(pyobject_as_index(index));
    }
}

std::string pydynd::pystring_as_string(PyObject *str)
{
    char *data = NULL;
    Py_ssize_t len = 0;
    if (PyUnicode_Check(str)) {
        pyobject_ownref utf8(PyUnicode_AsUTF8String(str));

#if PY_VERSION_HEX >= 0x03000000
        if (PyBytes_AsStringAndSize(utf8.get(), &data, &len) < 0) {
#else
        if (PyString_AsStringAndSize(utf8.get(), &data, &len) < 0) {
#endif
            throw runtime_error("Error getting string data");
        }
        return string(data, len);
#if PY_VERSION_HEX < 0x03000000
    } else if (PyString_Check(str)) {
        if (PyString_AsStringAndSize(str, &data, &len) < 0) {
            throw runtime_error("Error getting string data");
        }
        return string(data, len);
#endif
    } else if (WNDObject_Check(str)) {
        const ndobject& n = ((WNDObject *)str)->v;
        if (n.get_dtype().value_dtype().get_kind() == string_kind) {
            return n.as<string>();
        } else {
            stringstream ss;
            ss << "Cannot implicitly convert object of type ";
            ss << n.get_dtype() << " to string";
            throw runtime_error(ss.str());
        }
    } else {
        throw runtime_error("Cannot convert pyobject to string");
    }
}

void pydynd::pyobject_as_vector_dtype(PyObject *list_dtype, std::vector<dynd::dtype>& vector_dtype)
{
    Py_ssize_t size = PySequence_Size(list_dtype);
    vector_dtype.resize(size);
    for (Py_ssize_t i = 0; i < size; ++i) {
        pyobject_ownref item(PySequence_GetItem(list_dtype, i));
        vector_dtype[i] = make_dtype_from_pyobject(item.get());
    }
}

void pydynd::pyobject_as_vector_string(PyObject *list_string, std::vector<std::string>& vector_string)
{
    Py_ssize_t size = PySequence_Size(list_string);
    vector_string.resize(size);
    for (Py_ssize_t i = 0; i < size; ++i) {
        pyobject_ownref item(PySequence_GetItem(list_string, i));
        vector_string[i] = pystring_as_string(item.get());
    }
}

void pydynd::pyobject_as_vector_intp(PyObject *list_index, std::vector<intptr_t>& vector_intp,
                bool allow_int)
{
    if (allow_int) {
        // If permitted, convert an int into a size-1 list
        if (PyLong_Check(list_index)) {
            intptr_t v = PyLong_AsSsize_t(list_index);
            if (v == -1 && PyErr_Occurred()) {
                throw runtime_error("error converting int");
            }
            vector_intp.resize(1);
            vector_intp[0] = v;
            return;
        }
#if PY_VERSION_HEX < 0x03000000
        if (PyInt_Check(list_index)) {
            vector_intp.resize(1);
            vector_intp[0] = PyInt_AS_LONG(list_index);
            return;
        }
#endif
        if (PyIndex_Check(list_index)) {
            PyObject *idx_obj = PyNumber_Index(list_index);
            if (idx_obj != NULL) {
                intptr_t v = PyLong_AsSsize_t(idx_obj);
                Py_DECREF(idx_obj);
                if (v == -1 && PyErr_Occurred()) {
                    throw exception();
                }
                vector_intp.resize(1);
                vector_intp[0] = v;
                return;
            } else if (PyErr_ExceptionMatches(PyExc_TypeError)) {
                // Swallow a type error, fall through to the sequence code
                PyErr_Clear();
            } else {
                // Propagate the error
                throw exception();
            }
        }
    }
    Py_ssize_t size = PySequence_Size(list_index);
    vector_intp.resize(size);
    for (Py_ssize_t i = 0; i < size; ++i) {
        pyobject_ownref item(PySequence_GetItem(list_index, i));
        vector_intp[i] = pyobject_as_index(item.get());
    }
}

void pydynd::pyobject_as_vector_int(PyObject *list_int, std::vector<int>& vector_int)
{
    Py_ssize_t size = PySequence_Size(list_int);
    vector_int.resize(size);
    for (Py_ssize_t i = 0; i < size; ++i) {
        pyobject_ownref item(PySequence_GetItem(list_int, i));
        vector_int[i] = pyobject_as_int_index(item.get());
    }
}

PyObject* pydynd::intptr_array_as_tuple(size_t size, const intptr_t *values)
{
    PyObject *result = PyTuple_New(size);
    if (result == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < size; i++) {
        PyObject *o = PyLong_FromLongLong(values[i]);
        if (o == NULL) {
            Py_DECREF(result);
            return NULL;
        }
        PyTuple_SET_ITEM(result, i, o);
    }

    return result;
}

static void mark_axis(PyObject *int_axis, int ndim, dynd_bool *reduce_axes)
{
    pyobject_ownref value_obj(PyNumber_Index(int_axis));
    long value = PyLong_AsLong(value_obj);
    if (value == -1 && PyErr_Occurred()) {
        throw runtime_error("error getting integer for axis argument");
    }

    if (value >= ndim || value < -ndim) {
        throw dynd::axis_out_of_bounds(value, ndim);
    } else if (value < 0) {
        value += ndim;
    }

    if (!reduce_axes[value]) {
        reduce_axes[value] = true;
    } else {
        stringstream ss;
        ss << "axis " << value << " is specified more than once";
        throw runtime_error(ss.str());
    }
}

int pydynd::pyarg_axis_argument(PyObject *axis, int ndim, dynd_bool *reduce_axes)
{
    int axis_count = 0;

    if (axis == NULL || axis == Py_None) {
        // None means use all the axes
        for (int i = 0; i < ndim; ++i) {
            reduce_axes[i] = true;
        }
        axis_count = ndim;
    } else {
        // Start with no axes marked
        for (int i = 0; i < ndim; ++i) {
            reduce_axes[i] = false;
        }
        if (PyTuple_Check(axis)) {
            // A tuple of axes
            Py_ssize_t size = PyTuple_GET_SIZE(axis);
            for (Py_ssize_t i = 0; i < size; ++i) {
                mark_axis(PyTuple_GET_ITEM(axis, i), ndim, reduce_axes);
                axis_count++;
            }
        } else  {
            // Just one axis
            mark_axis(axis, ndim, reduce_axes);
            axis_count = 1;
        }
    }

    return axis_count;
}

assign_error_mode pydynd::pyarg_error_mode(PyObject *error_mode_obj)
{
    return (assign_error_mode)pyarg_strings_to_int(error_mode_obj, "error_mode", assign_error_default,
                    "none", assign_error_none,
                    "overflow", assign_error_overflow,
                    "fractional", assign_error_fractional,
                    "inexact", assign_error_inexact);
}

int pydynd::pyarg_strings_to_int(PyObject *obj, const char *argname, int default_value,
                const char *string0, int value0)
{
    if (obj == NULL || obj == Py_None) {
        return default_value;
    }

    string s = pystring_as_string(obj);

    if (s == string0) {
        return value0;
    }

    stringstream ss;
    ss << "argument " << argname << " was given the invalid argument value \"" << s << "\"";
    throw runtime_error(ss.str());
}

int pydynd::pyarg_strings_to_int(PyObject *obj, const char *argname, int default_value,
                const char *string0, int value0,
                const char *string1, int value1)
{
    if (obj == NULL || obj == Py_None) {
        return default_value;
    }

    string s = pystring_as_string(obj);

    if (s == string0) {
        return value0;
    } else if (s == string1) {
        return value1;
    }

    stringstream ss;
    ss << "argument " << argname << " was given the invalid argument value \"" << s << "\"";
    throw runtime_error(ss.str());
}

int pydynd::pyarg_strings_to_int(PyObject *obj, const char *argname, int default_value,
                const char *string0, int value0,
                const char *string1, int value1,
                const char *string2, int value2)
{
    if (obj == NULL || obj == Py_None) {
        return default_value;
    }

    string s = pystring_as_string(obj);

    if (s == string0) {
        return value0;
    } else if (s == string1) {
        return value1;
    } else if (s == string2) {
        return value2;
    }

    stringstream ss;
    ss << "argument " << argname << " was given the invalid argument value \"" << s << "\"";
    throw runtime_error(ss.str());
}

int pydynd::pyarg_strings_to_int(PyObject *obj, const char *argname, int default_value,
                const char *string0, int value0,
                const char *string1, int value1,
                const char *string2, int value2,
                const char *string3, int value3)
{
    if (obj == NULL || obj == Py_None) {
        return default_value;
    }

    string s = pystring_as_string(obj);

    if (s == string0) {
        return value0;
    } else if (s == string1) {
        return value1;
    } else if (s == string2) {
        return value2;
    } else if (s == string3) {
        return value3;
    }

    stringstream ss;
    ss << "argument " << argname << " was given the invalid argument value \"" << s << "\"";
    throw runtime_error(ss.str());
}

bool pydynd::pyarg_bool(PyObject *obj, const char *argname, bool default_value)
{
    if (obj == NULL || obj == Py_None) {
        return default_value;
    }

    if (obj == Py_False) {
        return false;
    } else if (obj == Py_True) {
        return true;
    } else {
        stringstream ss;
        ss << "argument " << argname << " must be a boolean True or False";
        throw runtime_error(ss.str());
    }
}

uint32_t pydynd::pyarg_access_flags(PyObject* obj)
{
    pyobject_ownref iterator(PyObject_GetIter(obj));
    PyObject *item_raw;

    uint32_t result = 0;

    while ((item_raw = PyIter_Next(iterator))) {
        pyobject_ownref item(item_raw);
        result |= (uint32_t)pyarg_strings_to_int(item, "access_flags", 0,
                    "read", read_access_flag,
                    "write", write_access_flag,
                    "immutable", immutable_access_flag);
    }

    if (PyErr_Occurred()) {
        throw runtime_error("propagating exception...");
    }

    return result;
}
