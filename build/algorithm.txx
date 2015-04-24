// file      : build/algorithm.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/context>
#include <build/utility> // reverse_iterate

namespace build
{
  template <executor_function* E>
  target_state
  execute_prerequisites (action a, target& t)
  {
    target_state ts (target_state::unchanged);

    if (t.group != nullptr)
      ts = execute_prerequisites<E> (a, *t.group);

    for (target* pt: t.prerequisites)
    {
      if (pt == nullptr) // Skip ignored.
        continue;

      if (E (a, *pt) == target_state::changed)
        ts = target_state::changed;
    }

    return ts;
  }

  template <executor_function* E>
  target_state
  reverse_execute_prerequisites (action a, target& t)
  {
    target_state ts (target_state::unchanged);

    for (target* pt: reverse_iterate (t.prerequisites))
    {
      if (pt == nullptr) // Skip ignored.
        continue;

      if (E (a, *pt) == target_state::changed)
        ts = target_state::changed;
    }

    if (t.group != nullptr)
    {
      if (reverse_execute_prerequisites<E> (a, *t.group) ==
            target_state::changed)
        ts = target_state::changed;
    }

    return ts;
  }

  template <executor_function* E>
  bool
  execute_prerequisites (action a, target& t, const timestamp& mt)
  {
    bool e (mt == timestamp_nonexistent);

    if (t.group != nullptr)
    {
      if (execute_prerequisites<E> (a, *t.group, mt))
        e = true;
    }

    for (target* pt: t.prerequisites)
    {
      if (pt == nullptr) // Skip ignored.
        continue;

      target_state ts (E (a, *pt));

      if (!e)
      {
        // If this is an mtime-based target, then compare timestamps.
        //
        if (auto mpt = dynamic_cast<const mtime_target*> (pt))
        {
          timestamp mp (mpt->mtime ());

          // What do we do if timestamps are equal? This can happen, for
          // example, on filesystems that don't have subsecond resolution.
          // There is not much we can do here except detect the case where
          // the prerequisite was changed in this run which means the
          // action must be executed on the target as well.
          //
          if (mt < mp || (mt == mp && ts == target_state::changed))
            e = true;
        }
        else
        {
          // Otherwise we assume the prerequisite is newer if it was changed.
          //
          if (ts == target_state::changed)
            e = true;
        }
      }
    }

    return e;
  }

  template <typename T, executor_function* E>
  T*
  execute_find_prerequisites (
    action a, target& t, const timestamp& mt, bool& e)
  {
    //@@ Can factor the bulk of it into a non-template code. Can
    // either do a function template that will do dynamic_cast check
    // or can scan the target type info myself. I think latter.
    //
    T* r (nullptr);

    if (t.group != nullptr)
      r = execute_find_prerequisites<T, E> (a, *t.group, mt, e);

    for (target* pt: t.prerequisites)
    {
      if (pt == nullptr) // Skip ignored.
        continue;

      target_state ts (E (a, *pt));

      if (!e)
      {
        // If this is an mtime-based target, then compare timestamps.
        //
        if (auto mpt = dynamic_cast<const mtime_target*> (pt))
        {
          timestamp mp (mpt->mtime ());

          // What do we do if timestamps are equal? This can happen, for
          // example, on filesystems that don't have subsecond resolution.
          // There is not much we can do here except detect the case where
          // the prerequisite was changed in this run which means the
          // action must be executed on the target as well.
          //
          if (mt < mp || (mt == mp && ts == target_state::changed))
            e = true;
        }
        else
        {
          // Otherwise we assume the prerequisite is newer if it was changed.
          //
          if (ts == target_state::changed)
            e = true;
        }
      }

      if (T* tmp = dynamic_cast<T*> (pt))
        r = tmp;
    }

    return r;
  }

  template <executor_function* E = execute>
  target_state
  default_action (action a, target& t)
  {
    return current_mode == execution_mode::first
      ? execute_prerequisites<E> (a, t)
      : reverse_execute_prerequisites<E> (a, t);
  }

  template <executor_function* E>
  target_state
  perform_clean (action a, target& t)
  {
    // The reverse order of update: first delete the file, then clean
    // prerequisites.
    //
    file& ft (dynamic_cast<file&> (t));

    bool r (rmfile (ft.path (), ft));

    // Update timestamp in case there are operations after us that
    // could use the information.
    //
    ft.mtime (timestamp_nonexistent);

    // Clean prerequisites.
    //
    target_state ts (reverse_execute_prerequisites<E> (a, t));

    return r ? target_state::changed : ts;
  }
}
