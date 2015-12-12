// file      : build/algorithm.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/algorithm>

#include <memory>   // unique_ptr
#include <cstddef>  // size_t
#include <utility>  // move
#include <cassert>

#include <butl/utility> // reverse_iterate

#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/rule>
#include <build/file> // import()
#include <build/search>
#include <build/context>
#include <build/utility>
#include <build/diagnostics>

using namespace std;
using namespace butl;

namespace build
{
  target&
  search (const prerequisite_key& pk)
  {
    // If this is a project-qualified prerequisite, then this
    // is import's business.
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

    n.dir.normalize ();

    return search (*tt, move (n.dir), move (n.value), e, &s);
  }

  pair<const rule*, match_result>
  match_impl (action a, target& t, bool apply)
  {
    pair<const rule*, match_result> r;

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
           o (oo != 0 ? oo : io); o != 0; o = (oo != 0 ? io : 0))
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

          // First try the map for the actual operation. If that
          // doesn't yeld anything, try the wildcard map.
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

              match_result m;
              {
                auto g (
                  make_exception_guard (
                    [](action a, target& t, const string& n)
                    {
                      info << "while matching rule " << n << " to "
                           << diag_do (a, t);
                    },
                    ra, t, n));

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
                      [](action a, target& t, const string& n1)
                      {
                        info << "while matching rule " << n1 << " to "
                             << diag_do (a, t);
                      },
                      ra, t, n1));

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
                      [](action a, target& t, const string& n)
                      {
                        info << "while applying rule " << n << " to "
                             << diag_do (a, t);
                      },
                      ra, t, n));

                  // @@ We could also allow the rule to change the recipe
                  // action in apply(). Could be useful with delegates.
                  //
                  t.recipe (ra, ru.apply (ra, t, m));
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

    return r;
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
      g.recipe (mr.recipe_action, rp.first->apply (mr.recipe_action, g, mr));
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
  search_and_match_prerequisites (action a, target& t, const dir_path& d)
  {
    const bool e (d.empty ());

    for (prerequisite p: group_prerequisites (t))
    {
      target& pt (search (p));

      if (e || pt.dir.sub (d))
      {
        match (a, pt);
        t.prerequisite_targets.push_back (&pt);
      }
    }
  }

  void
  search_and_match_prerequisite_members (action a,
                                         target& t,
                                         const dir_path& d)
  {
    const bool e (d.empty ());

    for (prerequisite_member p: group_prerequisite_members (a, t))
    {
      target& pt (p.search ());

      if (e || pt.dir.sub (d))
      {
        match (a, pt);
        t.prerequisite_targets.push_back (&pt);
      }
    }
  }

  void
  inject_parent_fsdir (action a, target& t)
  {
    tracer trace ("inject_parent_fsdir");

    scope& s (t.base_scope ());
    scope* rs (s.root_scope ());

    if (rs == nullptr) // Could be outside any project.
      return;

    const dir_path& out_root (rs->out_path ());

    // If t is a directory (name is empty), say foo/bar/, then
    // t is bar and its parent directory is foo/.
    //
    const dir_path& d (t.name.empty () ? t.dir.directory () : t.dir);

    if (!d.sub (out_root) || d == out_root)
      return;

    level6 ([&]{trace << "for " << t;});

    fsdir& dt (search<fsdir> (d, string (), nullptr, &s));
    match (a, dt);
    t.prerequisite_targets.emplace_back (&dt);
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
            [](action a, target& t)
            {
              t.raw_state = target_state::failed;
              info << "while " << diag_doing (a, t);
            },
            a, t));

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

  bool
  execute_prerequisites (action a, target& t, const timestamp& mt)
  {
    bool e (mt == timestamp_nonexistent);

    for (target* pt: t.prerequisite_targets)
    {
      if (pt == nullptr) // Skipped.
        continue;

      target_state ts (execute (a, *pt));

      if (!e)
      {
        // If this is an mtime-based target, then compare timestamps.
        //
        if (auto mpt = dynamic_cast<const mtime_target*> (pt))
        {
          timestamp mp (mpt->mtime ());

          // What do we do if timestamps are equal? This can happen, for
          // example, on filesystems that don't have subsecond resolution.
          // There is not much we can do here except detect the case where
          // the prerequisite was changed in this run which means the
          // action must be executed on the target as well.
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
    }

    return e;
  }

  target_state
  noop_action (action, target&)
  {
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
  perform_clean (action a, target& t)
  {
    // The reverse order of update: first delete the file, then clean
    // prerequisites.
    //
    file& ft (dynamic_cast<file&> (t));

    target_state r (rmfile (ft.path (), ft)
                    ? target_state::changed
                    : target_state::unchanged);

    // Update timestamp in case there are operations after us that
    // could use the information.
    //
    ft.mtime (timestamp_nonexistent);

    // Clean prerequisites.
    //
    r |= reverse_execute_prerequisites (a, t);

    return r;
  }
}
