// file      : build2/cc/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/target>

using namespace std;

namespace build2
{
  namespace cc
  {
    const target_type cc::static_type
    {
      "cc",
      &file::static_type,
      nullptr,
      nullptr,
      nullptr,
      &search_target,
      false
    };

    extern const char ext_var[] = "extension";  // VC14 rejects constexpr.

    extern const char h_ext_def[] = "h";
    const target_type h::static_type
    {
      "h",
      &file::static_type,
      &target_factory<h>,
      &target_extension_var<ext_var, h_ext_def>,
      nullptr,
      &search_file,
      false
    };

    extern const char c_ext_def[] = "c";
    const target_type c::static_type
    {
      "c",
      &cc::static_type,
      &target_factory<c>,
      &target_extension_var<ext_var, c_ext_def>,
      nullptr,
      &search_file,
      false
    };
  }
}
