// file      : build2/algorithm.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/algorithm>

#include <build2/scope>
#include <build2/target>
#include <build2/rule>
#include <build2/file> // import()
#include <build2/search>
#include <build2/context>
#include <build2/filesystem>
#include <build2/diagnostics>
#include <build2/prerequisite>

using namespace std;
using namespace butl;

namespace build2
{
  target&
  search (const prerequisite_key& pk)
  {
    // If this is a project-qualified prerequisite, then this is import's
    // business.
    //
    if (pk.proj != nullptr)
      return import (pk);

    if (target* t = pk.tk.type->search (pk))
      return *t;

    return create_new_target (pk);
  }

  target&
  search (name n, scope& s)
  {
    const string* e;
    const target_type* tt (s.find_target_type (n, e));

    if (tt == nullptr)
      fail << "unknown target type " << n.type << " in name " << n;

    if (!n.dir.empty ())
      n.dir.normalize (false, true); // Current dir collapses to an empty one.

    // @@ OUT: for now we assume the prerequisite's out is undetermined.
    //         Would need to pass a pair of names.
    //
    return search (*tt, n.dir, dir_path (), n.value, e, &s, n.proj);
  }

  pair<const rule*, match_result>
  match_impl (action a, target& t, bool apply, const rule* skip)
  {
    pair<const rule*, match_result> r (nullptr, false);

    // By default, clear the resolved targets list before calling
    // match(). The rule is free to modify this list in match()
    // (provided that it matches) in order to, for example, prepare
    // it for apply().
    //
    t.reset (a);

    // If this is a nested operation, first try the outer operation.
    // This allows a rule to implement a "precise match", that is,
    // both inner and outer operations match.
    //
    for (operation_id oo (a.outer_operation ()), io (a.operation ()),
           o (oo != 0 ? oo : io);
         o != 0;
         o = (oo != 0 && o != io ? io : 0))
    {
      // Adjust action for recipe: on the first iteration we want it
      // {inner, outer} (which is the same as 'a') while on the second
      // -- {inner, 0}. Note that {inner, 0} is the same or "stronger"
      // (i.e., overrides; see action::operator<()) than 'a'. This
      // allows "unconditional inner" to override "inner for outer"
      // recipes.
      //
      action ra (a.meta_operation (), io, o != oo ? 0 : oo);

      scope& bs (t.base_scope ());

      for (auto tt (&t.type ()); tt != nullptr; tt = tt->base)
      {
        // Search scopes outwards, stopping at the project root.
        //
        for (const scope* s (&bs);
             s != nullptr;
             s = s->root () ? global_scope : s->parent_scope ())
        {
          const operation_rule_map* om (s->rules[a.meta_operation ()]);

          if (om == nullptr)
            continue; // No entry for this meta-operation id.

          // First try the map for the actual operation. If that doesn't yeld
          // anything, try the wildcard map.
          //
          for (size_t oi (o), oip (o); oip != 0; oip = oi, oi = 0)
          {
            const target_type_rule_map* ttm ((*om)[oi]);

            if (ttm == nullptr)
              continue; // No entry for this operation id.

            if (ttm->empty ())
              continue; // Empty map for this operation id.

            auto i (ttm->find (tt));

            if (i == ttm->end () || i->second.empty ())
              continue; // No rules registered for this target type.

            const auto& rules (i->second); // Hint map.

            // @@ TODO
            //
            // Different rules can be used for different operations (update
            // vs test is a good example). So, at some point, we will probably
            // have to support a list of hints or even an operation-hint map
            // (e.g., 'hint=cxx test=foo' if cxx supports the test operation
            // but we want the foo rule instead). This is also the place where
            // the '{build clean}=cxx' construct (which we currently do not
            // support) can come handy.
            //
            // Also, ignore the hint (that is most likely ment for a different
            // operation) if this is a unique match.
            //
            string hint;
            auto rs (rules.size () == 1
                     ? make_pair (rules.begin (), rules.end ())
                     : rules.find_prefix (hint));

            for (auto i (rs.first); i != rs.second; ++i)
            {
              const string& n (i->first);
              const rule& ru (i->second);

              if (&ru == skip)
                continue;

              match_result m (false);
              {
                auto g (
                  make_exception_guard (
                    [ra, &t, &n]()
                    {
                      info << "while matching rule " << n << " to "
                           << diag_do (ra, t);
                    }));

                if (!(m = ru.match (ra, t, hint)))
                  continue;

                if (!m.recipe_action.valid ())
                  m.recipe_action = ra; // Default, if not set.
              }

              // Do the ambiguity test.
              //
              bool ambig (false);

              diag_record dr;

              for (++i; i != rs.second; ++i)
              {
                const string& n1 (i->first);
                const rule& ru1 (i->second);

                {
                  auto g (
                    make_exception_guard (
                      [ra, &t, &n1]()
                      {
                        info << "while matching rule " << n1 << " to "
                             << diag_do (ra, t);
                      }));

                  if (!ru1.match (ra, t, hint))
                    continue;
                }

                if (!ambig)
                {
                  dr << fail << "multiple rules matching "
                     << diag_doing (ra, t)
                     << info << "rule " << n << " matches";
                  ambig = true;
                }

                dr << info << "rule " << n1 << " also matches";
              }

              if (!ambig)
              {
                ra = m.recipe_action; // Use custom, if set.

                if (apply)
                {
                  auto g (
                    make_exception_guard (
                      [ra, &t, &n]()
                      {
                        info << "while applying rule " << n << " to "
                             << diag_do (ra, t);
                      }));

                  // @@ We could also allow the rule to change the recipe
                  // action in apply(). Could be useful with delegates.
                  //
                  t.recipe (ra, ru.apply (ra, t));
                }
                else
                {
                  r.first = &ru;
                  r.second = move (m);
                }

                return r;
              }
              else
                dr << info << "use rule hint to disambiguate this match";
            }
          }
        }
      }
    }

    diag_record dr;
    dr << fail << "no rule to " << diag_do (a, t);

    if (verb < 4)
      dr << info << "re-run with --verbose 4 for more information";

    dr << endf;
  }

  group_view
  resolve_group_members_impl (action a, target& g)
  {
    group_view r;

    // Unless we already have a recipe, try matching the target to
    // the rule.
    //
    if (!g.recipe (a))
    {
      auto rp (match_impl (a, g, false));

      r = g.group_members (a);
      if (r.members != nullptr)
        return r;

      // That didn't help, so apply the rule and go to the building
      // phase.
      //
      const match_result& mr (rp.second);
      g.recipe (mr.recipe_action, rp.first->apply (mr.recipe_action, g));
    }

    // Note that we use execute_direct() rather than execute() here to
    // sidestep the dependents count logic. In this context, this is by
    // definition the first attempt to execute this rule (otherwise we
    // would have already known the members list) and we really do need
    // to execute it now.
    //
    execute_direct (a, g);

    r = g.group_members (a);
    return r; // Might still be unresolved.
  }

  void
  search_and_match_prerequisites (action a, target& t, scope* s)
  {
    for (prerequisite p: group_prerequisites (t))
    {
      target& pt (search (p));

      if (s == nullptr || pt.in (*s))
      {
        match (a, pt);
        t.prerequisite_targets.push_back (&pt);
      }
    }
  }

  void
  search_and_match_prerequisite_members (action a, target& t, scope* s)
  {
    for (prerequisite_member p: group_prerequisite_members (a, t))
    {
      target& pt (p.search ());

      if (s == nullptr || pt.in (*s))
      {
        match (a, pt);
        t.prerequisite_targets.push_back (&pt);
      }
    }
  }

  fsdir*
  inject_fsdir (action a, target& t, bool parent)
  {
    tracer trace ("inject_fsdir");

    scope& bs (t.base_scope ());
    scope* rs (bs.root_scope ());

    // Handle the outside of any project case. Note that we also used to bail
    // our of this is the root of the project. But that proved not to be such
    // a great idea in case of subprojects (e.g., tests/).
    //
    if (rs == nullptr)
      return nullptr;

    // If t is a directory (name is empty), say foo/bar/, then t is bar and
    // its parent directory is foo/.
    //
    const dir_path& d (parent && t.name.empty () ? t.dir.directory () : t.dir);

    // Handle the src = out.
    //
    if (d.sub (rs->src_path ()))
      return nullptr;

    l6 ([&]{trace << d << " for " << t;});

    // Target is in the out tree, so out directory is empty.
    //
    fsdir* r (&search<fsdir> (d, dir_path (), string (), nullptr, nullptr));
    match (a, *r);
    t.prerequisite_targets.emplace_back (r);
    return r;
  }

  target_state
  execute_impl (action a, target& t)
  {
    // Implementation with some multi-threading ideas in mind.
    //
    switch (t.raw_state)
    {
    case target_state::group: // Means group's state is unknown.
    case target_state::unknown:
    case target_state::postponed:
      {
        auto g (
          make_exception_guard (
            [a, &t]()
            {
              t.raw_state = target_state::failed;

              if (verb != 0)
                info << "while " << diag_doing (a, t);
            }));

        target_state ts (t.recipe (a) (a, t));
        assert (ts != target_state::unknown && ts != target_state::failed);

        // Set the target's state unless it should be the group's state.
        //
        if (t.raw_state != target_state::group)
          t.raw_state = ts;

        return ts;
      }
    case target_state::unchanged:
    case target_state::changed:
      // Should have been handled by inline execute().
      assert (false);
    case target_state::failed:
      break;
    }

    throw failed ();
  }

  target_state
  execute_prerequisites (action a, target& t)
  {
    target_state r (target_state::unchanged);

    for (target* pt: t.prerequisite_targets)
    {
      if (pt == nullptr) // Skipped.
        continue;

      r |= execute (a, *pt);
    }

    return r;
  }

  target_state
  reverse_execute_prerequisites (action a, target& t)
  {
    target_state r (target_state::unchanged);

    for (target* pt: reverse_iterate (t.prerequisite_targets))
    {
      if (pt == nullptr) // Skipped.
        continue;

      r |= execute (a, *pt);
    }

    return r;
  }

  pair<target*, target_state>
  execute_prerequisites (const target_type* tt,
                         action a, target& t,
                         const timestamp& mt, const prerequisite_filter& pf)
  {
    bool e (mt == timestamp_nonexistent);

    target* rt (tt != nullptr ? nullptr : &t);
    target_state rs (target_state::unchanged);

    for (target* pt: t.prerequisite_targets)
    {
      if (pt == nullptr) // Skip ignored.
        continue;

      target_state ts (execute (a, *pt));
      rs |= ts;

      // Should we compare the timestamp to this target's?
      //
      if (!e && (!pf || pf (*pt)))
      {
        // If this is an mtime-based target, then compare timestamps.
        //
        if (auto mpt = dynamic_cast<const mtime_target*> (pt))
        {
          timestamp mp (mpt->mtime ());

          // The same logic as in mtime_target::newer() (but avoids a call to
          // state()).
          //
          if (mt < mp || (mt == mp && ts == target_state::changed))
            e = true;
        }
        else
        {
          // Otherwise we assume the prerequisite is newer if it was changed.
          //
          if (ts == target_state::changed)
            e = true;
        }
      }

      if (rt == nullptr && pt->is_a (*tt))
        rt = pt;
    }

    assert (rt != nullptr);
    return make_pair (e ? rt : nullptr, rs);
  }

  target_state
  noop_action (action a, target& t)
  {
    text << "noop action triggered for " << diag_doing (a, t);
    assert (false); // We shouldn't be called, see target::recipe().
    return target_state::unchanged;
  }

  target_state
  group_action (action a, target& t)
  {
    target_state r (execute (a, *t.group));

    // Indicate to the standard execute() logic that this target's
    // state comes from the group.
    //
    t.raw_state = target_state::group;

    return r;
  }

  target_state
  default_action (action a, target& t)
  {
    return current_mode == execution_mode::first
      ? execute_prerequisites (a, t)
      : reverse_execute_prerequisites (a, t);
  }

  target_state
  clean_extra (action a,
               file& ft,
               initializer_list<initializer_list<const char*>> extra)
  {
    // Clean the extras first and don't print the commands at verbosity level
    // below 3. Note the first extra file/directory that actually got removed
    // for diagnostics below.
    //
    target_state er (target_state::unchanged);
    bool ed (false);
    path ep;

    auto clean = [&er, &ed, &ep] (file& f, initializer_list<const char*> es)
    {
      for (const char* e: es)
      {
        size_t n;
        if (e == nullptr || (n = strlen (e)) == 0)
          continue;

        path p;
        bool d;

        if (path::traits::absolute (e))
        {
          p = path (e);
          d = p.to_directory ();
        }
        else
        {
          if ((d = (e[n - 1] == '/')))
            --n;

          p = f.path ();
          for (; *e == '-'; ++e)
            p = p.base ();

          p.append (e, n);
        }

        target_state r (target_state::unchanged);

        if (d)
        {
          dir_path dp (path_cast<dir_path> (p));

          switch (build2::rmdir_r (dp, true, 3))
          {
          case rmdir_status::success:
            {
              r = target_state::changed;
              break;
            }
          case rmdir_status::not_empty:
            {
              if (verb >= 3)
                text << dp << " is current working directory, not removing";
              break;
            }
          case rmdir_status::not_exist:
            break;
          }
        }
        else
        {
          if (rmfile (p, 3))
            r = target_state::changed;
        }

        if (r == target_state::changed && ep.empty ())
        {
          ed = d;
          ep = move (p);
        }

        er |= r;
      }
    };

    auto ei (extra.begin ()), ee (extra.end ());

    if (ei != ee)
      clean (ft, *ei++);

    // Now clean the ad hoc group file members, if any.
    //
    for (target* m (ft.member); m != nullptr; m = m->member)
    {
      file* fm (dynamic_cast<file*> (m));

      if (fm == nullptr || fm->path ().empty ())
        continue;

      if (ei != ee)
        clean (*fm, *ei++);

      const path& f (fm->path ());

      target_state r (rmfile (f, 3)
                      ? target_state::changed
                      : target_state::unchanged);

      if (r == target_state::changed && ep.empty ())
        ep = f;

      er |= r;
    }

    // Now clean the primary target and its prerequisited in the reverse order
    // of update: first remove the file, then clean the prerequisites.
    //
    target_state tr (rmfile (ft.path (), ft)
                     ? target_state::changed
                     : target_state::unchanged);

    // Update timestamp in case there are operations after us that could use
    // the information.
    //
    ft.mtime (timestamp_nonexistent);

    // Clean prerequisites.
    //
    tr |= reverse_execute_prerequisites (a, ft);

    // Factor the result of removing the extra files into the target state.
    // While strictly speaking removing them doesn't change the target state,
    // if we don't do this, then we may end up removing the file but still
    // saying that everything is clean (e.g., if someone removes the target
    // file but leaves the extra laying around). That would be confusing.
    //
    // What would also be confusing is if we didn't print any commands in
    // this case.
    //
    if (tr != target_state::changed && er == target_state::changed)
    {
      if (verb > 0 && verb < 3)
      {
        if (ed)
          text << "rm -r " << path_cast<dir_path> (ep);
        else
          text << "rm " << ep;
      }
    }

    tr |= er;
    return tr;
  }

  target_state
  perform_clean (action a, target& t)
  {
    return clean_extra (a, dynamic_cast<file&> (t), {nullptr});
  }

  target_state
  perform_clean_depdb (action a, target& t)
  {
    return clean_extra (a, dynamic_cast<file&> (t), {".d"});
  }
}
