// file      : build2/scheduler.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scheduler>

using namespace std;

namespace build2
{
  void scheduler::
  wait (atomic_count& task_count)
  {
    if (task_count == 0)
      return;

    // See if we can run some of our own tasks.
    //
    task_queue& tq (*task_queue_); // Must have been initializied by async().

    for (lock ql (tq.mutex); !tq.shutdown && !empty_back (tq); )
    {
      // Save the old stop point and set the new one in case the task we are
      // about to run adds sub-tasks.
      //
      size_t stop (tq.stop);
      tq.stop = tq.tail - 1; // Index of the first sub-task to be added (-1
                             // is for the following pop_back()).

      pop_back (tq, ql); // Releases the lock.
      ql.lock ();

      // Restore the old stop point which we might have to adjust.
      //
      tq.stop = tq.head > stop ? tq.head : tq.tail < stop ? tq.tail : stop;
    }

    // Note that empty task queue doesn't automatically mean the task count is
    // zero (some might still be executing asynchronously).
    //
    if (task_count == 0)
      return;

    suspend (task_count);
  }

  void scheduler::
  suspend (atomic_count& tc)
  {
    wait_slot& s (
      wait_queue_[std::hash<atomic_count*> () (&tc) % wait_queue_size_]);

    // This thread is no longer active.
    //
    {
      lock l (mutex_);
      active_--;
      waiting_++;

      if (waiting_ > stat_max_waiters_)
        stat_max_waiters_ = waiting_;

      // A spare active thread has become available. If there are ready
      // masters or eager helpers, wake someone up.
      //
      if (ready_ != 0)
        ready_condv_.notify_one ();
      else if (task_)
        activate_helper (l);
    }

    // Note that the task count is checked while holding the lock. We also
    // have to notify while holding the lock (see resume()). The aim here
    // is not to end up with a notification that happens between the check
    // and the wait.
    //
    bool collision;
    {
      lock l (s.mutex);
      collision = (s.waiters++ != 0);

      // Since we use a mutex for synchronization, we can relax the atomic
      // access.
      //
      while (!s.shutdown && tc.load (std::memory_order_relaxed) != 0)
        s.condv.wait (l);

      s.waiters--;
    }

    // This thread is no longer waiting.
    //
    {
      lock l (mutex_);
      waiting_--;

      if (collision)
        stat_wait_collisions_++;

      // If we have spare active threads, then become active. Otherwise it
      // enters the ready queue.
      //
      ready_++;

      while (!shutdown_ && active_ >= max_active_)
        ready_condv_.wait (l);

      ready_--;

      if (shutdown_)
        throw system_error (ECANCELED, system_category ());

      active_++;
    }
  }

  void scheduler::
  resume (atomic_count& tc)
  {
    wait_slot& s (
      wait_queue_[std::hash<atomic_count*> () (&tc) % wait_queue_size_]);

    // See suspend() for why we must hold the lock.
    //
    lock l (s.mutex);

    if (s.waiters != 0)
      s.condv.notify_all ();
  }

  scheduler::
  ~scheduler ()
  {
    try { shutdown (); } catch (system_error&) {}
  }

  void scheduler::
  startup (size_t max_active, size_t init_active, size_t max_threads)
  {
    // Use 4x max_active on 32-bit and 8x max_active on 64-bit. Unless we were
    // asked to run serially.
    //
    if (max_threads == 0)
      max_threads = max_active * (max_active == 1 ? 1 : sizeof (void*));

    assert (shutdown_ &&
            init_active != 0 &&
            init_active <= max_active &&
            max_active <= max_threads);

    active_ = init_active_ = init_active;
    max_active_ = max_active;
    max_threads_ = max_threads;

    // This value should be proportional to the amount of hardware concurrency
    // we have. Note that the queue entry is quite sizable.
    //
    task_queue_depth_ = max_active * sizeof (void*) * 4;

    task_queues_.clear ();
    task_queues_.reserve (max_threads_);

    // Pick a nice prime for common max_threads numbers. Though Intel Xeons
    // are all over the map when it comes to cores (6, 8, 10, 12, 14, 16,
    // 18, 20, 22).
    //
    wait_queue_size_ =            // HW threads x bits
      max_threads ==   8 ?   17 : // 2 x 4
      max_threads ==  16 ?   31 : // 4 x 4, 2 x 8
      max_threads ==  32 ?   63 : // 4 x 8
      max_threads ==  48 ?   97 : // 6 x 8
      max_threads ==  64 ?  127 : // 8 x 8
      max_threads ==  96 ?  191 : // 12 x 8
      max_threads == 128 ?  257 : // 16 x 8
      max_threads == 192 ?  383 : // 24 x 8
      max_threads == 256 ?  509 : // 32 x 8
      max_threads == 384 ?  769 : // 48 x 8
      max_threads == 512 ? 1021 : // 64 x 8
      2 * max_threads - 1;

    wait_queue_.reset (new wait_slot[wait_queue_size_]);

    // Reset stats counters.
    //
    stat_max_waiters_     = 0;
    stat_wait_collisions_ = 0;

    task_ = false;
    shutdown_ = false;

    for (size_t i (0); i != wait_queue_size_; ++i)
      wait_queue_[i].shutdown = false;
  }

  auto scheduler::
  shutdown () -> stat
  {
    // Our overall approach to shutdown is not to try and stop everything as
    // quickly as possible but rather to avoid performing any tasks. This
    // avoids having code littered with if(shutdown) on every second line.

    stat r;
    lock l (mutex_);

    if (!shutdown_)
    {
      // Signal shutdown and collect statistics.
      //
      shutdown_ = true;

      for (size_t i (0); i != wait_queue_size_; ++i)
      {
        wait_slot& ws (wait_queue_[i]);
        lock l (ws.mutex);
        ws.shutdown = true;
      }

      for (unique_ptr<task_queue>& tq: task_queues_)
      {
        lock l (tq->mutex);
        r.task_queue_full += tq->stat_full;
        tq->shutdown = true;
      }

      // Wait for all the helpers to terminate waking up any thread that
      // sleeps.
      //
      r.thread_helpers = helpers_;

      while (helpers_ != 0)
      {
        bool i (idle_ != 0);
        bool r (ready_ != 0);
        bool w (waiting_ != 0);

        l.unlock ();

        if (i)
          idle_condv_.notify_all ();

        if (r)
          ready_condv_.notify_all ();

        if (w)
          for (size_t i (0); i != wait_queue_size_; ++i)
            wait_queue_[i].condv.notify_all ();

        this_thread::yield ();
        l.lock ();
      }

      r.thread_max_active     = max_active_;
      r.thread_max_total      = max_threads_;
      r.thread_max_waiting    = stat_max_waiters_;

      r.task_queue_depth      = task_queue_depth_;

      r.wait_queue_slots      = wait_queue_size_;
      r.wait_queue_collisions = stat_wait_collisions_;
    }

    return r;
  }

  void scheduler::
  activate_helper (lock& l)
  {
    if (!shutdown_)
    {
      if (idle_ != 0)
        idle_condv_.notify_one ();
      else if (init_active_ + helpers_ < max_threads_)
        create_helper (l);
    }
  }

  void scheduler::
  create_helper (lock& l)
  {
    helpers_++;
    starting_++;
    l.unlock ();

    // Restore the counter if the thread creation fails.
    //
    struct guard
    {
      lock* l;
      size_t& h;
      size_t& s;

      ~guard () {if (l != nullptr) {l->lock (); h--; s--;}}

    } g {&l, helpers_, starting_};

    thread t (helper, this);
    g.l = nullptr; // Disarm.

    t.detach ();
  }

  void scheduler::
  helper (void* d)
  {
    scheduler& s (*static_cast<scheduler*> (d));

    // Note that this thread can be in an in-between state (not active or
    // idle) but only while holding the lock. Which means that if we have the
    // lock then we can account for all of them (this is important during
    // shutdown). Except when the thread is just starting, before acquiring
    // the lock for the first time, which we handle with the starting count.
    //
    lock l (s.mutex_);
    s.starting_--;

    while (!s.shutdown_)
    {
      // If there is a spare active thread, become active and go looking for
      // some work.
      //
      if (s.active_ < s.max_active_)
      {
        s.active_++;

        while (s.task_) // There might be a task.
        {
          s.task_ = false; // We will process all that are currently there.

          // Queues are never removed and there shouldn't be any reallocations
          // since we reserve maximum possible size upfront. Which means we
          // can get the current number of queues and release the main lock
          // while examining each of them.
          //
          size_t n (s.task_queues_.size ());
          l.unlock ();

          for (size_t i (0); i != n; ++i)
          {
            task_queue& tq (*s.task_queues_[i]);

            for (lock ql (tq.mutex);
                 !tq.shutdown && !s.empty_front (tq);
                 ql.lock ())
              s.pop_front (tq, ql); // Releases the lock.
          }

          l.lock ();
          // If task_ became true, then there might be new tasks.
        }

        s.active_--;

        // While executing the tasks a thread might have become ready.
        //
        if (s.ready_ != 0)
          s.ready_condv_.notify_one ();
      }

      // Become idle and wait for a notification (note that task_ is false
      // here).
      //
      s.idle_++;
      s.idle_condv_.wait (l);
      s.idle_--;
    }

    s.helpers_--;
  }

  thread_local scheduler::task_queue* scheduler::task_queue_ = nullptr;

  auto scheduler::
  create_queue () -> task_queue&
  {
    lock l (mutex_);
    task_queues_.push_back (make_unique<task_queue> (task_queue_depth_));
    task_queue_ = task_queues_.back ().get ();
    task_queue_->shutdown = shutdown_;
    return *task_queue_;
  }

  scheduler sched;
}
