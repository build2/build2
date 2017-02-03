// file      : build2/scheduler.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cerrno>

namespace build2
{
  template <typename F, typename... A>
  void scheduler::
  async (size_t start_count, atomic_count& task_count, F&& f, A&&... a)
  {
    using task = task_type<F, A...>;

    static_assert (sizeof (task) <= sizeof (task_data::data),
                   "insufficient space");

    static_assert (std::is_trivially_destructible<task>::value,
                   "not trivially destructible");

    // Push into the queue unless we are running serially or the queue is
    // full.
    //
    task_data* td (nullptr);

    if (max_active_ != 1)
    {
      task_queue* tq (task_queue_); // Single load.
      if (tq == nullptr)
        tq = &create_queue ();

      lock ql (tq->mutex);

      if (tq->shutdown)
        throw system_error (ECANCELED, std::system_category ());

      if ((td = push (*tq)) != nullptr)
      {
        // Package the task.
        //
        new (&td->data) task {
          &task_count,
          start_count,
          decay_copy (forward<F> (f)),
          typename task::args_type (decay_copy (forward<A> (a))...)};

        td->thunk = &task_thunk<F, A...>;
      }
      else
        tq->stat_full++;
    }

    // If serial/full, then run the task synchronously. In this case there is
    // no need to mess with task count.
    //
    if (td == nullptr)
    {
      forward<F> (f) (forward<A> (a)...);
      return;
    }

    // Increment the task count.
    //
    task_count.fetch_add (1, std::memory_order_release);

    lock l (mutex_);
    task_ = true;

    // If there is a spare active thread, wake up (or create) the helper.
    //
    if (active_ < max_active_)
      activate_helper (l);
  }

  template <typename F, typename... A>
  void scheduler::
  task_thunk (scheduler& s, lock& ql, void* td)
  {
    using task = task_type<F, A...>;

    // Move the data and release the lock.
    //
    task t (move (*static_cast<task*> (td)));
    ql.unlock ();

    t.thunk (std::index_sequence_for<A...> ());

    atomic_count& tc (*t.task_count);
    if (--tc <= t.start_count)
      s.resume (tc); // Resume a waiter, if any.
  }
}
