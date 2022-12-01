// file      : libbuild2/dist/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/dist/rule.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/dist/types.hxx>
#include <libbuild2/dist/module.hxx>

using namespace std;

namespace build2
{
  namespace dist
  {
    bool rule::
    match (action, target&) const
    {
      return true; // We always match.
    }

    recipe rule::
    apply (action a, target& t) const
    {
      const scope& rs (t.root_scope ());
      const dir_path& src_root (rs.src_path ());
      const dir_path& out_root (rs.out_path ());

      // If we can, go inside see-through groups.
      //
      for (prerequisite_member pm:
             group_prerequisite_members (a, t, members_mode::maybe))
      {
        // Note: no exclusion tests, we want all of them (and see also the
        // dist_include() override). But if we don't ignore post hoc ones
        // here, we will end up with a cycle (they will still be handled
        // by the post-pass).
        //
        lookup l; // Ignore any operation-specific values.
        if (include (a, t, pm, &l) == include_type::posthoc)
          continue;

        // Skip prerequisites imported from other projects.
        //
        if (pm.proj ())
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
        // @@ Note that this is still an issue in a custom dist rule.
        //
        const target* pt (nullptr);
        if (pm.is_a<file> ())
        {
          pt = pm.load ();

          if (pt == nullptr)
          {
            const prerequisite& p (pm.prerequisite);

            // Search for an existing target or existing file in src.
            //
            // Note: see also similar code in match_postponed() below.
            //
            const prerequisite_key& k (p.key ());
            pt = k.tk.type->search (t, k);

            if (pt == nullptr)
            {
              // Skip it if it's outside of the project (e.g., an executable
              // "imported" in an ad hoc way).
              //
              if (p.dir.absolute ()     &&
                  !p.dir.sub (src_root) &&
                  !p.dir.sub (out_root))
                continue;

              // This can be order-dependent: for example libs{} prerequisite
              // may be unknown because we haven't matched the lib{} group
              // yet. So we postpone this for later (see match_postponed()).
              //
              const module& mod (*rs.find_module<module> (module::name));

              mlock l (mod.postponed.mutex);
              mod.postponed.list.push_back (
                postponed_prerequisite {a, t, p, t.state[a].rule->first});
              continue;
            }

            search_custom (p, *pt); // Cache.
          }
        }
        else
          pt = &pm.search (t);

        // Don't match targets that are outside of our project.
        //
        if (pt->dir.sub (out_root))
          match_sync (a, *pt);
      }

      return noop_recipe; // We will never be executed.
    }

    void rule::
    match_postponed (const postponed_prerequisite& pp)
    {
      action a (pp.action);
      const target& t (pp.target);
      const prerequisite& p (pp.prereq);

      const prerequisite_key& k (p.key ());
      const target* pt (k.tk.type->search (t, k));

      if (pt == nullptr)
      {
        // Note that we do loose the diag frame that we normally get when
        // failing during match. So let's mention the target/rule manually.
        //
        fail << "prerequisite " << k << " is not existing source file nor "
             << "known output target" <<
          info << "while applying rule " << pp.rule << " to " << diag_do (a, t);
      }

      search_custom (p, *pt); // Cache.

      // It's theoretically possible that the target gets entered but nobody
      // else depends on it but us. So we need to make sure it's matched
      // (since it, in turns, can pull in other targets). Note that this could
      // potentially add new postponed prerequisites to the list.
      //
      if (!pt->matched (a))
      {
        if (pt->dir.sub (t.root_scope ().out_path ()))
          match_direct_sync (a, *pt);
      }
    }
  }
}
