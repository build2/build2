// file      : build/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/rule>

#include <utility>  // move()

#include <build/algorithm>
#include <build/diagnostics>

using namespace std;

namespace build
{
  rule_map rules;

  // path_rule
  //
  void* path_rule::
  match (target& t, const string&) const
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
  apply (target& t, void*) const
  {
    // Search and match all the prerequisites.
    //
    search_and_match (t);

    return &update;
  }

  target_state path_rule::
  update (target& t)
  {
    // Make sure the target is not older than any of its prerequisites.
    //
    timestamp mt (dynamic_cast<path_target&> (t).mtime ());

    for (const prerequisite& p: t.prerequisites)
    {
      target& pt (*p.target);
      target_state ts (update (pt));

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
        // Otherwise we assume the prerequisite is newer if it was updated.
        //
        if (ts == target_state::updated)
          fail << "no recipe to update target " << t <<
            info << "prerequisite " << pt << " is ahead of " << t
               << " because it was updated";
      }
    }

    return target_state::uptodate;
  }

  // dir_rule
  //
  void* dir_rule::
  match (target& t, const string&) const
  {
    return &t;
  }

  recipe dir_rule::
  apply (target& t, void*) const
  {
    search_and_match (t);
    return &update;
  }

  target_state dir_rule::
  update (target& t)
  {
    // Return updated if any of our prerequsites were updated and
    // uptodate otherwise.
    //
    return update_prerequisites (t);
  }
}
