// file      : build/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/operation>

#include <ostream>
#include <cassert>
#include <functional> // reference_wrapper

#include <butl/utility> // reverse_iterate

#include <build/scope>
#include <build/target>
#include <build/file>
#include <build/algorithm>
#include <build/diagnostics>
#include <build/dump>

using namespace std;
using namespace butl;

namespace build
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
  load (const path& bf,
        scope& root,
        const dir_path& out_base,
        const dir_path& src_base,
        const location&)
  {
    // Load project's root[-pre].build.
    //
    load_root_pre (root);

    // Create the base scope. Note that its existence doesn't
    // mean it was already processed as a base scope; it can
    // be the same as root.
    //
    scope& base (scopes[out_base]);

    base.assign ("out_base") = out_base;
    auto v (base.assign ("src_base") = src_base);
    base.src_path_ = &v.as<const dir_path&> ();

    // Load the buildfile unless it has already been loaded.
    //
    source_once (bf, root, base, root);
  }

  void
  search (scope&,
          const target_key& tk,
          const location& l,
          action_targets& ts)
  {
    tracer trace ("search");

    auto i (targets.find (tk, trace));
    if (i == targets.end ())
      fail (l) << "unknown target " << tk;

    ts.push_back (i->get ());
  }

  void
  match (action a, action_targets& ts)
  {
    tracer trace ("match");

    if (verb >= 5)
      dump (a);

    for (void* vt: ts)
    {
      target& t (*static_cast<target*> (vt));
      level4 ([&]{trace << "matching " << t;});
      match (a, t);
    }

    if (verb >= 5)
      dump (a);
  }

  void
  execute (action a, const action_targets& ts)
  {
    tracer trace ("execute");

    // Build collecting postponed targets (to be re-examined later).
    //
    vector<reference_wrapper<target>> psp;

    // Execute targets in reverse if the execution mode is 'last'.
    //
    auto body (
      [a, &psp, &trace] (void* v)
      {
        target& t (*static_cast<target*> (v));

        level4 ([&]{trace << diag_doing (a, t);});

        switch (execute (a, t))
        {
        case target_state::postponed:
          {
            info << diag_doing (a, t) << " is postponed";
            psp.push_back (t);
            break;
          }
        case target_state::unchanged:
          {
            // Be quiet in pre/post operations.
            //
            if (a.outer_operation () == 0)
              info << diag_already_done (a, t);
            break;
          }
        case target_state::changed:
          break;
        case target_state::failed:
          //@@ This could probably happen in a parallel build.
        default:
          assert (false);
        }
      });

    if (current_mode == execution_mode::first)
      for (void* v: ts) body (v);
    else
      for (void* v: reverse_iterate (ts)) body (v);

    // Re-examine postponed targets. Note that we will do it in the
    // order added, so no need to check the execution mode.
    //
    for (target& t: psp)
    {
      if (t.state () == target_state::postponed)
        execute_direct (a, t); // Try again, now ignoring the execution mode.

      switch (t.state ())
      {
      case target_state::postponed:
        {
          info << "unable to " << diag_do (a, t) << " at this time";
          break;
        }
      case target_state::unchanged:
        {
          // Be quiet in pre/post operations.
          //
          if (a.outer_operation () == 0)
            info << diag_already_done (a, t);
          break;
        }
      case target_state::unknown: // Assume something was done to it.
      case target_state::changed:
        break;
      case target_state::failed:
        //@@ This could probably happen in a parallel build.
      default:
        assert (false);
      }
    }
  }

  meta_operation_info perform {
    "perform",
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
  operation_info default_ {
    "<default>",
    "",
    "",
    "",
    execution_mode::first,
    nullptr,
    nullptr
  };

  operation_info update {
    "update",
    "update",
    "updating",
    "up to date",
    execution_mode::first,
    nullptr,
    nullptr
  };

  operation_info clean {
    "clean",
    "clean",
    "cleaning",
    "clean",
    execution_mode::last,
    nullptr,
    nullptr
  };
}
