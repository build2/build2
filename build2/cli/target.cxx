// file      : build2/cli/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cli/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cli
  {
    // cli
    //
    extern const char cli_ext_var[] = "extension";  // VC14 rejects constexpr.
    extern const char cli_ext_def[] = "cli";

    const target_type cli::static_type
    {
      "cli",
      &file::static_type,
      &target_factory<cli>,
      &target_extension_var<cli_ext_var, cli_ext_def>,
      &target_pattern_var<cli_ext_var, cli_ext_def>,
      nullptr,
      &file_search,
      false
    };

    // cli.cxx
    //
    group_view cli_cxx::
    group_members (action_type) const
    {
      static_assert (sizeof (cli_cxx_members) == sizeof (const target*) * 3,
                     "member layout incompatible with array");

      return h != nullptr
        ? group_view {reinterpret_cast<const target* const*> (&h),
                      (i != nullptr ? 3U : 2U)}
        : group_view {nullptr, 0};
    }

    static pair<target*, optional<string>>
    cli_cxx_factory (const target_type&,
                     dir_path d,
                     dir_path o,
                     string n,
                     optional<string> e)
    {
      tracer trace ("cli::cli_cxx_factory");

      // Pre-enter (potential) members as targets. The main purpose of doing
      // this is to avoid searching for existing files in src_base if the
      // buildfile mentions some of them explicitly as prerequisites.
      //
      targets.insert<cxx::hxx> (d, o, n, trace);
      targets.insert<cxx::cxx> (d, o, n, trace);
      targets.insert<cxx::ixx> (d, o, n, trace);

      return make_pair (new cli_cxx (move (d), move (o), move (n)), move (e));
    }

    const target_type cli_cxx::static_type
    {
      "cli.cxx",
      &mtime_target::static_type,
      &cli_cxx_factory,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      true // "See through" default iteration mode.
    };
  }
}
