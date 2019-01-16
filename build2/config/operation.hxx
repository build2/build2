// file      : build2/config/operation.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CONFIG_OPERATION_HXX
#define BUILD2_CONFIG_OPERATION_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/operation.hxx>

namespace build2
{
  namespace config
  {
    extern const meta_operation_info mo_configure;
    extern const meta_operation_info mo_disfigure;

    const string&
    preprocess_create (const variable_overrides&,
                       values&,
                       vector_view<opspec>&,
                       bool,
                       const location&);
  }
}

#endif // BUILD2_CONFIG_OPERATION_HXX
