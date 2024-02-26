// file      : libbuild2/scheduler.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline size_t scheduler::
  wait (size_t start_count, const atomic_count& task_count, work_queue wq)
  {
    // Note that task_count is a synchronization point.
    //
    size_t tc;
    if ((tc = task_count.load (memory_order_acquire)) <= start_count)
      return tc;

    if (optional<size_t> r = wait_impl (start_count, task_count, wq))
      return *r;

    return suspend (start_count, task_count);
  }

  inline size_t scheduler::
  wait (const atomic_count& task_count, work_queue wq)
  {
    return wait (0, task_count, wq);
  }

  template <typename L>
  inline size_t scheduler::
  wait (size_t start_count,
        const atomic_count& task_count,
        L& lock,
        work_queue wq)
  {
    // Note that task_count is a synchronization point.
    //
    size_t tc;
    if ((tc = task_count.load (memory_order_acquire)) <= start_count)
      return tc;

    if (optional<size_t> r = wait_impl (start_count, task_count, wq))
      return *r;

    lock.unlock ();
    return suspend (start_count, task_count);
  }

  inline void scheduler::
  deactivate (bool external)
  {
    if (max_active_ != 1) // Serial execution.
      deactivate_impl (external, lock (mutex_));
  }

  inline void scheduler::
  activate (bool external)
  {
    if (max_active_ != 1) // Serial execution.
      activate_impl (external, false /* collision */);
  }

  inline scheduler::queue_mark::
  queue_mark (scheduler& s)
      : tq_ (s.queue ())
  {
    if (tq_ != nullptr)
    {
      lock ql (tq_->mutex);

      if (tq_->mark != s.task_queue_depth_)
      {
        om_ = tq_->mark;
        tq_->mark = s.task_queue_depth_;
      }
      else
        tq_ = nullptr;
    }
  }

  inline scheduler::queue_mark::
  ~queue_mark ()
  {
    if (tq_ != nullptr)
    {
      lock ql (tq_->mutex);
      tq_->mark = tq_->size == 0 ? tq_->tail : om_;
    }
  }
}
