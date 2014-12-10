// file      : build/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/rule>

#include <iostream>

using namespace std;

namespace build
{
  rule_map rules;

  // default_path_rule
  //
  recipe default_path_rule::
  match (target& t) const
  {
    // @@ TODO:
    //
    // - need to assign path somehow. Get (potentially several)
    //   extensions from target type? Maybe target type should
    //   generate a list of potential paths that we can try here.
    //

    path_target& pt (dynamic_cast<path_target&> (t));

    return pt.mtime () != timestamp_nonexistent ? &update : nullptr;
  }

  target_state default_path_rule::
  update (target& t)
  {
    // Make sure the target is not older than any of its prerequisites.
    //
    path_target& pt (dynamic_cast<path_target&> (t));
    timestamp mt (pt.mtime ());

    for (const target& p: t.prerequisites ())
    {
      // If this is an mtime-based target, then simply compare timestamps.
      //
      if (auto mtp = dynamic_cast<const mtime_target*> (&p))
      {
        if (mt < mtp->mtime ())
        {
          cerr << "error: no rule to update target " << t << endl
               << "info: prerequisite " << p << " is ahead of " << t <<
            " by " << (mtp->mtime () - mt) << endl;

          return target_state::failed;
        }
      }
      else
      {
        // Otherwise we assume the prerequisite is newer if it was updated.
        //
        if (p.state () == target_state::updated)
        {
          cerr << "error: no rule to update target " << t << endl
               << "info: prerequisite " << p << " is ahead of " << t <<
            " because it was updated" << endl;

          return target_state::failed;
        }
      }
    }

    return target_state::uptodate;
  }
}
