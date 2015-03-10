// file      : build/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/rule>

#include <utility>      // move()
#include <system_error>

#include <build/algorithm>
#include <build/diagnostics>
#include <build/timestamp>
#include <build/mkdir>

using namespace std;

namespace build
{
  operation_rule_map rules;
  const target_rule_map* current_rules;

  // path_rule
  //
  void* path_rule::
  match (action a, target& t, const string&) const
  {
    // @@ TODO:
    //
    // - need to try all the target-type-specific extensions, just
    //   like search_existing_file().
    //
    path_target& pt (dynamic_cast<path_target&> (t));

    if (pt.path ().empty ())
    {
      path p (t.dir / path (pt.name));

      // @@ TMP: target name as an extension.
      //
      const string& e (pt.ext != nullptr ? *pt.ext : pt.type ().name);

      if (!e.empty ())
      {
        p += '.';
        p += e;
      }

      // While strictly speaking we shouldn't do this in match(),
      // no other rule should ever be ambiguous with this fallback
      // one.
      //
      pt.path (move (p));
    }

    return pt.mtime () != timestamp_nonexistent ? &t : nullptr;
  }

  recipe path_rule::
  apply (action a, target& t, void*) const
  {
    // Search and match all the prerequisites.
    //
    search_and_match (a, t);

    return &update;
  }

  target_state path_rule::
  update (action a, target& t)
  {
    // Make sure the target is not older than any of its prerequisites.
    //
    timestamp mt (dynamic_cast<path_target&> (t).mtime ());

    for (const prerequisite& p: t.prerequisites)
    {
      target& pt (*p.target);
      target_state ts (update (a, pt));

      // If this is an mtime-based target, then compare timestamps.
      //
      if (auto mpt = dynamic_cast<const mtime_target*> (&pt))
      {
        timestamp mp (mpt->mtime ());

        if (mt < mp)
          fail << "no recipe to update target " << t <<
            info << "prerequisite " << pt << " is ahead of " << t
               << " by " << (mp - mt);
      }
      else
      {
        // Otherwise we assume the prerequisite is newer if it was changed.
        //
        if (ts == target_state::changed)
          fail << "no recipe to update target " << t <<
            info << "prerequisite " << pt << " is ahead of " << t
               << " because it was updated";
      }
    }

    return target_state::unchanged;
  }

  // dir_rule
  //
  void* dir_rule::
  match (action a, target& t, const string&) const
  {
    return &t;
  }

  recipe dir_rule::
  apply (action a, target& t, void*) const
  {
    search_and_match (a, t);
    return &update;
  }

  target_state dir_rule::
  update (action a, target& t)
  {
    // Return changed if any of our prerequsites were updated and
    // unchanged otherwise.
    //
    return execute_prerequisites (a, t);
  }

  // fsdir_rule
  //
  void* fsdir_rule::
  match (action a, target& t, const string&) const
  {
    return &t;
  }

  recipe fsdir_rule::
  apply (action a, target& t, void*) const
  {
    // Let's not allow any prerequisites for this target since it
    // doesn't make much sense. The sole purpose of this target type
    // is to create a directory.
    //
    if (!t.prerequisites.empty ())
      fail << "no prerequisites allowed for target " << t;

    return &update;
  }

  target_state fsdir_rule::
  update (action a, target& t)
  {
    path d (t.dir / path (t.name));

    // Add the extension back if it was specified.
    //
    if (t.ext != nullptr)
    {
      d += '.';
      d += *t.ext;
    }

    if (path_mtime (d) != timestamp_nonexistent)
      return target_state::unchanged;

    if (verb >= 1)
      text << "mkdir " << d.string ();
    else
      text << "mkdir " << t; //@@ Probably only show if [show]?

    try
    {
      mkdir (d);
    }
    catch (const system_error& e)
    {
      fail << "unable to create directory " << d.string () << ": "
           << e.what ();
    }

    return target_state::changed;
  }
}
