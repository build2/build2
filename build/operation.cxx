// file      : build/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/operation>

#include <ostream>
#include <cassert>
#include <functional> // reference_wrapper

#include <build/scope>
#include <build/target>
#include <build/file>
#include <build/algorithm>
#include <build/diagnostics>
#include <build/dump>

using namespace std;

namespace build
{
  // action
  //
  ostream&
  operator<< (ostream& os, action a)
  {
    return os << '('
              << static_cast<uint16_t> (a.meta_operation ()) << ','
              << static_cast<uint16_t> (a.operation ())
              << ')';
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

    base.variables["out_base"] = out_base;
    auto v (base.variables["src_base"] = src_base);
    base.src_path_ = &v.as<const dir_path&> ();

    // Load the buildfile unless it has already been loaded.
    //
    source_once (bf, root, base, root);
  }

  void
  match (action a,
         scope&,
         const target_key& tk,
         const location& l,
         action_targets& ts)
  {
    tracer trace ("match");

    auto i (targets.find (tk, trace));
    if (i == targets.end ())
      fail (l) << "unknown target " << tk;

    target& t (**i);

    if (verb >= 5)
      dump ();

    level4 ([&]{trace << "matching " << t;});
    match (a, t);

    if (verb >= 5)
      dump ();

    ts.push_back (&t);
  }

  void
  execute (action a, const action_targets& ts)
  {
    tracer trace ("execute");

    // Build collecting postponed targets (to be re-examined later).
    //
    vector<reference_wrapper<target>> psp;

    for (void* v: ts)
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
    }

    // Re-examine postponed targets.
    //
    for (target& t: psp)
    {
      switch (t.state)
      {
      case target_state::postponed:
        {
          info << "unable to " << diag_do (a, t) << " at this time";
          break;
        }
      case target_state::unchanged:
        {
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
    execution_mode::first
  };

  operation_info update {
    "update",
    "update",
    "updating",
    "up to date",
    execution_mode::first
  };

  operation_info clean {
    "clean",
    "clean",
    "cleaning",
    "clean",
    execution_mode::last};
}
