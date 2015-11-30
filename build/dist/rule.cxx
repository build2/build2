// file      : build/dist/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/dist/rule>

#include <build/scope>
#include <build/target>
#include <build/algorithm>
#include <build/diagnostics>

using namespace std;

namespace build
{
  namespace dist
  {
    match_result rule::
    match (action, target& t, const std::string&) const
    {
      return t; // We always match.
    }

    recipe rule::
    apply (action a, target& t, const match_result&) const
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

        // If we can, go inside see-through groups.
        //
        if (p.type ().see_through && i.enter_group ())
          continue;

        target& pt (p.search ());

        // Don't match targets that are outside of our project.
        //
        if (pt.dir.sub (out_root))
          build::match (a, pt);
      }

      return noop_recipe; // We will never be executed.
    }
  }
}
