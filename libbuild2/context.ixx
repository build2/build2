// file      : libbuild2/context.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  // wait_guard
  //
  inline wait_guard::
  wait_guard ()
      : ctx (nullptr), start_count (0), task_count (nullptr), phase (false)
  {
  }

  inline wait_guard::
  wait_guard (context& c, atomic_count& tc, bool p)
      : wait_guard (c, 0, tc, p)
  {
  }

  inline wait_guard::
  wait_guard (context& c, size_t sc, atomic_count& tc, bool p)
      : ctx (&c), start_count (sc), task_count (&tc), phase (p)
  {
  }

  inline wait_guard::
  ~wait_guard () noexcept (false)
  {
    // Don't work our own queue since we are most likely in stack unwinding
    // causes by an exception.
    //
    if (task_count != nullptr)
      wait (false);
  }

  inline wait_guard::
  wait_guard (wait_guard&& x) noexcept
      : ctx (x.ctx),
        start_count (x.start_count),
        task_count (x.task_count),
        phase (x.phase)
  {
    x.task_count = nullptr;
  }

  inline wait_guard& wait_guard::
  operator= (wait_guard&& x) noexcept
  {
    if (&x != this)
    {
      assert (task_count == nullptr);
      ctx = x.ctx;
      start_count = x.start_count; task_count = x.task_count; phase = x.phase;
      x.task_count = nullptr;
    }
    return *this;
  }

  inline void wait_guard::
  wait (bool wq)
  {
    // @@ TMP: remove if the queue_mark approach pans out.
    //
#if 0
    // Don't work the queue if in the update during load during interrupting
    // load. Failed that we may deadlock via dir_search().
    //
    if (ctx->update_during_load > 1)
      wq = false;
#endif

    phase_unlock u (phase ? ctx : nullptr, true /* delay */);
    ctx->sched->wait (start_count,
                      *task_count,
                      u,
                      (wq
                       ? scheduler::work_queue::work_all
                       : scheduler::work_queue::work_none));
    task_count = nullptr;
  }
}
