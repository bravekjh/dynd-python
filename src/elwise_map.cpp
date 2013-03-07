//
// Copyright (C) 2011-13, DyND Developers
// BSD 2-Clause License, see LICENSE.txt
//

#include <Python.h>

#include <iostream>

#include <dynd/dtypes/expr_dtype.hpp>
#include <dynd/dtypes/unary_expr_dtype.hpp>
#include <dynd/dtypes/strided_dim_dtype.hpp>
#include <dynd/dtypes/var_dim_dtype.hpp>
#include <dynd/kernels/expr_kernel_generator.hpp>
#include <dynd/kernels/elwise_expr_kernels.hpp>
#include <dynd/shape_tools.hpp>

#include "utility_functions.hpp"
#include "elwise_map.hpp"
#include "ndobject_functions.hpp"
#include "dtype_functions.hpp"

using namespace std;
using namespace dynd;
using namespace pydynd;

namespace {
    struct pyobject_expr_kernel_extra {
        typedef pyobject_expr_kernel_extra extra_type;

        kernel_data_prefix base;
        size_t src_count;
        PyObject *callable;
        // After this are 1 + src_count shell WNDObjects,
        // whose guts are replaced for each call of
        // the kernel

        inline PyObject *set_data_pointers(char *dst, const char * const *src)
        {
            WNDObject **ndo = reinterpret_cast<WNDObject **>(this + 1);
            // Modify the temporary ndobjects to point at the data.
            // The constructor sets the metadata to be a size-1 array,
            // so no need to modify any of it.
            ndo[0]->v.get_ndo()->m_data_pointer = dst;
            for (size_t i = 0; i != src_count; ++i) {
                ndo[i+1]->v.get_ndo()->m_data_pointer = const_cast<char *>(src[i]);
            }

            // Put all the ndobjects in a tuple
            pyobject_ownref args(PyTuple_New(src_count + 1));
            for (size_t i = 0; i != src_count + 1; ++i) {
                Py_INCREF(ndo[i]);
                PyTuple_SET_ITEM(args.get(), i, (PyObject *)ndo[i]);
            }
            return args.release();
        }

        inline PyObject *set_data_pointers(char *dst, intptr_t dst_stride,
                        const char * const *src, const intptr_t *src_stride,
                        size_t count)
        {
            WNDObject **ndo = reinterpret_cast<WNDObject **>(this + 1);
            strided_dim_dtype_metadata *md;
            // Modify the temporary ndobjects to point at the data.
            ndo[0]->v.get_ndo()->m_data_pointer = dst;
            md = reinterpret_cast<strided_dim_dtype_metadata *>(ndo[0]->v.get_ndo_meta());
            md->size = count;
            md->stride = dst_stride;
            for (size_t i = 0; i != src_count; ++i) {
                ndo[i+1]->v.get_ndo()->m_data_pointer = const_cast<char *>(src[i]);
                md = reinterpret_cast<strided_dim_dtype_metadata *>(ndo[i+1]->v.get_ndo_meta());
                md->size = count;
                md->stride = src_stride[i];
            }

            // Put all the ndobjects in a tuple
            pyobject_ownref args(PyTuple_New(src_count + 1));
            for (size_t i = 0; i != src_count + 1; ++i) {
                Py_INCREF(ndo[i]);
                PyTuple_SET_ITEM(args.get(), i, (PyObject *)ndo[i]);
            }
            return args.release();
        }

        inline void verify_postcall_consistency(PyObject *res)
        {
            WNDObject **ndo = reinterpret_cast<WNDObject **>(this + 1);
            // Verify that nothing was returned
            if (res != Py_None) {
                throw runtime_error("Python callable for elwise_map must not return a value, got an object");
            }
            // Verify that no reference to a temporary ndobject was kept
            for (size_t i = 0; i != src_count + 1; ++i) {
                if (ndo[i]->ob_refcnt != 1) {
                    stringstream ss;
                    ss << "The elwise_map callable function held onto a reference to the ";
                    if (i == 0) {
                        ss << "dst";
                    } else {
                        ss << "src_" << i-1 << "";
                    }
                    ss << " argument, this is disallowed";
                    throw runtime_error(ss.str());
                } else if (ndo[i]->v.get_ndo()->m_memblockdata.m_use_count != 1) {
                    stringstream ss;
                    ss << "The elwise_map callable function held onto a reference to the data underlying the ";
                    if (i == 0) {
                        ss << "dst";
                    } else {
                        ss << "src_" << i-1 << "";
                    }
                    ss << " argument, this is disallowed";
                    throw runtime_error(ss.str());
                }
            }
        }

        static void single_unary(char *dst, const char *src,
                        kernel_data_prefix *extra)
        {
            PyGILState_RAII pgs;

            extra_type *e = reinterpret_cast<extra_type *>(extra);
            pyobject_ownref args(e->set_data_pointers(dst, &src));
            // Call the function
            pyobject_ownref res(PyObject_Call(e->callable, args.get(), NULL));
            args.clear();
            e->verify_postcall_consistency(res.get());
        }

        static void single(char *dst, const char * const *src,
                        kernel_data_prefix *extra)
        {
            PyGILState_RAII pgs;

            extra_type *e = reinterpret_cast<extra_type *>(extra);
            pyobject_ownref args(e->set_data_pointers(dst, src));
            // Call the function
            pyobject_ownref res(PyObject_Call(e->callable, args.get(), NULL));
            args.clear();
            e->verify_postcall_consistency(res.get());
        }

        static void strided_unary(char *dst, intptr_t dst_stride,
                    const char *src, intptr_t src_stride,
                    size_t count, kernel_data_prefix *extra)
        {
            PyGILState_RAII pgs;

            extra_type *e = reinterpret_cast<extra_type *>(extra);
            // Put all the ndobjects in a tuple
            pyobject_ownref args(e->set_data_pointers(dst, dst_stride, &src, &src_stride, count));
            // Call the function
            pyobject_ownref res(PyObject_Call(e->callable, args.get(), NULL));
            args.clear();
            e->verify_postcall_consistency(res.get());
        }

        static void strided(char *dst, intptr_t dst_stride,
                    const char * const *src, const intptr_t *src_stride,
                    size_t count, kernel_data_prefix *extra)
        {
            PyGILState_RAII pgs;

            extra_type *e = reinterpret_cast<extra_type *>(extra);
            // Put all the ndobjects in a tuple
            pyobject_ownref args(e->set_data_pointers(dst, dst_stride, src, src_stride, count));
            // Call the function
            pyobject_ownref res(PyObject_Call(e->callable, args.get(), NULL));
            args.clear();
            e->verify_postcall_consistency(res.get());
        }

        static void destruct(kernel_data_prefix *extra)
        {
            PyGILState_RAII pgs;

            extra_type *e = reinterpret_cast<extra_type *>(extra);
            WNDObject **ndo = reinterpret_cast<WNDObject **>(e + 1);
            size_t src_count = e->src_count;
            Py_XDECREF(e->callable);
            for (size_t i = 0; i != src_count + 1; ++i) {
                Py_XDECREF(ndo[i]);
            }
        }
    };
} // anonymous namespace

class pyobject_elwise_expr_kernel_generator : public expr_kernel_generator {
    pyobject_ownref m_callable;
    dtype m_dst_dt;
    vector<dtype> m_src_dt;
public:
    pyobject_elwise_expr_kernel_generator(PyObject *callable,
                    const dtype& dst_dt, const std::vector<dtype>& src_dt)
        : expr_kernel_generator(true), m_callable(callable, true),
                        m_dst_dt(dst_dt), m_src_dt(src_dt)
    {
    }

    pyobject_elwise_expr_kernel_generator(PyObject *callable,
                    const dtype& dst_dt, const dtype& src_dt)
        : expr_kernel_generator(true), m_callable(callable, true),
                        m_dst_dt(dst_dt), m_src_dt(1)
    {
        m_src_dt[0] = src_dt;
    }

    virtual ~pyobject_elwise_expr_kernel_generator() {
    }

    size_t make_expr_kernel(
                hierarchical_kernel *out, size_t offset_out,
                const dtype& dst_dt, const char *dst_metadata,
                size_t src_count, const dtype *src_dt, const char **src_metadata,
                kernel_request_t kernreq, const eval::eval_context *ectx) const
    {
        if (src_count != m_src_dt.size()) {
            stringstream ss;
            ss << "This elwise_map kernel requires " << m_src_dt.size() << " src operands, ";
            ss << "received " << src_count;
            throw runtime_error(ss.str());
        }
        bool require_elwise = dst_dt != m_dst_dt;
        if (require_elwise) {
            for (size_t i = 0; i != src_count; ++i) {
                if (src_dt[i] != m_src_dt[i]) {
                    require_elwise = true;
                    break;
                }
            }
        }
        // If the dtypes don't match the ones for this generator,
        // call the elementwise dimension handler to handle one dimension,
        // giving 'this' as the next kernel generator to call
        if (require_elwise) {
            return make_elwise_dimension_expr_kernel(out, offset_out,
                            dst_dt, dst_metadata,
                            src_count, src_dt, src_metadata,
                            kernreq, ectx,
                            this);
        }

        size_t extra_size = sizeof(pyobject_expr_kernel_extra) +
                        (src_count + 1) * sizeof(WNDObject *);
        out->ensure_capacity_leaf(offset_out + extra_size);
        pyobject_expr_kernel_extra *e = out->get_at<pyobject_expr_kernel_extra>(offset_out);
        WNDObject **ndo = reinterpret_cast<WNDObject **>(e + 1);
        switch (kernreq) {
            case kernel_request_single:
                if (src_count == 1) {
                    // Unary kernels are special-cased to be the same as assignment kernels
                    e->base.set_function<unary_single_operation_t>(&pyobject_expr_kernel_extra::single_unary);
                } else {
                    e->base.set_function<expr_single_operation_t>(&pyobject_expr_kernel_extra::single);
                }
                break;
            case kernel_request_strided:
                if (src_count == 1) {
                    // Unary kernels are special-cased to be the same as assignment kernels
                    e->base.set_function<unary_strided_operation_t>(&pyobject_expr_kernel_extra::strided_unary);
                } else {
                    e->base.set_function<expr_strided_operation_t>(&pyobject_expr_kernel_extra::strided);
                }
                break;
            default: {
                stringstream ss;
                ss << "pyobject_elwise_expr_kernel_generator: unrecognized request " << (int)kernreq;
                throw runtime_error(ss.str());
            }
        }
        e->base.destructor = &pyobject_expr_kernel_extra::destruct;
        e->src_count = src_count;
        e->callable = m_callable.get();
        Py_INCREF(e->callable);
        // Create shell WNDObjects which are used to give the kernel data to Python
        strided_dim_dtype_metadata *md;
        dtype dt = make_strided_dim_dtype(dst_dt);
        ndobject n(make_ndobject_memory_block(dt.get_metadata_size()));
        n.get_ndo()->m_dtype = dt.release();
        n.get_ndo()->m_flags = write_access_flag;
        md = reinterpret_cast<strided_dim_dtype_metadata *>(n.get_ndo_meta());
        md->size = 1;
        md->stride = 0;
        if (dst_dt.get_metadata_size() > 0) {
            dst_dt.extended()->metadata_copy_construct(
                            n.get_ndo_meta() + sizeof(strided_dim_dtype_metadata),
                            dst_metadata, NULL);
        }
        ndo[0] = (WNDObject *)wrap_ndobject(DYND_MOVE(n));
        for (size_t i = 0; i != src_count; ++i) {
            dt = make_strided_dim_dtype(src_dt[i]);
            n.set(make_ndobject_memory_block(dt.get_metadata_size()));
            n.get_ndo()->m_dtype = dt.release();
            n.get_ndo()->m_flags = read_access_flag;
            md = reinterpret_cast<strided_dim_dtype_metadata *>(n.get_ndo_meta());
            md->size = 1;
            md->stride = 0;
            if (src_dt[i].get_metadata_size() > 0) {
                src_dt[i].extended()->metadata_copy_construct(
                                n.get_ndo_meta() + sizeof(strided_dim_dtype_metadata),
                                src_metadata[i], NULL);
            }
            ndo[i+1] = (WNDObject *)wrap_ndobject(DYND_MOVE(n));
        }

        return offset_out + extra_size;
    }

    void print_dtype(std::ostream& o) const
    {
        PyGILState_RAII pgs;

        PyObject *name_obj = PyObject_GetAttrString(m_callable.get(), "__name__");
        if (name_obj != NULL) {
            pyobject_ownref name(name_obj);
            o << pystring_as_string(name.get());
        } else {
            PyErr_Clear();
            o << "_unnamed";
        }
        o << "(op0";
        for (size_t i = 1; i != m_src_dt.size(); ++i) {
            o << ", op" << i;
        }
        o << ")";
    }
};

static PyObject *unary_elwise_map(PyObject *n_obj, PyObject *callable,
                PyObject *dst_type, PyObject *src_type)
{
    ndobject n = ndobject_from_py(n_obj);
    if (n.get_ndo() == NULL) {
        throw runtime_error("elwise_map received a NULL ndobject");
    }

    dtype dst_dt, src_dt;

    dst_dt = make_dtype_from_pyobject(dst_type);
    if (src_type != Py_None) {
        // Cast to the source dtype if requested
        src_dt = make_dtype_from_pyobject(src_type);
        n = n.cast_udtype(src_dt);
    } else {
        src_dt = n.get_udtype();
    }

    dtype edt = make_unary_expr_dtype(dst_dt, src_dt,
                    new pyobject_elwise_expr_kernel_generator(callable, dst_dt, src_dt.value_dtype()));
    return wrap_ndobject(n.replace_udtype(edt));
}

static PyObject *general_elwise_map(PyObject *n_list, PyObject *callable,
                PyObject *dst_type, PyObject *src_type_list)
{
    vector<ndobject> n(PyList_Size(n_list));
    for (size_t i = 0; i != n.size(); ++i) {
        n[i] = ndobject_from_py(PyList_GET_ITEM(n_list, i));
        if (n[i].get_ndo() == NULL) {
            throw runtime_error("elwise_map received a NULL ndobject");
        }
    }

    dtype dst_dt;
    vector<dtype> src_dt(n.size());

    dst_dt = make_dtype_from_pyobject(dst_type);
    if (src_type_list != Py_None) {
        for (size_t i = 0; i != n.size(); ++i) {
            // Cast to the source dtype if requested
            src_dt[i] = make_dtype_from_pyobject(PyList_GET_ITEM(src_type_list, i));
            n[i] = n[i].cast_udtype(src_dt[i]);
        }
    } else {
        for (size_t i = 0; i != n.size(); ++i) {
            src_dt[i] = n[i].get_udtype();
        }
    }

    size_t undim = 0;
    for (size_t i = 0; i != n.size(); ++i) {
        size_t undim_i = n[i].get_undim();
        if (undim_i > undim) {
            undim = undim_i;
        }
    }
    dimvector result_shape(undim), tmp_shape(undim);
    for (size_t j = 0; j != undim; ++j) {
        result_shape[j] = 1;
    }
    for (size_t i = 0; i != n.size(); ++i) {
        size_t undim_i = n[i].get_undim();
        if (undim_i > 0) {
            n[i].get_shape(tmp_shape.get());
            incremental_broadcast(undim, result_shape.get(), undim_i, tmp_shape.get());
        }
    }

    dtype result_vdt = dst_dt;
    for (size_t j = 0; j != undim; ++j) {
        if (result_shape[undim - j - 1] == -1) {
            result_vdt = make_var_dim_dtype(result_vdt);
        } else {
            result_vdt = make_strided_dim_dtype(result_vdt);
        }
    }

    // Create the result
    vector<string> field_names(n.size());
    for (size_t i = 0; i != n.size(); ++i) {
        stringstream ss;
        ss << "arg" << i;
        field_names[i] = ss.str();
    }
    ndobject result = combine_into_struct(n.size(), &field_names[0], &n[0]);

    // Because the expr dtype's operand is the result's dtype,
    // we can swap it in as the dtype
    dtype edt = make_expr_dtype(result_vdt,
                    result.get_dtype(),
                    new pyobject_elwise_expr_kernel_generator(callable, dst_dt, src_dt));
    edt.swap(result.get_ndo()->m_dtype);
    return wrap_ndobject(DYND_MOVE(result));
}

PyObject *pydynd::elwise_map(PyObject *n_obj, PyObject *callable,
                PyObject *dst_type, PyObject *src_type)
{
    if (!PyList_Check(n_obj)) {
        PyErr_SetString(PyExc_TypeError, "First parameter to elwise_map, 'n', "
                        "is not a list of dynd ndobjects");
        return NULL;
    }
    if (src_type != Py_None) {
        if (!PyList_Check(src_type)) {
            PyErr_SetString(PyExc_TypeError, "Fourth parameter to elwise_map, 'src_type', "
                            "is not a list of dynd types");
            return NULL;
        }
    }
    if (PyList_Size(n_obj) == 1) {
        return unary_elwise_map(PyList_GET_ITEM(n_obj, 0), callable, dst_type,
                        src_type == Py_None ? Py_None : PyList_GET_ITEM(src_type, 0));
    } else {
        return general_elwise_map(n_obj, callable, dst_type, src_type);
    }
}