// file      : build2/scheduler.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scheduler>

#include <cerrno>

using namespace std;

namespace build2
{
  void scheduler::
  wait (size_t start_count, atomic_count& task_count)
  {
    if (task_count <= start_count)
      return;

    assert (max_active_ != 1); // Serial execution, nobody to wait for.

    // See if we can run some of our own tasks.
    //
    // If we are waiting on someone else's task count then there migh still
    // be no queue which is set by async().
    //
    if (task_queue* tq = task_queue_)
    {
      for (lock ql (tq->mutex); !tq->shutdown && !empty_back (*tq); )
        pop_back (*tq, ql);

      // Note that empty task queue doesn't automatically mean the task count
      // has been decremented (some might still be executing asynchronously).
      //
      if (task_count <= start_count)
        return;
    }

    suspend (start_count, task_count);
  }

  void scheduler::
  suspend (size_t start_count, atomic_count& tc)
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

      // We have a collision if there is already a waiter for a different
      // task count.
      //
      collision = (s.waiters++ != 0 && s.tcount != &tc);

      // This is nuanced: we want to always have the task count of the last
      // thread to join the queue. Otherwise, if threads are leaving and
      // joining the queue simultaneously, we may end up with a task count of
      // a thread group that is no longer waiting.
      //
      s.tcount = &tc;

      // Since we use a mutex for synchronization, we can relax the atomic
      // access.
      //
      while (!(s.shutdown ||
               tc.load (std::memory_order_relaxed) <= start_count))
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
    if (max_active_ == 1) // Serial execution, nobody to wakeup.
      return;

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
  startup (size_t max_active,
           size_t init_active,
           size_t max_threads,
           size_t queue_depth)
  {
    // Lock the mutex to make sure our changes are visible in (other) active
    // threads.
    //
    lock l (mutex_);

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
    max_active_ = orig_max_active_ = max_active;
    max_threads_ = max_threads;

    // This value should be proportional to the amount of hardware concurrency
    // we have (no use queing things if helpers cannot keep up). Note that the
    // queue entry is quite sizable.
    //
    task_queue_depth_ = queue_depth != 0
      ? queue_depth
      : max_active * sizeof (void*) * 2;

    task_queues_.reserve (max_threads_);

    // Pick a nice prime for common max_threads numbers. Experience shows that
    // we want something close to 2x for small numbers, then reduce to 1.5x
    // in-between, and 1x for large ones.
    //
    // Note that Intel Xeons are all over the map when it comes to cores (6,
    // 8, 10, 12, 14, 16, 18, 20, 22).
    //
    wait_queue_size_ =            // HW threads x bits
      //
      // 2x
      //
      max_threads ==   8 ?   17 : // 2 x 4
      max_threads ==  16 ?   31 : // 4 x 4, 2 x 8
      //
      // 1.5x
      //
      max_threads ==  32 ?   47 : // 4 x 8
      max_threads ==  48 ?   53 : // 6 x 8
      max_threads ==  64 ?   67 : // 8 x 8
      max_threads ==  80 ?   89 : // 10 x 8
      //
      // 1x
      //
      max_threads ==  96 ?  101 : // 12 x 8
      max_threads == 112 ?  127 : // 14 x 8
      max_threads == 128 ?  131 : // 16 x 8
      max_threads == 144 ?  139 : // 18 x 8
      max_threads == 160 ?  157 : // 20 x 8
      max_threads == 176 ?  173 : // 22 x 8
      max_threads == 192 ?  191 : // 24 x 8
      max_threads == 224 ?  223 : // 28 x 8
      max_threads == 256 ?  251 : // 32 x 8
      max_threads == 288 ?  271 : // 36 x 8
      max_threads == 320 ?  313 : // 40 x 8
      max_threads == 352 ?  331 : // 44 x 8
      max_threads == 384 ?  367 : // 48 x 8
      max_threads == 512 ?  499 : // 64 x 8
      max_threads - 1;            // Assume max_threads is even.

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

  void scheduler::
  tune (size_t max_active)
  {
    lock l (mutex_);

    if (max_active == 0)
      max_active = orig_max_active_;

    assert (max_active >= init_active_ &&
            max_active <= orig_max_active_);

    // The scheduler must not be active though some threads might still be
    // comming off from finishing a task. So we busy-wait for them.
    //
    while (active_ != init_active_)
    {
      l.unlock ();
      this_thread::yield ();
      l.lock ();
    }

    assert (waiting_ == 0);
    assert (ready_ == 0);

    max_active_ = max_active;
  }

  auto scheduler::
  shutdown () -> stat
  {
    // Our overall approach to shutdown is not to try and stop everything as
    // quickly as possible but rather to avoid performing any tasks. This
    // avoids having code littered with if(shutdown) on every other line.

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

      // Free the memory.
      //
      wait_queue_.reset ();
      task_queues_.clear ();

      r.thread_max_active     = orig_max_active_;
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

    // Restore the counters if the thread creation fails.
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

            for (lock ql (tq.mutex); !tq.shutdown && !s.empty_front (tq); )
              s.pop_front (tq, ql);
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

#ifdef __cpp_thread_local
    thread_local
#else
    __thread
#endif
  scheduler::task_queue* scheduler::task_queue_ = nullptr;

  auto scheduler::
  create_queue () -> task_queue&
  {
    // Note that task_queue_depth is immutable between startup() and
    // shutdown() (but see join()).
    //
    unique_ptr<task_queue> tqp (new task_queue (task_queue_depth_));
    task_queue& tq (*tqp);

    {
      lock l (mutex_);
      tq.shutdown = shutdown_;
      task_queues_.push_back (move (tqp));
    }

    task_queue_ = &tq;
    return tq;
  }

  scheduler sched;
}
