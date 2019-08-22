// file      : libbuild2/config/operation.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CONFIG_OPERATION_HXX
#define LIBBUILD2_CONFIG_OPERATION_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/operation.hxx>

namespace build2
{
  namespace config
  {
    extern const meta_operation_info mo_configure;
    extern const meta_operation_info mo_disfigure;

    const string&
    preprocess_create (context&,
                       values&,
                       vector_view<opspec>&,
                       bool,
                       const location&);
  }
}

#endif // LIBBUILD2_CONFIG_OPERATION_HXX
