// file      : build2/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/operation>

#include <build2/scope>
#include <build2/target>
#include <build2/file>
#include <build2/algorithm>
#include <build2/diagnostics>
#include <build2/dump>

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
  load (const path& bf,
        scope& root,
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
    auto i (scopes.insert (out_base, false));
    scope& base (setup_base (i, out_base, src_base));

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

    if (verb >= 6)
      dump (a);

    for (void* vt: ts)
    {
      target& t (*static_cast<target*> (vt));
      l5 ([&]{trace << "matching " << t;});
      match (a, t);
    }

    if (verb >= 6)
      dump (a);
  }

  void
  execute (action a, const action_targets& ts, bool quiet)
  {
    tracer trace ("execute");

    // Execute collecting postponed targets (to be re-examined later).
    // Do it in reverse order if the execution mode is 'last'.
    //
    vector<reference_wrapper<target>> psp;

    auto body (
      [a, quiet, &psp, &trace] (void* v)
      {
        target& t (*static_cast<target*> (v));

        l5 ([&]{trace << diag_doing (a, t);});

        switch (execute (a, t))
        {
        case target_state::unchanged:
          {
            if (!quiet)
              info << diag_done (a, t);
            break;
          }
        case target_state::postponed:
          psp.push_back (t);
          break;
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

    // We should have executed every target that we matched.
    //
    assert (dependency_count == 0);

    // Re-examine postponed targets. This is the only reliable way to
    // find out whether the target has changed.
    //
    for (target& t: psp)
    {
      switch (execute (a, t))
      {
      case target_state::unchanged:
        {
          if (!quiet)
            info << diag_done (a, t);
          break;
        }
      case target_state::changed:
        break;
      case target_state::postponed:
        assert (false);
      case target_state::failed:
        //@@ This could probably happen in a parallel build.
      default:
        assert (false);
      }
    }
  }

  meta_operation_info noop {
    noop_id,
    "noop",
    "",      // Presumably we will never need these since we are not going
    "",      // to do anything.
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

  meta_operation_info perform {
    perform_id,
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
    default_id,
    "<default>",
    "",
    "",
    "",
    execution_mode::first,
    nullptr,
    nullptr
  };

  operation_info update {
    update_id,
    "update",
    "update",
    "updating",
    "is up to date",
    execution_mode::first,
    nullptr,
    nullptr
  };

  operation_info clean {
    clean_id,
    "clean",
    "clean",
    "cleaning",
    "is clean",
    execution_mode::last,
    nullptr,
    nullptr
  };

  // Tables.
  //
  string_table<meta_operation_id> meta_operation_table;
  string_table<operation_id> operation_table;
}
