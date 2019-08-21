// file      : libbuild2/context.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  // wait_guard
  //
  inline wait_guard::
  wait_guard ()
      : start_count (0), task_count (nullptr), phase (false)
  {
  }

  inline wait_guard::
  wait_guard (atomic_count& tc, bool p)
      : wait_guard (0, tc, p)
  {
  }

  inline wait_guard::
  wait_guard (size_t sc, atomic_count& tc, bool p)
      : start_count (sc), task_count (&tc), phase (p)
  {
  }

  inline wait_guard::
  ~wait_guard () noexcept (false)
  {
    if (task_count != nullptr)
      wait ();
  }

  inline wait_guard::
  wait_guard (wait_guard&& x)
      : start_count (x.start_count), task_count (x.task_count), phase (x.phase)
  {
    x.task_count = nullptr;
  }

  inline wait_guard& wait_guard::
  operator= (wait_guard&& x)
  {
    if (&x != this)
    {
      assert (task_count == nullptr);
      start_count = x.start_count; task_count = x.task_count; phase = x.phase;
      x.task_count = nullptr;
    }
    return *this;
  }

  inline void wait_guard::
  wait ()
  {
    phase_unlock u (phase);
    sched.wait (start_count, *task_count);
    task_count = nullptr;
  }
}
