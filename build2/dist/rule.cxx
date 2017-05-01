// file      : build2/dist/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/dist/rule.hxx>

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/algorithm.hxx>
#include <build2/diagnostics.hxx>

using namespace std;

namespace build2
{
  namespace dist
  {
    match_result rule::
    match (action, target&, const string&) const
    {
      return true; // We always match.
    }

    recipe rule::
    apply (action a, target& t) const
    {
      const dir_path& out_root (t.root_scope ().out_path ());

      // If we can, go inside see-through groups.
      //
      for (prerequisite_member p:
             group_prerequisite_members (a, t, members_mode::maybe))
      {
        // Skip prerequisites imported from other projects.
        //
        if (p.proj ())
          continue;

        const target& pt (p.search (t));

        // Don't match targets that are outside of our project.
        //
        if (pt.dir.sub (out_root))
          build2::match (a, pt);
      }

      return noop_recipe; // We will never be executed.
    }
  }
}
