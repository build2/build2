// file      : build/algorithm.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

namespace build
{
  template <typename T>
  T*
  update_prerequisites (target& t, const timestamp& mt)
  {
    //@@ Can factor the bulk of it into a non-template code. Can
    // either do a function template that will do dynamic_cast check
    // or can scan the target type info myself. I think latter.
    //

    T* r (nullptr);
    bool u (mt == timestamp_nonexistent);

    for (const prerequisite& p: t.prerequisites)
    {
      assert (p.target != nullptr);
      target& pt (*p.target);

      target_state ts (update (pt));

      if (!u)
      {
        // If this is an mtime-based target, then compare timestamps.
        //
        if (auto mpt = dynamic_cast<const mtime_target*> (&pt))
        {
          timestamp mp (mpt->mtime ());

          // What do we do if timestamps are equal? This can happen, for
          // example, on filesystems that don't have subsecond resolution.
          // There is not much we can do here except detect the case where
          // the prerequisite was updated in this run which means the
          // target must be out of date.
          //
          if (mt < mp || (mt == mp && ts == target_state::updated))
            u = true;
        }
        else
        {
          // Otherwise we assume the prerequisite is newer if it was updated.
          //
          if (ts == target_state::updated)
            u = true;
        }
      }

      if (r == nullptr)
        r = dynamic_cast<T*> (&pt);
    }

    assert (r != nullptr);
    return u ? r : nullptr;
  }
}
