// file      : libbuild2/cc/target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/target.hxx>

#include <libbuild2/context.hxx>

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
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::none
    };

    extern const char h_ext_def[] = "h";
    const target_type h::static_type
    {
      "h",
      &cc::static_type,
      &target_factory<h>,
      nullptr, /* fixed_extension */
      &target_extension_var<h_ext_def>,
      &target_pattern_var<h_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    extern const char c_ext_def[] = "c";
    const target_type c::static_type
    {
      "c",
      &cc::static_type,
      &target_factory<c>,
      nullptr, /* fixed_extension */
      &target_extension_var<c_ext_def>,
      &target_pattern_var<c_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    extern const char m_ext_def[] = "m";
    const target_type m::static_type
    {
      "m",
      &cc::static_type,
      &target_factory<m>,
      nullptr, /* fixed_extension */
      &target_extension_var<m_ext_def>,
      &target_pattern_var<m_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    extern const char S_ext_def[] = "S";
    const target_type S::static_type
    {
      "S",
      &cc::static_type,
      &target_factory<S>,
      nullptr, /* fixed_extension */
      &target_extension_var<S_ext_def>,
      &target_pattern_var<S_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    const target_type c_inc::static_type
    {
      "c_inc",
      &cc::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::none
    };

    extern const char pc_ext[] = "pc"; // VC14 rejects constexpr.
    const target_type pc::static_type
    {
      "pc",
      &file::static_type,
      &target_factory<pc>,
      &target_extension_fix<pc_ext>,
      nullptr, /* default_extension */
      &target_pattern_fix<pc_ext>,
      &target_print_0_ext_verb, // Fixed extension, no use printing.
      &file_search,
      target_type::flag::none
    };

    extern const char pca_ext[] = "static.pc"; // VC14 rejects constexpr.
    const target_type pca::static_type
    {
      "pca",
      &pc::static_type,
      &target_factory<pca>,
      &target_extension_fix<pca_ext>,
      nullptr, /* default_extension */
      &target_pattern_fix<pca_ext>,
      &target_print_0_ext_verb, // Fixed extension, no use printing.
      &file_search,
      target_type::flag::none
    };

    extern const char pcs_ext[] = "shared.pc"; // VC14 rejects constexpr.
    const target_type pcs::static_type
    {
      "pcs",
      &pc::static_type,
      &target_factory<pcs>,
      &target_extension_fix<pcs_ext>,
      nullptr, /* default_extension */
      &target_pattern_fix<pcs_ext>,
      &target_print_0_ext_verb, // Fixed extension, no use printing.
      &file_search,
      target_type::flag::none
    };
  }
}
