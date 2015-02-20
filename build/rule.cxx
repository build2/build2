// file      : build/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/rule>

#include <utility>  // move()

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
    // - need to assign path somehow. Get (potentially several)
    //   extensions from target type? Maybe target type should
    //   generate a list of potential paths that we can try here.
    //   What if none of them exist, which one do we use? Should
    //   there be a default extension, perhaps configurable via
    //   a variable?
    //

    path_target& pt (dynamic_cast<path_target&> (t));

    if (pt.path ().empty ())
    {
      path p (t.dir / path (pt.name));

      // @@ TMP: derive file name by appending target name as an extension?
      //
      const string& e (pt.ext != nullptr ? *pt.ext : pt.type ().name);

      if (!e.empty ())
      {
        p += '.';
        p += e;
      }

      pt.path (move (p));
    }

    return pt.mtime () != timestamp_nonexistent ? &t : nullptr;
  }

  recipe path_rule::
  select (target&, void*) const
  {
    return &update;
  }

  target_state path_rule::
  update (target& t)
  {
    // Make sure the target is not older than any of its prerequisites.
    //
    path_target& pt (dynamic_cast<path_target&> (t));
    timestamp mt (pt.mtime ());

    for (const prerequisite& p: t.prerequisites)
    {
      const target& pt (*p.target); // Should be resolved at this stage.

      // If this is an mtime-based target, then simply compare timestamps.
      //
      if (auto mtp = dynamic_cast<const mtime_target*> (&pt))
      {
        if (mt < mtp->mtime ())
        {
          error << "no rule to update target " << t <<
            info << "prerequisite " << pt << " is ahead of " << t
                << " by " << (mtp->mtime () - mt);

          return target_state::failed;
        }
      }
      else
      {
        // Otherwise we assume the prerequisite is newer if it was updated.
        //
        if (pt.state () == target_state::updated)
        {
          error << "no rule to update target " << t <<
            info << "prerequisite " << pt << " is ahead of " << t
                << " because it was updated";

          return target_state::failed;
        }
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
  select (target&, void*) const
  {
    return &update;
  }

  target_state dir_rule::
  update (target& t)
  {
    for (const prerequisite& p: t.prerequisites)
    {
      auto ts (p.target->state ());

      if (ts != target_state::uptodate)
        return ts; // updated or failed
    }

    return target_state::uptodate;
  }
}
