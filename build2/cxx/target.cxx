// file      : build2/cxx/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/target.hxx>

#include <build2/context.hxx>

using namespace std;

namespace build2
{
  namespace cxx
  {
    extern const char hxx_ext_def[] = "hxx";
    const target_type hxx::static_type
    {
      "hxx",
      &cc::static_type,
      &target_factory<hxx>,
      nullptr, /* fixed_extension */
      &target_extension_var<var_extension, hxx_ext_def>,
      &target_pattern_var<var_extension, hxx_ext_def>,
      nullptr,
      &file_search,
      false
    };

    extern const char ixx_ext_def[] = "ixx";
    const target_type ixx::static_type
    {
      "ixx",
      &cc::static_type,
      &target_factory<ixx>,
      nullptr, /* fixed_extension */
      &target_extension_var<var_extension, ixx_ext_def>,
      &target_pattern_var<var_extension, ixx_ext_def>,
      nullptr,
      &file_search,
      false
    };

    extern const char txx_ext_def[] = "txx";
    const target_type txx::static_type
    {
      "txx",
      &cc::static_type,
      &target_factory<txx>,
      nullptr, /* fixed_extension */
      &target_extension_var<var_extension, txx_ext_def>,
      &target_pattern_var<var_extension, txx_ext_def>,
      nullptr,
      &file_search,
      false
    };

    extern const char cxx_ext_def[] = "cxx";
    const target_type cxx::static_type
    {
      "cxx",
      &cc::static_type,
      &target_factory<cxx>,
      nullptr, /* fixed_extension */
      &target_extension_var<var_extension, cxx_ext_def>,
      &target_pattern_var<var_extension, cxx_ext_def>,
      nullptr,
      &file_search,
      false
    };

    extern const char mxx_ext_def[] = "mxx";
    const target_type mxx::static_type
    {
      "mxx",
      &cc::static_type,
      &target_factory<mxx>,
      nullptr, /* fixed_extension */
      &target_extension_var<var_extension, mxx_ext_def>,
      &target_pattern_var<var_extension, mxx_ext_def>,
      nullptr,
      &file_search,
      false
    };
  }
}
