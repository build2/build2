// file      : build2/pkgconfig/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/pkgconfig/target.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace pkgconfig
  {
    const target_type pc::static_type
    {
      "pc",
      &file::static_type,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      false
    };

    extern const char pca_ext[] = "static.pc"; // VC14 rejects constexpr.

    const target_type pca::static_type
    {
      "pca",
      &pc::static_type,
      &file_factory<pca, pca_ext>,
      &target_extension_fix<pca_ext>,
      &target_pattern_fix<pca_ext>,
      &target_print_0_ext_verb, // Fixed extension, no use printing.
      &file_search,
      false
    };

    extern const char pcs_ext[] = "shared.pc"; // VC14 rejects constexpr.

    const target_type pcs::static_type
    {
      "pcs",
      &pc::static_type,
      &file_factory<pcs, pcs_ext>,
      &target_extension_fix<pcs_ext>,
      &target_pattern_fix<pcs_ext>,
      &target_print_0_ext_verb, // Fixed extension, no use printing.
      &file_search,
      false
    };
  }
}
