// file      : libbuild2/cli/target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cli/target.hxx>

#include <libbuild2/context.hxx>

namespace build2
{
  namespace cli
  {
    // cli
    //
    extern const char cli_ext_def[] = "cli";

    const target_type cli::static_type
    {
      "cli",
      &file::static_type,
      &target_factory<cli>,
      nullptr, /* fixed_extension */
      &target_extension_var<cli_ext_def>,
      &target_pattern_var<cli_ext_def>,
      nullptr,
      &file_search,
      target_type::flag::none
    };

    // cli.cxx
    //
    group_view cli_cxx::
    group_members (action) const
    {
      static_assert (sizeof (cli_cxx_members) == sizeof (const target*) * 3,
                     "member layout incompatible with array");

      return h != nullptr
        ? group_view {reinterpret_cast<const target* const*> (&h),
                      (i != nullptr ? 3U : 2U)}
        : group_view {nullptr, 0};
    }

    static target*
    cli_cxx_factory (context& ctx,
                     const target_type&, dir_path d, dir_path o, string n)
    {
      tracer trace ("cli::cli_cxx_factory");

      // Pre-enter (potential) members as targets. The main purpose of doing
      // this is to avoid searching for existing files in src_base if the
      // buildfile mentions some of them explicitly as prerequisites.
      //
      // Also required for the src-out remapping logic.
      //
      ctx.targets.insert<cxx::hxx> (d, o, n, trace);
      ctx.targets.insert<cxx::cxx> (d, o, n, trace);
      ctx.targets.insert<cxx::ixx> (d, o, n, trace);

      return new cli_cxx (ctx, move (d), move (o), move (n));
    }

    const target_type cli_cxx::static_type
    {
      "cli.cxx",
      &mtime_target::static_type,
      &cli_cxx_factory,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &target_search,
      target_type::flag::see_through // Group with "see through" iteration.
    };
  }
}
