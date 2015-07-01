// file      : build/cli/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cli/target>

#include <butl/filesystem>

using namespace std;
using namespace butl;

namespace build
{
  namespace cli
  {
    // cli
    //
    constexpr const char cli_ext[] = "cli";
    const target_type cli::static_type
    {
      typeid (cli),
      "cli",
      &file::static_type,
      &target_factory<cli>,
      &target_extension_fix<cli_ext>,
      &search_file,
      false
    };

    // cli.cxx
    //
    group_view cli_cxx::
    group_members (action) const
    {
      return m[0] != nullptr
        ? group_view {m, (m[2] != nullptr ? 3U : 2U)}
        : group_view {nullptr, 0};
    }

    timestamp cli_cxx::
    load_mtime () const
    {
      // The rule has been matched which means the members should
      // be resolved and paths assigned.
      //
      return file_mtime (h ()->path ());
    }

    const target_type cli_cxx::static_type
    {
      typeid (cli_cxx),
      "cli.cxx",
      &mtime_target::static_type,
      &target_factory<cli_cxx>,
      nullptr,
      &search_target,
      true // See through default semantics.
    };
  }
}
