// file      : build2/bash/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bash/target.hxx>

#include <build2/context.hxx>

using namespace std;

namespace build2
{
  namespace bash
  {
    extern const char bash_ext_def[] = "bash";

    const target_type bash::static_type
    {
      "bash",
      &file::static_type,
      &target_factory<bash>,
      nullptr, /* fixed_extension */
      &target_extension_var<var_extension, bash_ext_def>,
      &target_pattern_var<var_extension, bash_ext_def>,
      nullptr,
      &file_search,
      false
    };
  }
}
