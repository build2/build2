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
    if (*pk.proj != nullptr)
      return import (pk);

    if (target* t = pk.tk.type->search (pk))
      return *t;

    return create_new_target (pk);
  }

  pair<const rule*, match_result>
  match_impl (action a, target& t, bool apply)
  {
    pair<const rule*, match_result> r (nullptr, nullptr);

    // Clear the resolved targets list before calling match(). The rule
    // is free to, say, resize() this list in match() (provided that it
    // matches) in order to, for example, prepare it for apply().
    //
    t.prerequisite_targets.clear ();

    size_t oi (a.operation () - 1); // Operation index in rule_map.
    scope& bs (t.base_scope ());

    for (auto tt (&t.type ());
         tt != nullptr && !t.recipe (a);
         tt = tt->base)
    {
      // Search scopes outwards, stopping at the project root.
      //
      for (const scope* s (&bs);
           s != nullptr;
           s = s->root () ? global_scope : s->parent_scope ())
      {
        const rule_map& om (s->rules);

        if (om.size () <= oi)
          continue; // No entry for this operation id.

        const target_type_rule_map& ttm (om[oi]);

        if (ttm.empty ())
          continue; // Empty map for this operation id.

        auto i (ttm.find (tt->id));

        if (i == ttm.end () || i->second.empty ())
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
                a, t, n));

            m = ru.match (a, t, hint);
          }

          if (m)
          {
            // Do the ambiguity test.
            //
            bool ambig (false);

            diag_record dr;

            for (++i; i != rs.second; ++i)
            {
              const string& n1 (i->first);
              const rule& ru1 (i->second);

              match_result m1;
              {
                auto g (
                  make_exception_guard (
                    [](action a, target& t, const string& n1)
                    {
                      info << "while matching rule " << n1 << " to "
                           << diag_do (a, t);
                    },
                    a, t, n1));

                m1 = ru1.match (a, t, hint);
              }

              if (m1)
              {
                if (!ambig)
                {
                  dr << fail << "multiple rules matching " << diag_doing (a, t)
                     << info << "rule " << n << " matches";
                  ambig = true;
                }

                dr << info << "rule " << n1 << " also matches";
              }
            }

            if (!ambig)
            {
              if (apply)
              {
                auto g (
                  make_exception_guard (
                    [](action a, target& t, const string& n)
                    {
                      info << "while applying rule " << n << " to "
                           << diag_do (a, t);
                    },
                    a, t, n));

                t.recipe (a, ru.apply (a, t, m));
              }
              else
              {
                r.first = &ru;
                r.second = m;
              }

              return r;
            }
            else
              dr << info << "use rule hint to disambiguate this match";
          }
        }
      }
    }

    diag_record dr;
    dr << fail << "no rule to " << diag_do (a, t);

    if (verb < 3)
      dr << info << "re-run with --verbose 3 for more information";

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
      auto p (match_impl (a, g, false));

      r = g.group_members (a);
      if (r.members != nullptr)
        return r;

      // That didn't help, so apply the rule and go to the building
      // phase.
      //
      g.recipe (a, p.first->apply (a, g, p.second));
    }

    // Note that we use execute_direct() rather than execute() here to
    // sidestep the dependents count logic. In this context, this is by
    // definition the first attempt to execute this rule (otherwise we
    // would have already known the members list) and we really do need
    // to execute it now.
    //
    execute_direct (a, g);

    r = g.group_members (a);
    assert (r.members != nullptr); // What "next step" did the group expect?
    return r;
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

    const dir_path& out_root (rs->path ());

    // If t is a directory (name is empty), say foo/bar/, then
    // t is bar and its parent directory is foo/.
    //
    const dir_path& d (t.name.empty () ? t.dir.directory () : t.dir);

    if (!d.sub (out_root) || d == out_root)
      return;

    level5 ([&]{trace << "for " << t;});

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
    case target_state::group:
    case target_state::unknown:
    case target_state::postponed:
      {
        t.raw_state = target_state::failed; // So the rule can just throw.

        auto g (
          make_exception_guard (
            [](action a, target& t){info << "while " << diag_doing (a, t);},
            a, t));

        target_state ts (t.recipe (a) (a, t));
        assert (ts != target_state::unknown && ts != target_state::failed);

        // The recipe may have set the target's state manually.
        //
        if (t.raw_state == target_state::failed)
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
    target_state ts (target_state::unchanged);

    for (target* pt: t.prerequisite_targets)
    {
      if (pt == nullptr) // Skipped.
        continue;

      if (execute (a, *pt) == target_state::changed)
        ts = target_state::changed;
    }

    return ts;
  }

  target_state
  reverse_execute_prerequisites (action a, target& t)
  {
    target_state ts (target_state::unchanged);

    for (target* pt: reverse_iterate (t.prerequisite_targets))
    {
      if (pt == nullptr) // Skipped.
        continue;

      if (execute (a, *pt) == target_state::changed)
        ts = target_state::changed;
    }

    return ts;
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

    // The standard execute() logic sets the state to failed just
    // before calling the recipe (so that the recipe can just throw
    // to indicate a failure). After the recipe is successfully
    // executed and unless the recipe has updated the state manually,
    // the recipe's return value is set as the new state. But we
    // don't want that. So we are going to set it manually.
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

    bool r (rmfile (ft.path (), ft));

    // Update timestamp in case there are operations after us that
    // could use the information.
    //
    ft.mtime (timestamp_nonexistent);

    // Clean prerequisites.
    //
    target_state ts (reverse_execute_prerequisites (a, t));

    return r ? target_state::changed : ts;
  }
}
