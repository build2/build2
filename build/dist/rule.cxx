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

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        // Skip prerequisites imported from other projects.
        //
        if (p.proj () != nullptr)
          continue;

        // @@ This is where we will handle dist/nodist.

        target& pt (p.search ());

        // Don't match targets that are outside our project.
        //
        if (pt.dir.sub (out_root))
          build::match (a, pt);
      }

      return noop_recipe; // We will never be executed.
    }
  }
}
