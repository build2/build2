// file      : build2/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/operation>

#include <build2/file>
#include <build2/dump>
#include <build2/scope>
#include <build2/target>
#include <build2/algorithm>
#include <build2/scheduler>
#include <build2/diagnostics>

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
  load (scope& root,
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

    // Load the buildfile unless it has already been loaded.
    //
    source_once (root, base, bf, root);
  }

  void
  search (const scope&,
          const target_key& tk,
          const location& l,
          action_targets& ts)
  {
    tracer trace ("search");

    phase_guard pg (run_phase::search_match);

    if (target* t = targets.find (tk, trace))
      ts.push_back (t);
    else
      fail (l) << "unknown target " << tk;
  }

  void
  match (action a, action_targets& ts)
  {
    tracer trace ("match");

    if (verb >= 6)
      dump (a);

    {
      phase_guard pg (run_phase::search_match);

      if (ts.size () > 1)
        sched.tune (1); //@@ MT TMP run serially.

      scheduler::atomic_count task_count (0);
      {
        model_slock ml;

        for (void* vt: ts)
        {
          target& t (*static_cast<target*> (vt));
          l5 ([&]{trace << "matching " << t;});

          sched.async (task_count,
                       [a] (target& t)
                       {
                         model_slock ml;
                         match (ml, a, t); // @@ MT exception handling.
                       },
                       ref (t));
        }
      }
      sched.wait (task_count);

      sched.tune (0); //@@ MT TMP run serially restore.
    }

    if (verb >= 6)
      dump (a);
  }

  void
  execute (action a, action_targets& ts, bool quiet)
  {
    tracer trace ("execute");

    // Reverse the order of targets if the execution mode is 'last'.
    //
    if (current_mode == execution_mode::last)
      reverse (ts.begin (), ts.end ());

    phase_guard pg (run_phase::execute);

    // Tune the scheduler.
    //
    switch (current_inner_oif->concurrency)
    {
    case 0: sched.tune (1); break;          // Run serially.
    case 1:                 break;          // Run as is.
    default:                assert (false); // Not yet supported.
    }

    // Similar logic to execute_members(): first start asynchronous execution
    // of all the top-level targets.
    //
    atomic_count task_count (0);

    size_t n (ts.size ());
    for (size_t i (0); i != n; ++i)
    {
      const target& t (*static_cast<const target*> (ts[i]));

      l5 ([&]{trace << diag_doing (a, t);});

      target_state s (execute_async (a, t, 0, task_count));

      if (s == target_state::failed && !keep_going)
        break;
    }
    sched.wait (task_count);
    sched.tune (0); // Restore original scheduler settings.

    // We are now running serially. Re-examine them all.
    //
    bool fail (false);
    for (size_t i (0); i != n; ++i)
    {
      const target& t (*static_cast<const target*> (ts[i]));

      switch (t.synchronized_state (false))
      {
      case target_state::unknown:
        {
          // We bailed before executing it.
          //
          if (!quiet)
            info << "not " << diag_did (a, t);

          break;
        }
      case target_state::unchanged:
        {
          // Nothing had to be done.
          //
          if (!quiet)
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
          if (!quiet)
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
    assert (dependency_count == 0);
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
  string_table<meta_operation_id> meta_operation_table;
  string_table<operation_id> operation_table;
}
