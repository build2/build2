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
    bool rule::
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

        // We used to always search and match but that resulted in the
        // undesirable behavior in case one of the "source" files is
        // missing. In this case we would enter a target as "output", this
        // rule would match it, and then dist_execute() would ignore it by
        // default.
        //
        // So now if this is a file target (we still want to always "see
        // through" other targets like aliases), we will only match it if (1)
        // it exists in src or (2) it exists as a target. It feels like we
        // don't need the stronger "... and not implied" condition since if it
        // is mentioned as a target, then it is in out (we don't do the same
        // target in both src/out).
        //
        const target* pt (nullptr);
        if (p.is_a<file> ())
        {
          pt = p.load ();

          if (pt == nullptr)
          {
            // Search for an existing target or existing file in src.
            //
            const prerequisite_key& k (p.key ());
            pt = k.tk.type->search (t, k);

            if (pt == nullptr)
              fail << "prerequisite " << k << " is not existing source file "
                   << "nor known output target" << endf;

            search_custom (p.prerequisite, *pt); // Cache.
          }
        }
        else
          pt = &p.search (t);

        // Don't match targets that are outside of our project.
        //
        if (pt->dir.sub (out_root))
          build2::match (a, *pt);
      }

      return noop_recipe; // We will never be executed.
    }
  }
}
