/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2017, James R. Barlow (https://github.com/jbarlow83/)
 */

/* Intended for use as
 * #include "qpdf_pagelist.h" in qpdf.cpp
 */

#include <qpdf/Constants.h>
#include <qpdf/Types.h>
#include <qpdf/DLL.h>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFXRefEntry.hh>
#include <qpdf/PointerHolder.hh>
#include <qpdf/Buffer.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "pikepdf.h"


void assert_pyobject_is_page(py::handle obj)
{
    QPDFObjectHandle h;
    try {
        h = obj.cast<QPDFObjectHandle>();
    } catch (py::cast_error) {
        throw py::type_error("only pikepdf pages can be assigned to a page list");
    }
    if (!h.isPageObject()) {
        throw py::type_error("only pages can be assigned to a page list");
    }
}


class PageList {
public:
    size_t iterpos;

    PageList(QPDF &q, size_t iterpos = 0) : iterpos(iterpos), qpdf(q) {};

    QPDFObjectHandle getPage(ssize_t index) const
    {
        auto pages = this->qpdf.getAllPages();
        if (index < 0)
            index += pages.size();
        if (index < 0) // Still
            throw py::index_error("Accessing nonexistent PDF page number");
        size_t uindex = index;
        if (uindex < pages.size())
            return pages.at(uindex);
        throw py::index_error("Accessing nonexistent PDF page number");
    }

    py::list getPages(py::slice slice) 
    {
        size_t start, stop, step, slicelength;
        if (!slice.compute(this->count(), &start, &stop, &step, &slicelength))
            throw py::error_already_set();
        py::list result;
        for (size_t i = 0; i < slicelength; ++i) {
            QPDFObjectHandle oh = this->getPage(start);
            result.append(oh);
            start += step;
        }
        return result;
    }

    void setPage(size_t index, py::object page) 
    {
        this->insertPage(index, page);
        if (index != this->count()) {
            this->deletePage(index + 1);
        }
    }

    void setPagesFromIterable(py::slice slice, py::iterable other)
    {
        size_t start, stop, step, slicelength;
        if (!slice.compute(this->count(), &start, &stop, &step, &slicelength))
            throw py::error_already_set();
        py::list results;
        py::iterator it = other.attr("__iter__")();

        // Unpack list into iterable, check that each object is a page but
        // don't save the handles yet
        for(; it != py::iterator::sentinel(); ++it) {
            assert_pyobject_is_page(*it);
            results.append(*it);
        }

        if (step != 1) {
            // For an extended slice we must be replace an equal number of pages
            if (results.size() != slicelength) {
                throw py::value_error(
                    std::string("attempt to assign sequence of length ") +
                    std::to_string(results.size()) +
                    std::string(" to extended slice of size ") +
                    std::to_string(slicelength)
                );
            }
            for (size_t i = 0; i < slicelength; ++i) {
                this->setPage(start + (i * step), results[i]);
            }
        } else {
            // For simple slices, we can replace differing sizes
            // meaning results.size() could be slicelength, or not
            // so insert all pages first (to ensure nothing is freed yet)
            // and then delete all pages we no longer need

            // Insert first to ensure we don't delete any pages we will need
            for (size_t i = 0; i < results.size(); ++i) {
                this->insertPage(start + i, results[i]);
            }

            size_t del_start = start + results.size();
            for (size_t i = 0; i < slicelength; ++i) {
                this->deletePage(del_start);
            }
        }
    }

    void deletePage(size_t index)
    {
        auto page = this->getPage(index);
        /*
        // Need a dec_ref to match the inc_ref in insertPage, but it's unclear
        // how to do that. The item will be set the current QPDF always.
        // Accessing data from another PDF seems to involve some pipeline
        // magic in QPDF around libqpdf/QPDFWriter.cc:1614
        if (original page owner != &this->getQPDF()) {
            // If we are removing a page not originally owned by our QPDF,
            // remove the reference count we put it in insertPage()
            py::object pyqpdf = py::cast(page_owner);
            pyqpdf.dec_ref();
        }
        */
        this->qpdf.removePage(page);
    }

    size_t count() const
    {
        return this->qpdf.getAllPages().size();
    }

    void insertPage(size_t index, py::handle obj)
    {
        QPDFObjectHandle page;
        try {
            page = obj.cast<QPDFObjectHandle>();
        } catch (py::cast_error) {
            throw py::type_error("only pages can be inserted");
        }
        if (!page.isPageObject())
            throw py::type_error("only pages can be inserted");

        this->insertPage(index, page);
    }

    void insertPage(size_t index, QPDFObjectHandle page)
    {
        // Find out who owns us
        QPDF *page_owner = page.getOwningQPDF();

        if (page_owner == &this->getQPDF()) {
            // qpdf does not accept duplicating pages within the same file, 
            // so manually create a copy
            page = this->qpdf.makeIndirectObject(page);
        } else {
            // libqpdf does not transfer a page's contents to the new QPDF.
            // Instead WHEN ASKED TO WRITE it will go back and get the data
            // from objecthandle->getOwningQPDF(). Therefore we must ensure
            // our owner stays alive.
            py::object pyqpdf = py::cast(page_owner);
            pyqpdf.inc_ref();
        }

        if (index != this->count()) {
            QPDFObjectHandle refpage = this->getPage(index);
            this->qpdf.addPageAt(page, true, refpage);
        } else {
            this->qpdf.addPage(page, false);
        }
    }

    QPDF &getQPDF() { return qpdf; }

private:
    QPDF &qpdf;    
};


void init_pagelist(py::module &m)
{
    py::class_<PageList>(m, "PageList")
        .def("__getitem__", &PageList::getPage)
        .def("__getitem__", &PageList::getPages)
        .def("__setitem__", &PageList::setPage)
        .def("__setitem__", &PageList::setPagesFromIterable)
        .def("__delitem__", &PageList::deletePage)
        .def("__len__", &PageList::count)
        .def("p",
            [](PageList &pl, size_t index) {
                if (index == 0)
                    throw py::index_error("can't access page 0 in 1-based indexing");
                return pl.getPage(index - 1);
            },
            "convenience - look up page number in ordinal numbering, .p(1) is first page"
        )
        .def("__iter__",
            [](PageList &pl) {
                return PageList(pl.getQPDF(), 0);
            }
        )
        .def("__next__",
            [](PageList &pl) {
                if (pl.iterpos < pl.count())
                    return pl.getPage(pl.iterpos++);
                throw py::stop_iteration();
            }
        )
        .def("insert", 
            [](PageList &pl, size_t index, py::object obj) {
                pl.insertPage(index, obj);
            }, py::keep_alive<1, 3>()
        )
        .def("reverse", 
            [](PageList &pl) {
                py::slice ordinary_indices(0, pl.count(), 1);                
                py::int_ step(-1);
                PyObject *raw_slice = PySlice_New(Py_None, Py_None, step.ptr());
                py::slice reversed = py::reinterpret_steal<py::slice>(raw_slice);
                py::list reversed_pages = pl.getPages(reversed);
                pl.setPagesFromIterable(ordinary_indices, reversed_pages);
            }
        )
        .def("append",
            [](PageList &pl, py::object page) {
                pl.insertPage(pl.count(), page);
            },
            py::keep_alive<1, 2>()
        )
        .def("extend",
            [](PageList &pl, PageList &other) {
                size_t other_count = other.count();
                for (size_t i = 0; i < other_count; i++) {
                    if (other_count != other.count())
                        throw py::value_error("source page list modified during iteration");
                    pl.insertPage(pl.count(), other.getPage(i));
                }
            },
            py::keep_alive<1, 2>()
        )
        .def("extend",
            [](PageList &pl, py::iterable iterable) {
                py::iterator it = iterable.attr("__iter__")();
                while (it != py::iterator::sentinel()) {
                    assert_pyobject_is_page(*it);
                    pl.insertPage(pl.count(), *it);
                    ++it;
                }
            },
            py::keep_alive<1, 2>()
        );
}