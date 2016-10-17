// file      : build2/dist/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/dist/rule>

#include <build2/scope>
#include <build2/target>
#include <build2/algorithm>
#include <build2/diagnostics>

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

      auto r (group_prerequisite_members (a, t, false));
      for (auto i (r.begin ()); i != r.end (); ++i)
      {
        prerequisite_member p (*i);

        // Skip prerequisites imported from other projects.
        //
        if (p.proj () != nullptr)
          continue;

        // If we can, go inside see-through groups. Note that here we are
        // not going into ad hoc groups but maybe we should (which would
        // have to be done after match()).
        //
        if (p.type ().see_through && i.enter_group ())
          continue;

        target& pt (p.search ());

        // Don't match targets that are outside of our project.
        //
        if (pt.dir.sub (out_root))
          build2::match (a, pt);
      }

      return noop_recipe; // We will never be executed.
    }
  }
}
