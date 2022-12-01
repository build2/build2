// file      : libbuild2/bin/rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bin/rule.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/target.hxx>
#include <libbuild2/bin/utility.hxx>

using namespace std;

namespace build2
{
  namespace bin
  {
    // Search for an existing (declared real) member and match it if found.
    //
    static void
    dist_match (action a, target& t, const target_type& tt)
    {
      if (const target* m = search_existing (t.ctx, tt, t.dir, t.out, t.name))
      {
        // Only a real target declaration can have prerequisites (which is
        // the reason we are doing this).
        //
        if (m->decl == target_decl::real)
          match_sync (a, *m);
      }
    }

    // obj_rule
    //
    bool obj_rule::
    match (action a, target& t) const
    {
      if (a.meta_operation () == dist_id)
        return true;

      const char* n (t.dynamic_type->name); // Ignore derived type.

      fail << diag_doing (a, t) << " target group" <<
        info << "explicitly select " << n << "e{}, " << n << "a{}, or "
           << n << "s{} member" << endf;
    }

    recipe obj_rule::
    apply (action a, target& t) const
    {
      // We only get here for dist.
      //
      const target_type* ett (nullptr);
      const target_type* att (nullptr);
      const target_type* stt (nullptr);

      if (t.is_a<obj> ())
      {
        ett = &obje::static_type;
        att = &obja::static_type;
        stt = &objs::static_type;
      }
      else if (t.is_a<bmi> ())
      {
        ett = &bmie::static_type;
        att = &bmia::static_type;
        stt = &bmis::static_type;
      }
      else if (t.is_a<hbmi> ())
      {
        ett = &hbmie::static_type;
        att = &hbmia::static_type;
        stt = &hbmis::static_type;
      }
      else
        assert (false);

      dist_match (a, t, *ett);
      dist_match (a, t, *att);
      dist_match (a, t, *stt);

      // Delegate to the default dist rule to match prerequisites.
      //
      return dist::rule::apply (a, t);
    }

    // libul_rule
    //
    bool libul_rule::
    match (action, target&) const
    {
      return true;
    }

    recipe libul_rule::
    apply (action a, target& t) const
    {
      if (a.meta_operation () == dist_id)
      {
        dist_match (a, t, libua::static_type);
        dist_match (a, t, libus::static_type);

        // Delegate to the default dist rule to match prerequisites.
        //
        return dist::rule::apply (a, t);
      }

      // Pick one of the members. First looking for the one already matched.
      //
      const target* m (nullptr);

      const libus* ls (nullptr);
      {
        ls = search_existing<libus> (t.ctx, t.dir, t.out, t.name);

        if (ls != nullptr && ls->matched (a))
          m = ls;
      }

      const libua* la (nullptr);
      if (m == nullptr)
      {
        la = search_existing<libua> (t.ctx, t.dir, t.out, t.name);

        if (la != nullptr && la->matched (a))
          m = la;
      }

      if (m == nullptr)
      {
        const scope& bs (t.base_scope ());

        lmembers lm (link_members (*bs.root_scope ()));

        if (lm.s && lm.a)
        {
          // Use the bin.exe.lib order as a heuristics to pick the library
          // (i.e., the most likely utility library to be built is the one
          // most likely to be linked).
          //
          lorder lo (link_order (bs, otype::e));

          (lo == lorder::s_a || lo == lorder::s ? lm.a : lm.s) = false;
        }

        if (lm.s)
          m = ls != nullptr ? ls : &search<libus> (t, t.dir, t.out, t.name);
        else
          m = la != nullptr ? la : &search<libua> (t, t.dir, t.out, t.name);
      }

      // Save the member we picked in case others (e.g., $x.lib_poptions())
      // need this information.
      //
      t.prerequisite_targets[a].push_back (m);

      if (match_sync (a, *m, unmatch::safe).first)
        return noop_recipe;

      return [] (action a, const target& t)
      {
        const target* m (t.prerequisite_targets[a].back ());

        // For update always return unchanged so we are consistent whether we
        // managed to unmatch or now. Note that for clean we may get postponed
        // so let's return the actual target state.
        //
        target_state r (execute_sync (a, *m));
        return a == perform_update_id ? target_state::unchanged : r;
      };
    }

    // lib_rule
    //
    // The whole logic is pretty much as if we had our two group members as
    // our prerequisites.
    //
    // Note also that unlike the obj and libul rules above, we don't need to
    // delegate to the default dist rule since any group prerequisites will be
    // matched by one of the members (the key difference here is that unlike
    // those rules, we insert and match members unconditionally).
    //
    bool lib_rule::
    match (action a, target& xt) const
    {
      lib& t (xt.as<lib> ());

      lmembers bm (a.meta_operation () != dist_id
                   ? link_members (t.root_scope ())
                   : lmembers {true, true});

      t.a = bm.a ? &search<liba> (t, t.dir, t.out, t.name) : nullptr;
      t.s = bm.s ? &search<libs> (t, t.dir, t.out, t.name) : nullptr;

      return true;
    }

    recipe lib_rule::
    apply (action a, target& xt) const
    {
      lib& t (xt.as<lib> ());

      //@@ outer: also prerequisites (if outer) or not?

      const target* m[] = {t.a, t.s};
      match_members (a, t, m);

      return &perform;
    }

    target_state lib_rule::
    perform (action a, const target& xt)
    {
      const lib& t (xt.as<lib> ());

      const target* m[] = {t.a, t.s};
      return execute_members (a, t, m);
    }
  }
}
