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
    group_members (action_type) const
    {
      return h != nullptr
        ? group_view {m, (i != nullptr ? 3U : 2U)}
        : group_view {nullptr, 0};
    }

    timestamp cli_cxx::
    load_mtime () const
    {
      // The rule has been matched which means the members should
      // be resolved and paths assigned.
      //
      return file_mtime (h->path ());
    }

    static target*
    cli_cxx_factory (const target_type&, dir_path d, string n, const string* e)
    {
      tracer trace ("cli::cli_cxx_factory");

      // Pre-enter (potential) members as targets. The main purpose
      // of doing this is to avoid searching for existing files in
      // src_base if the buildfile mentions some of them explicitly
      // as prerequisites.
      //
      targets.insert<cxx::hxx> (d, n, trace);
      targets.insert<cxx::cxx> (d, n, trace);
      targets.insert<cxx::ixx> (d, n, trace);

      return new cli_cxx (move (d), move (n), e);
    }

    const target_type cli_cxx::static_type
    {
      "cli.cxx",
      &mtime_target::static_type,
      &cli_cxx_factory,
      nullptr,
      &search_target,
      true // "See through" default iteration mode.
    };
  }
}
