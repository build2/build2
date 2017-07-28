// file      : build2/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/operation.hxx>

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
    // Load project's root[-pre].build.
    //
    load_root_pre (root);

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
  match (const values&, action a, action_targets& ts)
  {
    tracer trace ("match");

    if (verb >= 6)
      dump ();

    {
      phase_lock l (run_phase::match);

      // Setup progress reporting if requested.
      //
      scheduler::monitor_guard mg;
      string what;
      if (ops.progress ())
      {
        what = " targets to " + diag_do (a);

        mg = sched.monitor (
          target_count,
          10,
          [&what] (size_t c) -> size_t
          {
            diag_progress_lock pl;
            diag_progress  = ' ';
            diag_progress += to_string (c);
            diag_progress += what;
            return c + 10;
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
          const target& t (*static_cast<const target*> (ts[i]));
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
      if (ops.progress ())
      {
        diag_progress_lock pl;
        diag_progress.clear ();
      }

      // We are now running serially. Re-examine targets that we have matched.
      //
      bool fail (false);
      for (size_t j (0); j != n; ++j)
      {
        const target& t (*static_cast<const target*> (ts[j]));

        // Finish matching targets that we have started. Note that we use the
        // state for the "final" action that will be executed and not our
        // action. Failed that, we may fail to find a match for a "stronger"
        // action but will still get unchanged for the original one.
        //
        target_state s;
        if (j < i)
        {
          match (a, t, false);
          s = t.serial_state (false);
        }
        else
          s = target_state::postponed;

        switch (s)
        {
        case target_state::postponed:
          {
            // We bailed before matching it.
            //
            if (verb != 0)
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
            if (verb != 0)
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
    }

    // Phase restored to load.
    //
    assert (phase == run_phase::load);

    if (verb >= 6)
      dump ();
  }

  void
  execute (const values&, action a, action_targets& ts, bool quiet)
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
    scheduler::monitor_guard mg;
    string what;
    if (ops.progress ())
    {
      what = "% of targets " + diag_did (a);

      size_t init (target_count.load (memory_order_relaxed));
      size_t incr (init / 100); // 1%.
      if (incr == 0)
        incr = 1;

      mg = sched.monitor (
        target_count,
        init - incr,
        [&what, init, incr] (size_t c) -> size_t
        {
          size_t p ((init - c) * 100 / init);
          diag_progress_lock pl;
          diag_progress  = ' ';
          diag_progress += to_string (p);
          diag_progress += what;
          return c - incr;
        });
    }

    // Similar logic to execute_members(): first start asynchronous execution
    // of all the top-level targets.
    //
    {
      atomic_count task_count (0);
      wait_guard wg (task_count);

      for (const void* vt: ts)
      {
        const target& t (*static_cast<const target*> (vt));

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

    // Clear the progress if present.
    //
    if (ops.progress ())
    {
      diag_progress_lock pl;
      diag_progress.clear ();
    }

    sched.tune (0); // Restore original scheduler settings.

    // We are now running serially. Re-examine them all.
    //
    bool fail (false);
    for (const void* vt: ts)
    {
      const target& t (*static_cast<const target*> (vt));

      switch (t.executed_state (false))
      {
      case target_state::unknown:
        {
          // We bailed before executing it.
          //
          if (verb != 0 && !quiet)
            info << "not " << diag_did (a, t);

          break;
        }
      case target_state::unchanged:
        {
          // Nothing had to be done.
          //
          if (verb != 0 && !quiet)
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
          if (verb != 0 && !quiet)
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

  const meta_operation_info noop {
    noop_id,
    "noop",
    "",      // Presumably we will never need these since we are not going
    "",      // to do anything.
    "",
    "",
    nullptr, // meta-operation pre
    nullptr, // operation pre
    &load,
    nullptr, // search
    nullptr, // match
    nullptr, // execute
    nullptr, // operation post
    nullptr  // meta-operation post
  };

  const meta_operation_info perform {
    perform_id,
    "perform",
    "",
    "",
    "",
    "",
    nullptr, // meta-operation pre
    nullptr, // operation pre
    &load,
    &search,
    &match,
    &execute,
    nullptr, // operation post
    nullptr  // meta-operation post
  };

  // operations
  //
  const operation_info default_ {
    default_id,
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

  const operation_info update {
    update_id,
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

  const operation_info clean {
    clean_id,
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
