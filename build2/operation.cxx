// file      : build2/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/operation.hxx>

#include <iostream> // cout

#include <build2/file.hxx>
#include <build2/dump.hxx>
#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/algorithm.hxx>
#include <build2/diagnostics.hxx>

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
    nullptr  // meta-operation post
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
    auto i (scopes.rw (root).insert (out_base, false));
    scope& base (setup_base (i, out_base, src_base));

    // Load the buildfile unless it is implied.
    //
    if (!bf.empty ())
      source_once (root, base, bf, root);
  }

  void
  search (const values&,
          const scope&,
          const scope& bs,
          const target_key& tk,
          const location& l,
          action_targets& ts)
  {
    tracer trace ("search");

    phase_lock pl (run_phase::match);

    const target* t (targets.find (tk, trace));

    if (t == nullptr && tk.is_a<dir> ())
      t = dir::search_implied (bs, tk, trace);

    if (t == nullptr)
      fail (l) << "unknown target " << tk;

    ts.push_back (t);
  }

  void
  match (const values&, action a, action_targets& ts, uint16_t diag, bool prog)
  {
    tracer trace ("match");

    if (verb >= 6)
      dump ();

    {
      phase_lock l (run_phase::match);

      // Setup progress reporting if requested.
      //
      string what; // Note: must outlive monitor_guard.
      scheduler::monitor_guard mg;

      if (prog && show_progress (2 /* max_verb */))
      {
        size_t incr (stderr_term ? 1 : 10); // Scale depending on output type.

        what = " targets to " + diag_do (a);

        mg = sched.monitor (
          target_count,
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
        wait_guard wg (task_count, true);

        for (; i != n; ++i)
        {
          const target& t (ts[i].as_target ());
          l5 ([&]{trace << diag_doing (a, t);});

          target_state s (match_async (a, t, 0, task_count, false));

          // Bail out if the target has failed and we weren't instructed to
          // keep going.
          //
          if (s == target_state::failed && !keep_going)
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
        const target& t (at.as_target ());

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
    }

    // Phase restored to load.
    //
    assert (phase == run_phase::load);

    if (verb >= 6)
      dump ();
  }

  void
  execute (const values&, action a, action_targets& ts,
           uint16_t diag, bool prog)
  {
    tracer trace ("execute");

    // Reverse the order of targets if the execution mode is 'last'.
    //
    if (current_mode == execution_mode::last)
      reverse (ts.begin (), ts.end ());

    // Tune the scheduler.
    //
    switch (current_inner_oif->concurrency)
    {
    case 0: sched.tune (1); break;          // Run serially.
    case 1:                 break;          // Run as is.
    default:                assert (false); // Not yet supported.
    }

    phase_lock pl (run_phase::execute); // Never switched.

    // Setup progress reporting if requested.
    //
    string what; // Note: must outlive monitor_guard.
    scheduler::monitor_guard mg;

    if (prog && show_progress (1 /* max_verb */))
    {
      size_t init (target_count.load (memory_order_relaxed));
      size_t incr (init > 100 ? init / 100 : 1); // 1%.

      if (init != incr)
      {
        what = "% of targets " + diag_did (a);

        mg = sched.monitor (
          target_count,
          init - incr,
          [init, incr, &what] (size_t c) -> size_t
          {
            size_t p ((init - c) * 100 / init);
            size_t s (skip_count.load (memory_order_relaxed));

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

    // Similar logic to execute_members(): first start asynchronous execution
    // of all the top-level targets.
    //
    {
      atomic_count task_count (0);
      wait_guard wg (task_count);

      for (const action_target& at: ts)
      {
        const target& t (at.as_target ());

        l5 ([&]{trace << diag_doing (a, t);});

        target_state s (execute_async (a, t, 0, task_count, false));

        // Bail out if the target has failed and we weren't instructed to keep
        // going.
        //
        if (s == target_state::failed && !keep_going)
          break;
      }

      wg.wait ();
    }

    // We are now running serially.
    //

    sched.tune (0); // Restore original scheduler settings.

    // Clear the progress if present.
    //
    if (mg)
    {
      diag_progress_lock pl;
      diag_progress.clear ();
    }

    // Print skip count if not zero. Note that we print it regardless of the
    // diag level since this is essentially a "summary" of all the commands
    // that we did not (and, in fact, used to originally) print.
    //
    if (verb != 0)
    {
      if (size_t s = skip_count.load (memory_order_relaxed))
      {
        text << "skipped " << diag_doing (a) << ' ' << s << " targets";
      }
    }

    // Re-examine all the targets and print diagnostics.
    //
    bool fail (false);
    for (action_target& at: ts)
    {
      const target& t (at.as_target ());

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
    assert (target_count.load (memory_order_relaxed) == 0);
    assert (dependency_count.load (memory_order_relaxed) == 0);
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
    nullptr  // meta-operation post
  };

  // info
  //
  static operation_id
  info_operation_pre (const values&, operation_id o)
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

    setup_base (scopes.rw (rs).insert (out_base, false), out_base, src_base);
  }

  void
  info_search (const values&,
               const scope& rs,
               const scope&,
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

      const scope& s (*static_cast<const scope*> (ts[i].target));

      // This could be a simple project that doesn't set project name.
      //
      cout
        << "project: "      << cast_empty<string> (s[var_project]) << endl
        << "version: "      << cast_empty<string> (s[var_version]) << endl
        << "summary: "      << cast_empty<string> (s[var_project_summary]) << endl
        << "url: "          << cast_empty<string> (s[var_project_url]) << endl
        << "src_root: "     << cast<dir_path> (s[var_src_root]) << endl
        << "out_root: "     << cast<dir_path> (s[var_out_root]) << endl
        << "amalgamation: " << cast_empty<dir_path> (s[var_amalgamation]) << endl
        << "subprojects: "  << cast_empty<subprojects> (s[var_subprojects]) << endl;
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
    nullptr  // meta-operation post
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
    1,
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
    1,
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
    1,
    nullptr,
    nullptr
  };

  // Tables.
  //
  string_table<meta_operation_id, meta_operation_data> meta_operation_table;
  string_table<operation_id> operation_table;
}
