// file      : libbuild2/operation.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/operation.hxx>

#include <iostream>      // cout
#include <unordered_map>

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/serializer.hxx>
#endif

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/diagnostics.hxx>

#if 0
#include <libbuild2/adhoc-rule-buildscript.hxx> // @@ For a hack below.
#endif

using namespace std;
using namespace butl;

namespace build2
{
  // action
  //
  ostream&
  operator<< (ostream& os, action a)
  {
    uint16_t
      m (a.meta_operation ()),
      i (a.operation ()),
      o (a.outer_operation ());

    os << '(' << m << ',';

    if (o != 0)
      os << o << '(';

    os << i;

    if (o != 0)
      os << ')';

    os << ')';

    return os;
  }

  // noop
  //
  const meta_operation_info mo_noop {
    noop_id,
    "noop",
    "",      // Presumably we will never need these since we are not going
    "",      // to do anything.
    "",
    "",
    true,    // bootstrap_outer
    nullptr, // meta-operation pre
    nullptr, // operation pre
    &perform_load,
    nullptr, // search
    nullptr, // match
    nullptr, // execute
    nullptr, // operation post
    nullptr, // meta-operation post
    nullptr  // include
  };

  // perform
  //
  void
  perform_load (const values&,
                scope& root,
                const path& bf,
                const dir_path& out_base,
                const dir_path& src_base,
                const location&)
  {
    // Load project's root.build.
    //
    if (!root.root_extra->loaded)
      load_root (root);

    // Create the base scope. Note that its existence doesn't mean it was
    // already setup as a base scope; it can be the same as root.
    //
    auto i (root.ctx.scopes.rw (root).insert_out (out_base));
    scope& base (setup_base (i, out_base, src_base));

    // Load the buildfile unless it is implied.
    //
    if (!bf.empty ())
      source_once (root, base, bf);
  }

  void
  perform_search (const values&,
                  const scope&,
                  const scope& bs,
                  const path& bf,
                  const target_key& tk,
                  const location& l,
                  action_targets& ts)
  {
    tracer trace ("perform_search");

    context& ctx (bs.ctx);
    phase_lock pl (ctx, run_phase::match);

    const target* t (ctx.targets.find (tk, trace));

    // Only do the implied buildfile if we haven't loaded one. Failed that we
    // may try go this route even though we've concluded the implied buildfile
    // is implausible and have loaded an outer buildfile (see main() for
    // details).
    //
    if (t == nullptr && tk.is_a<dir> () && bf.empty ())
      t = dir::search_implied (bs, tk, trace);

    if (t == nullptr)
    {
      diag_record dr (fail (l));

      dr << "unknown target " << tk;

      if (!bf.empty ())
        dr << " in " << bf;
    }

    ts.push_back (t);
  }

  // Verify that no two targets share a path unless they both are "read-only"
  // (have noop recipes).
  //
  // Note: somewhat similar logic in dyndep::verify_existing_file().
  //
  static void
  verify_targets (context& ctx, action a)
  {
    // On the first pass we collect all the targets that have non-noop
    // recipes. On the second pass we check if there are any other targets
    // that have the same path. Note that we must also deal with two non-noop
    // targets that have the same path.
    //
    // Strictly speaking we may need to produce some sort of progress if this
    // takes long. However, currently we are looking at verification speed of
    // ~1ms per 2K targets, which means it will only becomes noticeable with
    // over 1M targets.
    //
    unordered_map<reference_wrapper<const path>,
                  const target*,
                  hash<path>,
                  equal_to<path>> map;

    // Half of the total appears to be a reasonable heuristics.
    //
    map.reserve (ctx.targets.size () / 2);

    size_t count_matched (ctx.count_matched ());

    bool e (false);
    for (size_t pass (1); pass != 3; ++pass)
    {
      for (const auto& pt: ctx.targets)
      {
        // We are only interested in path-based targets.
        //
        const path_target* t (pt->is_a<path_target> ());
        if (t == nullptr)
          continue;

        // We are only interested in the matched targets.
        //
        const target::opstate& s (t->state[a]);

        if (s.task_count.load (memory_order_relaxed) < count_matched)
          continue;

        // Skip if for some reason the path is not assigned.
        //
        const path& p (t->path (memory_order_relaxed));
        if (p.empty ())
          continue;

        recipe_function* const* rf (s.recipe.target<recipe_function*> ());
        bool noop (rf != nullptr && *rf == &noop_action);

        if ((noop ? 2 : 1) != pass)
          continue;

        const target* t1;
        if (pass == 1)
        {
          auto r (map.emplace (p, t));

          if (r.second)
            continue;

          t1 = r.first->second;
        }
        else
        {
          auto i (map.find (p));

          if (i == map.end ())
            continue;

          t1 = i->second;
        }

        e = true;

        diag_record dr (error);

        dr << "multiple targets share path " << p <<
          info << "first target:  " << *t1 <<
          info << "second target: " << *t <<
          info << "target " << *t1 << " has non-noop recipe";

        if (pass == 1)
        {
          dr << info << "target " << *t << " has non-noop recipe";
        }
        else if (t->decl != target_decl::real)
        {
          if (t->decl == target_decl::implied)
          {
            dr << info << "target " << *t << " is implied by a buildfile";
          }
          else
          {
            dr << info << "target " << *t << " is not declared in a buildfile";

            if (t->decl == target_decl::prereq_file)
              dr << " but has corresponding existing file";

            dr << info << "perhaps it is a dynamic dependency?";
          }
        }
      }
    }

    if (e)
      throw failed ();
  }

  void
  perform_match (const values&, action a, action_targets& ts,
                 uint16_t diag, bool prog)
  {
    tracer trace ("perform_match");

    if (ts.empty ())
      return;

    context& ctx (ts[0].as<target> ().ctx);

    {
      phase_lock l (ctx, run_phase::match);

      // Setup progress reporting if requested.
      //
      struct monitor_data
      {
        size_t incr;
        string what;
        atomic<timestamp::rep> time {timestamp_nonexistent_rep};
      } md; // Note: must outlive monitor_guard.
      scheduler::monitor_guard mg;

      if (prog && show_progress (2 /* max_verb */))
      {
        // Note that showing progress is not free and it can take up to 10% of
        // the up-to-date check on some projects (e.g., Boost). So we jump
        // through a few hoops to make sure we don't overindulge.
        //
        md.incr = stderr_term // Scale depending on output type.
          ? (ctx.sched->serial () ? 1 : 5)
          : 100;
        md.what = " targets to " + diag_do (ctx, a);

        mg = ctx.sched->monitor (
          ctx.target_count,
          md.incr,
          [&md] (size_t c) -> size_t
          {
            size_t r (c + md.incr);

            if (stderr_term)
            {
              timestamp o (duration (md.time.load (memory_order_consume)));
              timestamp n (system_clock::now ());

              if (n - o < chrono::milliseconds (80))
                return r;

              md.time.store (n.time_since_epoch ().count (),
                             memory_order_release);
            }

            diag_progress_lock pl;
            diag_progress  = ' ';
            diag_progress += to_string (c);
            diag_progress += md.what;

            return r;
          });
      }

      // Start asynchronous matching of prerequisites keeping track of how
      // many we have started. Wait with unlocked phase to allow phase
      // switching.
      //
      bool fail (false);
      size_t i (0), n (ts.size ());
      {
        atomic_count task_count (0);
        wait_guard wg (ctx, task_count, true);

        for (; i != n; ++i)
        {
          const target& t (ts[i].as<target> ());
          l5 ([&]{trace << diag_doing (a, t);});

          target_state s (match_async (a, t,
                                       0, task_count,
                                       match_extra::all_options,
                                       false /* fail */));

          // Bail out if the target has failed and we weren't instructed to
          // keep going.
          //
          if (s == target_state::failed)
          {
            fail = true;

            if (!ctx.keep_going)
            {
              ++i;
              break;
            }
          }
        }

        wg.wait ();
      }

      // If we have any targets with post hoc prerequisites, match those.
      //
      // See match_posthoc() for the overall approach description.
      //
      bool posthoc_fail (false);
      if (!ctx.current_posthoc_targets.empty () && (!fail || ctx.keep_going))
      {
        using posthoc_target = context::posthoc_target;
        using posthoc_prerequisite_target = posthoc_target::prerequisite_target;

        // Note that on each iteration we may end up with new entries at the
        // back. Since we start and end each iteration in serial execution, we
        // don't need to mess with the mutex.
        //
        for (const posthoc_target& p: ctx.current_posthoc_targets)
        {
          action a (p.action); // May not be the same as argument action.
          const target& t (p.target);

          auto df = make_diag_frame (
            [a, &t](const diag_record& dr)
            {
              if (verb != 0)
                dr << info << "while matching to " << diag_do (t.ctx, a)
                   << " post hoc prerequisites of " << t;
            });

          // Cannot use normal match because incrementing dependency counts in
          // the face of cycles does not work well (we will deadlock for the
          // reverse execution mode).
          //
          // @@ PERF: match in parallel (need match_direct_async(), etc).
          //
          for (const posthoc_prerequisite_target& pt: p.prerequisite_targets)
          {
            if (pt.target != nullptr)
            {
              target_state s (match_direct_sync (a, *pt.target,
                                                 pt.match_options,
                                                 false /* fail */));

              if (s == target_state::failed)
              {
                posthoc_fail = true;

                if (!ctx.keep_going)
                  break;
              }
            }
          }

          if (posthoc_fail && !ctx.keep_going)
            break;
        }
      }

      // Clear the progress if present.
      //
      if (mg)
      {
        diag_progress_lock pl;
        diag_progress.clear ();
      }

      // We are now running serially. Re-examine targets that we have matched.
      //
      for (size_t j (0); j != n; ++j)
      {
        action_target& at (ts[j]);
        const target& t (at.as<target> ());

        // We cannot attribute post hoc failures to specific targets so it
        // seems the best we can do is just fail them all.
        //
        target_state s;
        if (j < i)
        {
          s = match_complete (a, t, match_extra::all_options, false /* fail */);

          if (posthoc_fail)
            s = /*t.state[a].state =*/ target_state::failed;
        }
        else
          s = target_state::postponed;

        switch (s)
        {
        case target_state::postponed:
          {
            // We bailed before matching it (leave state in action_target as
            // unknown).
            //
            if (verb != 0 && diag >= 1)
              info << "not " << diag_did (a, t);

            break;
          }
        case target_state::unknown:
        case target_state::unchanged:
        case target_state::changed: // Can happend for ad hoc group member.
          {
            break; // Matched successfully.
          }
        case target_state::failed:
          {
            // Things didn't go well for this target.
            //
            if (verb != 0 && diag >= 1)
              info << "failed to " << diag_do (a, t);

            at.state = s;
            fail = true;
            break;
          }
        default:
          assert (false);
        }
      }

      if (fail)
        throw failed ();

      // @@ This feels a bit ad hoc. Maybe we should invent operation hooks
      //    for this (e.g., post-search, post-match, post-execute)?
      //
      if (a == perform_update_id)
        verify_targets (ctx, a);
    }

    // Phase restored to load.
    //
    assert (ctx.phase == run_phase::load);
  }

  void
  perform_execute (const values&, action a, action_targets& ts,
                   uint16_t diag, bool prog)
  {
    tracer trace ("perform_execute");

    if (ts.empty ())
      return;

    context& ctx (ts[0].as<target> ().ctx);

    bool posthoc_fail (false);
    auto execute_posthoc = [&ctx, &posthoc_fail] ()
    {
      using posthoc_target = context::posthoc_target;
      using posthoc_prerequisite_target = posthoc_target::prerequisite_target;

      for (const posthoc_target& p: ctx.current_posthoc_targets)
      {
        action a (p.action); // May not be the same as argument action.
        const target& t (p.target);

        auto df = make_diag_frame (
          [a, &t](const diag_record& dr)
          {
            if (verb != 0)
              dr << info << "while " << diag_doing (t.ctx, a)
                 << " post hoc prerequisites of " << t;
          });

#if 0
        for (const posthoc_prerequisite_target& pt: p.prerequisite_targets)
        {
          if (pt.target != nullptr)
          {
            target_state s (
              execute_direct_sync (a, *pt.target, false /* fail */));

            if (s == target_state::failed)
            {
              posthoc_fail = true;

              if (!ctx.keep_going)
                break;
            }
          }
        }
#else
        // Note: similar logic/reasoning to below except we use direct
        // execution.
        //
        atomic_count tc (0);
        wait_guard wg (ctx, tc);

        for (const posthoc_prerequisite_target& pt: p.prerequisite_targets)
        {
          if (pt.target != nullptr)
          {
            target_state s (
              execute_direct_async (a, *pt.target, 0, tc, false /*fail*/));

            if (s == target_state::failed)
            {
              posthoc_fail = true;

              if (!ctx.keep_going)
                break;
            }
          }
        }

        wg.wait ();

        // Process the result.
        //
        for (const posthoc_prerequisite_target& pt: p.prerequisite_targets)
        {
          if (pt.target != nullptr)
          {
            // Similar to below, no need to wait.
            //
            target_state s (pt.target->executed_state (a, false /* fail */));

            if (s == target_state::failed)
            {
              // Note: no need to keep going.
              //
              posthoc_fail = true;
              break;
            }
          }
        }
#endif
        if (posthoc_fail && !ctx.keep_going)
          break;
      }
    };

    // Reverse the order of targets if the execution mode is 'last'.
    //
    if (ctx.current_mode == execution_mode::last)
      reverse (ts.begin (), ts.end ());

    phase_lock pl (ctx, run_phase::execute); // Never switched.

    bool fail (false);
    {
      // Tune the scheduler.
      //
      using tune_guard = scheduler::tune_guard;
      tune_guard sched_tune;

      switch (ctx.current_inner_oif->concurrency)
      {
      case 0: sched_tune = tune_guard (*ctx.sched, 1); break; // Run serially.
      case 1:                                         break; // Run as is.
      default:                               assert (false); // Not supported.
      }

      // Set the dry-run flag.
      //
      ctx.dry_run = ctx.dry_run_option;

      // Setup progress reporting if requested.
      //
      string what; // Note: must outlive monitor_guard.
      scheduler::monitor_guard mg;

      if (prog && show_progress (1 /* max_verb */))
      {
        size_t init (ctx.target_count.load (memory_order_relaxed));
        size_t incr (init > 100 ? init / 100 : 1); // 1%.

        if (init != incr)
        {
          what = "% of targets " + diag_did (ctx, a);

          mg = ctx.sched->monitor (
            ctx.target_count,
            init - incr,
            [init, incr, &what, &ctx] (size_t c) -> size_t
            {
              size_t p ((init - c) * 100 / init);
              size_t s (ctx.skip_count.load (memory_order_relaxed));

              diag_progress_lock pl;
              diag_progress  = ' ';
              diag_progress += to_string (p);
              diag_progress += what;

              if (s != 0)
              {
                diag_progress += " (";
                diag_progress += to_string (s);
                diag_progress += " skipped)";
              }

              return c - incr;
            });
        }
      }

      // In the 'last' execution mode run post hoc first.
      //
      if (ctx.current_mode == execution_mode::last)
      {
        if (!ctx.current_posthoc_targets.empty ())
          execute_posthoc ();
      }

      // Similar logic to execute_members(): first start asynchronous
      // execution of all the top-level targets.
      //
      if (!posthoc_fail || ctx.keep_going)
      {
        atomic_count task_count (0);
        wait_guard wg (ctx, task_count);

        for (const action_target& at: ts)
        {
          const target& t (at.as<target> ());

          l5 ([&]{trace << diag_doing (a, t);});

          target_state s (execute_async (a, t, 0, task_count, false));

          // Bail out if the target has failed and we weren't instructed to
          // keep going.
          //
          if (s == target_state::failed)
          {
            fail = true;

            if (!ctx.keep_going)
              break;
          }
        }

        wg.wait ();
      }

      if (ctx.current_mode == execution_mode::first)
      {
        if (!ctx.current_posthoc_targets.empty () && (!fail || ctx.keep_going))
          execute_posthoc ();
      }

      // We are now running serially.
      //

      // Clear the dry-run flag.
      //
      ctx.dry_run = false;

      // Clear the progress if present.
      //
      if (mg)
      {
        diag_progress_lock pl;
        diag_progress.clear ();
      }

      // Restore original scheduler settings.
    }

    // Print skip count if not zero. Note that we print it regardless of the
    // diag level since this is essentially a "summary" of all the commands
    // that we did not (and, in fact, used to originally) print. However, we
    // do suppress it if no progress was requested: conceptually, it feels
    // like part of the progress report and real usage suggests this as well
    // (e.g., when building modules/recipes in a nested context).
    //
    if (prog && verb != 0)
    {
      if (size_t s = ctx.skip_count.load (memory_order_relaxed))
      {
        text << "skipped " << diag_doing (ctx, a) << ' ' << s << " targets";
      }
    }

    // Re-examine all the targets and print diagnostics.
    //
    for (action_target& at: ts)
    {
      const target& t (at.as<target> ());

      // Similar to match we cannot attribute post hoc failures to specific
      // targets so it seems the best we can do is just fail them all.
      //
      if (!posthoc_fail)
      {
        // Note that here we call executed_state() directly instead of
        // execute_complete() since we know there is no need to wait.
        //
        at.state = t.executed_state (a, false /* fail */);
      }
      else
        at.state = /*t.state[a].state =*/ target_state::failed;

      switch (at.state)
      {
      case target_state::unknown:
        {
          // We bailed before executing it (leave state in action_target as
          // unknown).
          //
          if (verb != 0 && diag >= 1)
            info << "not " << diag_did (a, t);

          break;
        }
      case target_state::unchanged:
        {
          // Nothing had to be done.
          //
          if (verb != 0 && diag >= 2)
            info << diag_done (a, t);

          break;
        }
      case target_state::changed:
        {
          // Something has been done.
          //
          break;
        }
      case target_state::failed:
        {
          // Things didn't go well for this target.
          //
          if (verb != 0 && diag >= 1)
            info << "failed to " << diag_do (a, t);

          fail = true;
          break;
        }
      default:
        assert (false);
      }
    }

    if (fail)
      throw failed ();

#ifndef NDEBUG
    size_t base (ctx.count_base ());

    // For now we disable these checks if we've performed any group member
    // resolutions that required a match (with apply()) but not execute.
    //
    if (ctx.target_count.load (memory_order_relaxed) != 0 &&
        ctx.resolve_count.load (memory_order_relaxed) != 0)
    {
      // These counts are only tracked for the inner operation.
      //
      action ia (a.outer () ? a.inner_action () : a);

      // While it may seem that just decrementing the counters for every
      // target with the resolve_counted flag set should be enough, this will
      // miss any prerequisites that this target has matched but did not
      // execute, which may affect both task_count and dependency_count. Note
      // that this applies recursively and we effectively need to pretend to
      // execute this target and all its prerequisites, recursively without
      // actually executing any of their recepies.
      //
      // That last bit means we must be able to interpret the populated
      // prerequisite_targets generically, which is a requirement we place on
      // rules that resolve groups in apply (see target::group_members() for
      // details). It so happens that our own adhoc_buildscript_rule doesn't
      // follow this rule (see execute_update_prerequisites()) so we detect
      // and handle this with a hack.
      //
      // @@ Hm, but there is no guarantee that this holds recursively since
      // prerequisites may not be see-through groups. For this to work we
      // would have to impose this restriction globally. Which we could
      // probably do, just need to audit things carefully (especially
      // cc::link_rule). But we already sort of rely on that for dump! Maybe
      // should just require it everywhere and fix adhoc_buildscript_rule.
      //
      // @@ There are special recipes that don't populate prerequisite_targets
      //    like group_recipe! Are we banning any user-defined such recipes?
      //    Need to actually look if we have anything else like this. There
      //    is also inner_recipe, though doesn't apply here (only for outer).
      //
      // @@ TMP: do and enable after the 0.16.0 release.
      //
      // Note: recursive lambda.
      //
#if 0
      auto pretend_execute = [base, ia] (target& t,
                                         const auto& pretend_execute) -> void
      {
        context& ctx (t.ctx);

        // Note: tries to emulate the execute_impl() functions semantics.
        //
        auto execute_impl = [base, ia, &ctx, &pretend_execute] (target& t)
        {
          target::opstate& s (t.state[ia]);

          size_t gd (ctx.dependency_count.fetch_sub (1, memory_order_relaxed));
          size_t td (s.dependents.fetch_sub (1, memory_order_release));
          assert (td != 0 && gd != 0);

          // Execute unless already executed.
          //
          if (s.task_count.load (memory_order_relaxed) !=
              base + target::offset_executed)
            pretend_execute (t, pretend_execute);
        };

        target::opstate& s (t.state[ia]);

        if (s.state != target_state::unchanged) // Noop recipe.
        {
          if (s.recipe_group_action)
          {
            execute_impl (const_cast<target&> (*t.group));
          }
          else
          {
            // @@ Special hack for adhoc_buildscript_rule (remember to drop
            //    include above if getting rid of).
            //
            bool adhoc (
              ia == perform_update_id &&
              s.rule != nullptr       &&
              dynamic_cast<const adhoc_buildscript_rule*> (
                &s.rule->second.get ()) != nullptr);

            for (const prerequisite_target& p: t.prerequisite_targets[ia])
            {
              const target* pt;

              if (adhoc)
                pt = (p.target != nullptr ? p.target :
                      p.adhoc ()          ? reinterpret_cast<target*> (p.data) :
                      nullptr);
              else
                pt = p.target;

              if (pt != nullptr)
                execute_impl (const_cast<target&> (*pt));
            }

            ctx.target_count.fetch_sub (1, memory_order_relaxed);
            if (s.resolve_counted)
            {
              s.resolve_counted = false;
              ctx.resolve_count.fetch_sub (1, memory_order_relaxed);
            }
          }

          s.state = target_state::changed;
        }

        s.task_count.store (base + target::offset_executed,
                            memory_order_relaxed);
      };
#endif

      for (const auto& pt: ctx.targets)
      {
        target& t (*pt);
        target::opstate& s (t.state[ia]);

        // We are only interested in the targets that have been matched for
        // this operation and are in the applied state.
        //
        if (s.task_count.load (memory_order_relaxed) !=
            base + target::offset_applied)
          continue;

        if (s.resolve_counted)
        {
#if 0
          pretend_execute (t, pretend_execute);

          if (ctx.resolve_count.load (memory_order_relaxed) == 0)
            break;
#else
          return; // Skip all the below checks.
#endif
        }
      }
    }

    // We should have executed every target that we have matched, provided we
    // haven't failed (in which case we could have bailed out early).
    //
    assert (ctx.target_count.load (memory_order_relaxed) == 0);
    assert (ctx.resolve_count.load (memory_order_relaxed) == 0); // Sanity check.

    if (ctx.dependency_count.load (memory_order_relaxed) != 0)
    {
      auto dependents = [base] (action a, const target& t)
      {
        const target::opstate& s (t.state[a]);

        // Only consider targets that have been matched for this operation
        // (since matching is what causes the dependents count reset).
        //
        size_t c (s.task_count.load (memory_order_relaxed));

        return (c >= base + target::offset_applied
                ? s.dependents.load (memory_order_relaxed)
                : 0);
      };

      diag_record dr;
      dr << info << "detected unexecuted matched targets:";

      for (const auto& pt: ctx.targets)
      {
        const target& t (*pt);

        if (size_t n = dependents (a, t))
          dr << text << t << ' ' << n;

        if (a.outer ())
        {
          if (size_t n = dependents (a.inner_action (), t))
            dr << text << t << ' ' << n;
        }
      }
    }

    assert (ctx.dependency_count.load (memory_order_relaxed) == 0);
#endif
  }

  const meta_operation_info mo_perform {
    perform_id,
    "perform",
    "",
    "",
    "",
    "",
    true,    // bootstrap_outer
    nullptr, // meta-operation pre
    nullptr, // operation pre
    &perform_load,
    &perform_search,
    &perform_match,
    &perform_execute,
    nullptr, // operation post
    nullptr, // meta-operation post
    nullptr  // include
  };

  // info
  //

  // Note: similar approach to forward() in configure.
  //
  struct info_params
  {
    bool json = false;
    bool subprojects = true;
  };

  // Note: should not fail if mo is NULL (see info_subprojects() below).
  //
  static info_params
  info_parse_params (const values& params,
                     const char* mo = nullptr,
                     const location& l = location ())
  {
    info_params r;

    if (params.size () == 1)
    {
      for (const name& n: cast<names> (params[0]))
      {
        if (n.simple ())
        {
          if (n.value == "json")
          {
            r.json = true;
            continue;
          }

          if (n.value == "no_subprojects")
          {
            r.subprojects = false;
            continue;
          }

          // Fall through.
        }

        if (mo != nullptr)
          fail (l) << "unexpected parameter '" << n << "' for "
                   << "meta-operation " << mo;
      }
    }
    else if (!params.empty ())
    {
      if (mo != nullptr)
        fail (l) << "unexpected parameters for meta-operation " << mo;
    }

    return r;
  }

  bool
  info_subprojects (const values& params)
  {
    return info_parse_params (params).subprojects;
  }

  static void
  info_pre (context&, const values& params, const location& l)
  {
    info_parse_params (params, "info", l); // Validate.
  }

  static operation_id
  info_operation_pre (context&, const values&, operation_id o)
  {
    if (o != default_id)
      fail << "explicit operation specified for meta-operation info";

    return o;
  }

  void
  info_load (const values&,
             scope& rs,
             const path&,
             const dir_path& out_base,
             const dir_path& src_base,
             const location& l)
  {
    // For info we don't want to go any further than bootstrap so that it can
    // be used in pretty much any situation (unresolved imports, etc). We do
    // need to setup root as base though.

    if (rs.out_path () != out_base || rs.src_path () != src_base)
      fail (l) << "meta-operation info target must be project root directory";

    setup_base (rs.ctx.scopes.rw (rs).insert_out (out_base),
                out_base,
                src_base);
  }

  void
  info_search (const values&,
               const scope& rs,
               const scope&,
               const path&,
               const target_key& tk,
               const location& l,
               action_targets& ts)
  {
    // Collect all the projects we need to print information about.

    // We've already verified the target is in the project root. Now verify
    // it is dir{}.
    //
    if (!tk.type->is_a<dir> ())
      fail (l) << "meta-operation info target must be project root directory";

    ts.push_back (&rs);
  }

  static void
  info_execute_lines (action_targets& ts, bool subp)
  {
    for (size_t i (0); i != ts.size (); ++i)
    {
      // Separate projects with blank lines.
      //
      if (i != 0)
        cout << endl;

      const scope& rs (ts[i].as<scope> ());

      context& ctx (rs.ctx);

      // Print [meta_]operation names. Due to the way our aliasing works, we
      // have to go through the [meta_]operation_table.
      //
      auto print_ops = [] (const auto& ov, const auto& ot)
      {
        // This is a sparse vector with NULL holes. id 0 is invalid while 1 is
        // the noop meta-operation and the default operation; we omit printing
        // both.
        //
        for (uint8_t id (2); id < ov.size (); ++id)
        {
          if (ov[id] != nullptr)
            cout << ' ' << ot[id];
        }
      };

      // Print bootstrapped modules.
      //
      auto print_mods = [&rs] ()
      {
        for (const module_state& ms: rs.root_extra->loaded_modules)
          cout << ' ' << ms.name;
      };

      // Print a potentially empty/null instance.
      //
      auto print_empty = [] (const auto& x)
      {
        if (!x.empty ())
          cout << ' ' << x;
      };

      auto print_null = [] (const auto* p)
      {
        if (p != nullptr && !p->empty ())
          cout << ' ' << *p;
      };

      // Print a potentially null/empty directory path without trailing slash.
      //
      auto print_dir = [] (const dir_path& d)
      {
        if (!d.empty ())
          cout << ' ' << d.string ();
      };

      auto print_pdir = [&print_dir] (const dir_path* d)
      {
        if (d != nullptr)
          print_dir (*d);
      };

      // This could be a simple project that doesn't set project name.
      //
      cout
        << "project:"        ; print_empty (project (rs)); cout << endl
        << "version:"        ; print_empty (cast_empty<string> (rs[ctx.var_version])); cout << endl
        << "summary:"        ; print_empty (cast_empty<string> (rs[ctx.var_project_summary])); cout << endl
        << "url:"            ; print_empty (cast_empty<string> (rs[ctx.var_project_url])); cout << endl
        << "src_root:"       ; print_dir (cast<dir_path> (rs[ctx.var_src_root])); cout << endl
        << "out_root:"       ; print_dir (cast<dir_path> (rs[ctx.var_out_root])); cout << endl
        << "amalgamation:"   ; print_pdir (*rs.root_extra->amalgamation); cout << endl;
      if (subp)
      {
        cout
          << "subprojects:"  ; print_null (*rs.root_extra->subprojects); cout << endl;
      }
      cout
        << "operations:"     ; print_ops (rs.root_extra->operations, ctx.operation_table); cout << endl
        << "meta-operations:"; print_ops (rs.root_extra->meta_operations, ctx.meta_operation_table); cout << endl
        << "modules:"        ; print_mods (); cout << endl;
    }
  }

#ifndef BUILD2_BOOTSTRAP
  static void
  info_execute_json (action_targets& ts, bool subp)
  {
    json::stream_serializer s (cout);
    s.begin_array ();

    for (size_t i (0); i != ts.size (); ++i)
    {
      const scope& rs (ts[i].as<scope> ());

      context& ctx (rs.ctx);

      s.begin_object ();

      // Print a potentially empty string.
      //
      auto print_string = [&s] (const char* n,
                                const string& v,
                                bool check = false)
      {
        if (!v.empty ())
          s.member (n, v, check);
      };

      // Print a potentially null/empty directory path without trailing slash.
      //
      auto print_dir = [&s] (const char* n, const dir_path& v)
      {
        if (!v.empty ())
          s.member (n, v.string ());
      };

      auto print_pdir = [&print_dir] (const char* n, const dir_path* v)
      {
        if (v != nullptr)
          print_dir (n, *v);
      };

      // Print [meta_]operation names (see info_lines() for details).
      //
      auto print_ops = [&s] (const char* name,
                             const auto& ov,
                             const auto& ot,
                             const auto& printer)
      {
        s.member_name (name, false /* check */);

        s.begin_array ();

        for (uint8_t id (2); id < ov.size (); ++id)
        {
          if (ov[id] != nullptr)
            printer (ot[id]);
        }

        s.end_array ();
      };

      // Note that we won't check some values for being valid UTF-8, since
      // their characters belong to even stricter character sets and/or are
      // read from buildfile which is already verified to be valid UTF-8.
      //
      print_string ("project", project (rs).string ());
      print_string ("version", cast_empty<string> (rs[ctx.var_version]));
      print_string ("summary", cast_empty<string> (rs[ctx.var_project_summary]));
      print_string ("url", cast_empty<string> (rs[ctx.var_project_url]));
      print_dir    ("src_root", cast<dir_path> (rs[ctx.var_src_root]));
      print_dir    ("out_root", cast<dir_path> (rs[ctx.var_out_root]));
      print_pdir   ("amalgamation", *rs.root_extra->amalgamation);

      // Print subprojects.
      //
      if (subp)
      {
        const subprojects* sps (*rs.root_extra->subprojects);

        if (sps != nullptr && !sps->empty ())
        {
          s.member_name ("subprojects", false /* check */);
          s.begin_array ();

          for (const auto& sp: *sps)
          {
            s.begin_object ();

            print_dir ("path", sp.second);

            // See find_subprojects() for details.
            //
            const string& n (sp.first.string ());

            if (!path::traits_type::is_separator (n.back ()))
              print_string ("name", n);

            s.end_object ();
          }

          s.end_array ();
        }
      }

      print_ops ("operations",
                 rs.root_extra->operations,
                 ctx.operation_table,
                 [&s] (const string& v) {s.value (v, false /* check */);});

      print_ops ("meta-operations",
                 rs.root_extra->meta_operations,
                 ctx.meta_operation_table,
                 [&s] (const meta_operation_data& v)
                 {
                   s.value (v.name, false /* check */);
                 });

      // Print modules.
      //
      if (!rs.root_extra->loaded_modules.empty ())
      {
        s.member_name ("modules", false /* check */);
        s.begin_array ();

        for (const module_state& ms: rs.root_extra->loaded_modules)
          s.value (ms.name, false /* check */);

        s.end_array ();
      }

      s.end_object ();
    }

    s.end_array ();
    cout << endl;
  }
#else
  static void
  info_execute_json (action_targets&, bool)
  {
  }
#endif //BUILD2_BOOTSTRAP

  static void
  info_execute (const values& params,
                action,
                action_targets& ts,
                uint16_t,
                bool)
  {
    info_params ip (info_parse_params (params));

    // Note that both outputs will not be "ideal" if the user does something
    // like `b info(foo/) info(bar/)` instead of `b info(foo/ bar/)`. Oh,
    // well.
    //
    if (ip.json)
      info_execute_json (ts, ip.subprojects);
    else
      info_execute_lines (ts, ip.subprojects);
  }

  const meta_operation_info mo_info {
    info_id,
    "info",
    "",
    "",
    "",
    "",
    false,     // bootstrap_outer
    &info_pre, // meta-operation pre
    &info_operation_pre,
    &info_load,
    &info_search,
    nullptr, // match
    &info_execute,
    nullptr, // operation post
    nullptr, // meta-operation post
    nullptr  // include
  };

  // operations
  //
  const operation_info op_default {
    default_id,
    0,
    "<default>",
    "",
    "",
    "",
    "",
    execution_mode::first,
    1 /* concurrency */,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
  };

#ifndef _MSC_VER
  constexpr
#else
  // VC doesn't "see" this can be const-initialized so we have to hack around
  // to ensure correct initialization order.
  //
  #pragma warning(disable: 4073)
  #pragma init_seg(lib)
  const
#endif
  operation_info op_update {
    update_id,
    0,
    "update",
    "update",
    "updating",
    "updated",
    "is up to date",
    execution_mode::first,
    1 /* concurrency */,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
  };

  const operation_info op_clean {
    clean_id,
    0,
    "clean",
    "clean",
    "cleaning",
    "cleaned",
    "is clean",
    execution_mode::last,
    1 /* concurrency */,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
  };
}
