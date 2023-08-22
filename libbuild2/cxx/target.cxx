// file      : libbuild2/cxx/target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cxx/target.hxx>

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
      &target_extension_var<hxx_ext_def>,
      &target_pattern_var<hxx_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    extern const char ixx_ext_def[] = "ixx";
    const target_type ixx::static_type
    {
      "ixx",
      &cc::static_type,
      &target_factory<ixx>,
      nullptr, /* fixed_extension */
      &target_extension_var<ixx_ext_def>,
      &target_pattern_var<ixx_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    extern const char txx_ext_def[] = "txx";
    const target_type txx::static_type
    {
      "txx",
      &cc::static_type,
      &target_factory<txx>,
      nullptr, /* fixed_extension */
      &target_extension_var<txx_ext_def>,
      &target_pattern_var<txx_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    extern const char cxx_ext_def[] = "cxx";
    const target_type cxx::static_type
    {
      "cxx",
      &cc::static_type,
      &target_factory<cxx>,
      nullptr, /* fixed_extension */
      &target_extension_var<cxx_ext_def>,
      &target_pattern_var<cxx_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    extern const char mxx_ext_def[] = "mxx";
    const target_type mxx::static_type
    {
      "mxx",
      &cc::static_type,
      &target_factory<mxx>,
      nullptr, /* fixed_extension */
      &target_extension_var<mxx_ext_def>,
      &target_pattern_var<mxx_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    extern const char mm_ext_def[] = "mm";
    const target_type mm::static_type
    {
      "mm",
      &cc::static_type,
      &target_factory<mm>,
      nullptr, /* fixed_extension */
      &target_extension_var<mm_ext_def>,
      &target_pattern_var<mm_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    const target_type cxx_inc::static_type
    {
      "cxx_inc",
      &cc::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::none
    };
  }
}
