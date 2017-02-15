// file      : build2/context.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  // phase_lock
  //
  inline phase_lock::
  phase_lock (run_phase p)
      : p (p)
  {
    if (phase_lock* l = instance)
      assert (l->p == p);
    else
    {
      phase_mutex::instance.lock (p);
      instance = this;

      //text << this_thread::get_id () << " phase acquire " << p;
    }
  }

  inline phase_lock::
  ~phase_lock ()
  {
    if (instance == this)
    {
      instance = nullptr;
      phase_mutex::instance.unlock (p);

      //text << this_thread::get_id () << " phase release " << p;
    }
  }

  // phase_unlock
  //
  inline phase_unlock::
  phase_unlock (bool u)
      : l (u ? phase_lock::instance : nullptr)
  {
    if (u)
    {
      phase_lock::instance = nullptr;
      phase_mutex::instance.unlock (l->p);

      //text << this_thread::get_id () << " phase unlock  " << l->p;
    }
  }

  inline phase_unlock::
  ~phase_unlock ()
  {
    if (l != nullptr)
    {
      phase_mutex::instance.lock (l->p);
      phase_lock::instance = l;

      //text << this_thread::get_id () << " phase lock    " << l->p;
    }
  }

  // phase_switch
  //
  inline phase_switch::
  phase_switch (run_phase n)
      : o (phase), n (n)
  {
    phase_mutex::instance.relock (o, n);
    phase_lock::instance->p = n;

    if (n == run_phase::load) // Note: load lock is exclusive.
      load_generation++;

    //text << this_thread::get_id () << " phase switch  " << o << " " << n;
  }

  inline phase_switch::
  ~phase_switch ()
  {
    phase_mutex::instance.relock (n, o);
    phase_lock::instance->p = o;

    //text << this_thread::get_id () << " phase restore " << n << " " << o;
  }

  // wait_guard
  //
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

  inline void wait_guard::
  wait ()
  {
    phase_unlock u (phase);
    sched.wait (start_count, *task_count);
    task_count = nullptr;
  }
}
