// file      : build/cxx/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/target>

using namespace std;

namespace build
{
  namespace cxx
  {
    constexpr const char hxx_ext_var[] = "hxx.ext";
    const target_type hxx::static_type
    {
      "hxx",
      &file::static_type,
      &target_factory<hxx>,
      &target_extension_var<hxx_ext_var>,
      &search_file,
      false
    };

    constexpr const char ixx_ext_var[] = "ixx.ext";
    const target_type ixx::static_type
    {
      "ixx",
      &file::static_type,
      &target_factory<ixx>,
      &target_extension_var<ixx_ext_var>,
      &search_file,
      false
    };

    constexpr const char txx_ext_var[] = "txx.ext";
    const target_type txx::static_type
    {
      "txx",
      &file::static_type,
      &target_factory<txx>,
      &target_extension_var<txx_ext_var>,
      &search_file,
      false
    };

    constexpr const char cxx_ext_var[] = "cxx.ext";
    const target_type cxx::static_type
    {
      "cxx",
      &file::static_type,
      &target_factory<cxx>,
      &target_extension_var<cxx_ext_var>,
      &search_file,
      false
    };

    constexpr const char h_ext_var[] = "h.ext";
    const target_type h::static_type
    {
      "h",
      &file::static_type,
      &target_factory<h>,
      &target_extension_var<h_ext_var>,
      &search_file,
      false
    };

    constexpr const char c_ext_var[] = "c.ext";
    const target_type c::static_type
    {
      "c",
      &file::static_type,
      &target_factory<c>,
      &target_extension_var<c_ext_var>,
      &search_file,
      false
    };
  }
}
