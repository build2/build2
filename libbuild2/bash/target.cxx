// file      : libbuild2/bash/target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bash/target.hxx>

#include <libbuild2/context.hxx>

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
      &target_extension_var<bash_ext_def>,
      &target_pattern_var<bash_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };
  }
}
