// file      : build2/cxx/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/target>

using namespace std;

namespace build2
{
  namespace cxx
  {
    extern const char ext_var[] = "extension";  // VC14 rejects constexpr.

    extern const char hxx_ext_def[] = "hxx";
    const target_type hxx::static_type
    {
      "hxx",
      &file::static_type,
      &target_factory<hxx>,
      &target_extension_var<ext_var, hxx_ext_def>,
      nullptr,
      &search_file,
      false
    };

    extern const char ixx_ext_def[] = "ixx";
    const target_type ixx::static_type
    {
      "ixx",
      &file::static_type,
      &target_factory<ixx>,
      &target_extension_var<ext_var, ixx_ext_def>,
      nullptr,
      &search_file,
      false
    };

    extern const char txx_ext_def[] = "txx";
    const target_type txx::static_type
    {
      "txx",
      &file::static_type,
      &target_factory<txx>,
      &target_extension_var<ext_var, txx_ext_def>,
      nullptr,
      &search_file,
      false
    };

    extern const char cxx_ext_def[] = "cxx";
    const target_type cxx::static_type
    {
      "cxx",
      &cc::static_type,
      &target_factory<cxx>,
      &target_extension_var<ext_var, cxx_ext_def>,
      nullptr,
      &search_file,
      false
    };
  }
}
