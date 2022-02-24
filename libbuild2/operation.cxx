// file      : libbuild2/operation.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/operation.hxx>

#include <iostream>      // cout
#include <unordered_map>

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/diagnostics.hxx>

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
    &load,
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
  load (const values&,
        scope& root,
        const path& bf,
        const dir_path& out_base,
        const dir_path& src_base,
        const location&)
  {
    // Load project's root.build.
    //
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
  search (const values&,
          const scope&,
          const scope& bs,
          const path& bf,
          const target_key& tk,
          const location& l,
          action_targets& ts)
  {
    tracer trace ("search");

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

        if (s.task_count.load (memory_order_relaxed) - ctx.count_base () <
            target::offset_matched)
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
          dr << info << "target " << *t << " is not explicitly declared "
             << "in any buildfile" <<
            info << "perhaps it is a dynamic dependency?";
        }
      }
    }

    if (e)
      throw failed ();
  }

  void
  match (const values&, action a, action_targets& ts, uint16_t diag, bool prog)
  {
    tracer trace ("match");

    if (ts.empty ())
      return;

    context& ctx (ts[0].as<target> ().ctx);

    {
      phase_lock l (ctx, run_phase::match);

      // Setup progress reporting if requested.
      //
      string what; // Note: must outlive monitor_guard.
      scheduler::monitor_guard mg;

      if (prog && show_progress (2 /* max_verb */))
      {
        size_t incr (stderr_term ? 1 : 10); // Scale depending on output type.

        what = " targets to " + diag_do (ctx, a);

        mg = ctx.sched.monitor (
          ctx.target_count,
          incr,
          [incr, &what] (size_t c) -> size_t
          {
            diag_progress_lock pl;
            diag_progress  = ' ';
            diag_progress += to_string (c);
            diag_progress += what;
            return c + incr;
          });
      }

      // Start asynchronous matching of prerequisites keeping track of how
      // many we have started. Wait with unlocked phase to allow phase
      // switching.
      //
      size_t i (0), n (ts.size ());
      {
        atomic_count task_count (0);
        wait_guard wg (ctx, task_count, true);

        for (; i != n; ++i)
        {
          const target& t (ts[i].as<target> ());
          l5 ([&]{trace << diag_doing (a, t);});

          target_state s (match_async (a, t, 0, task_count, false));

          // Bail out if the target has failed and we weren't instructed to
          // keep going.
          //
          if (s == target_state::failed && !ctx.keep_going)
          {
            ++i;
            break;
          }
        }

        wg.wait ();
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
      bool fail (false);
      for (size_t j (0); j != n; ++j)
      {
        action_target& at (ts[j]);
        const target& t (at.as<target> ());

        target_state s (j < i
                        ? match (a, t, false)
                        : target_state::postponed);
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
  execute (const values&, action a, action_targets& ts,
           uint16_t diag, bool prog)
  {
    tracer trace ("execute");

    if (ts.empty ())
      return;

    context& ctx (ts[0].as<target> ().ctx);

    // Reverse the order of targets if the execution mode is 'last'.
    //
    if (ctx.current_mode == execution_mode::last)
      reverse (ts.begin (), ts.end ());

    phase_lock pl (ctx, run_phase::execute); // Never switched.

    {
      // Tune the scheduler.
      //
      using tune_guard = scheduler::tune_guard;
      tune_guard sched_tune;

      switch (ctx.current_inner_oif->concurrency)
      {
      case 0: sched_tune = tune_guard (ctx.sched, 1); break; // Run serially.
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

          mg = ctx.sched.monitor (
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

      // Similar logic to execute_members(): first start asynchronous
      // execution of all the top-level targets.
      //
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
          if (s == target_state::failed && !ctx.keep_going)
            break;
        }

        wg.wait ();
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
    bool fail (false);
    for (action_target& at: ts)
    {
      const target& t (at.as<target> ());

      switch ((at.state = t.executed_state (a, false)))
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

    // We should have executed every target that we matched, provided we
    // haven't failed (in which case we could have bailed out early).
    //
    assert (ctx.target_count.load (memory_order_relaxed) == 0);

#ifndef NDEBUG
    if (ctx.dependency_count.load (memory_order_relaxed) != 0)
    {
      diag_record dr;
      dr << info << "detected unexecuted matched targets:";

      for (const auto& pt: ctx.targets)
      {
        const target& t (*pt);
        if (size_t n = t[a].dependents.load (memory_order_relaxed))
          dr << text << t << ' ' << n;
      }
    }
#endif
    assert (ctx.dependency_count.load (memory_order_relaxed) == 0);
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
    &load,
    &search,
    &match,
    &execute,
    nullptr, // operation post
    nullptr, // meta-operation post
    nullptr  // include
  };

  // info
  //
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
  info_execute (const values&, action, action_targets& ts, uint16_t, bool)
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
        for (const module_state& ms: rs.root_extra->modules)
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

      // This could be a simple project that doesn't set project name.
      //
      cout
        << "project:"        ; print_empty (project (rs)); cout << endl
        << "version:"        ; print_empty (cast_empty<string> (rs[ctx.var_version])); cout << endl
        << "summary:"        ; print_empty (cast_empty<string> (rs[ctx.var_project_summary])); cout << endl
        << "url:"            ; print_empty (cast_empty<string> (rs[ctx.var_project_url])); cout << endl
        << "src_root: "     << cast<dir_path> (rs[ctx.var_src_root]) << endl
        << "out_root: "     << cast<dir_path> (rs[ctx.var_out_root]) << endl
        << "amalgamation:"   ; print_null (*rs.root_extra->amalgamation); cout << endl
        << "subprojects:"    ; print_null (*rs.root_extra->subprojects); cout << endl
        << "operations:"     ; print_ops (rs.root_extra->operations, ctx.operation_table); cout << endl
        << "meta-operations:"; print_ops (rs.root_extra->meta_operations, ctx.meta_operation_table); cout << endl
        << "modules:"        ; print_mods (); cout << endl;
    }
  }

  const meta_operation_info mo_info {
    info_id,
    "info",
    "",
    "",
    "",
    "",
    false,   // bootstrap_outer
    nullptr, // meta-operation pre
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
    nullptr,
    "",
    "",
    "",
    "",
    execution_mode::first,
    1 /* concurrency */,
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
    "update",
    "updating",
    "updated",
    "is up to date",
    execution_mode::first,
    1 /* concurrency */,
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
    "clean",
    "cleaning",
    "cleaned",
    "is clean",
    execution_mode::last,
    1 /* concurrency */,
    nullptr,
    nullptr,
    nullptr,
    nullptr
  };
}
