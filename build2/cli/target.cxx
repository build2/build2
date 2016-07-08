// file      : build2/cli/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cli/target>

#include <build2/filesystem> // file_mtime()

using namespace std;
using namespace butl;

namespace build2
{
  namespace cli
  {
    // cli
    //
    extern const char cli_ext_var[] = "extension";  // VC 19 rejects constexpr.
    extern const char cli_ext_def[] = "cli";

    const target_type cli::static_type
    {
      "cli",
      &file::static_type,
      &target_factory<cli>,
      &target_extension_var<cli_ext_var, cli_ext_def>,
      nullptr,
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
    cli_cxx_factory (const target_type&,
                     dir_path d,
                     dir_path o,
                     string n,
                     const string* e)
    {
      tracer trace ("cli::cli_cxx_factory");

      // Pre-enter (potential) members as targets. The main purpose
      // of doing this is to avoid searching for existing files in
      // src_base if the buildfile mentions some of them explicitly
      // as prerequisites.
      //
      targets.insert<cxx::hxx> (d, o, n, trace);
      targets.insert<cxx::cxx> (d, o, n, trace);
      targets.insert<cxx::ixx> (d, o, n, trace);

      return new cli_cxx (move (d), move (o), move (n), e);
    }

    const target_type cli_cxx::static_type
    {
      "cli.cxx",
      &mtime_target::static_type,
      &cli_cxx_factory,
      nullptr,
      nullptr,
      &search_target,
      true // "See through" default iteration mode.
    };
  }
}
