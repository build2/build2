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

  inline void
  set_current_mif (const meta_operation_info& mif)
  {
    if (current_mname != mif.name)
    {
      current_mname = mif.name;
      global_scope->rw ().assign (var_build_meta_operation) = mif.name;
    }

    current_mif = &mif;
    current_on = 0; // Reset.
  }

  inline void
  set_current_oif (const operation_info& inner_oif,
                   const operation_info* outer_oif,
                   bool diag_noise)
  {
    current_oname = (outer_oif == nullptr ? inner_oif : *outer_oif).name;
    current_inner_oif = &inner_oif;
    current_outer_oif = outer_oif;
    current_on++;
    current_mode = inner_oif.mode;
    current_diag_noise = diag_noise;

    // Reset counters (serial execution).
    //
    dependency_count.store (0, memory_order_relaxed);
    target_count.store (0, memory_order_relaxed);
    skip_count.store (0, memory_order_relaxed);
  }
}
