// file      : build2/scheduler.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/scheduler.hxx>

#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
#  include <pthread.h>
#  ifdef __FreeBSD__
#    include <pthread_np.h> // pthread_attr_get_np()
#  endif
#endif

#include <cerrno>

using namespace std;

namespace build2
{
  size_t scheduler::
  wait (size_t start_count, const atomic_count& task_count, work_queue wq)
  {
    // Note that task_count is a synchronization point.
    //
    size_t tc;

    if ((tc = task_count.load (memory_order_acquire)) <= start_count)
      return tc;

    assert (max_active_ != 1); // Serial execution, nobody to wait for.

    // See if we can run some of our own tasks.
    //
    if (wq != work_none)
    {
      // If we are waiting on someone else's task count then there migh still
      // be no queue (set by async()).
      //
      if (task_queue* tq = task_queue_)
      {
        for (lock ql (tq->mutex); !tq->shutdown && !empty_back (*tq); )
        {
          pop_back (*tq, ql);

          if (wq == work_one)
          {
            if ((tc = task_count.load (memory_order_acquire)) <= start_count)
              return tc;
          }
        }

        // Note that empty task queue doesn't automatically mean the task
        // count has been decremented (some might still be executing
        // asynchronously).
        //
        if ((tc = task_count.load (memory_order_acquire)) <= start_count)
          return tc;
      }
    }

    return suspend (start_count, task_count);
  }

  void scheduler::
  deactivate ()
  {
    if (max_active_ == 1) // Serial execution.
      return;

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
    else if (queued_task_count_.load (std::memory_order_consume) != 0)
      activate_helper (l);
  }

  void scheduler::
  activate (bool collision)
  {
    if (max_active_ == 1) // Serial execution.
      return;

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
      throw_generic_error (ECANCELED);

    active_++;
  }

  size_t scheduler::
  suspend (size_t start_count, const atomic_count& task_count)
  {
    wait_slot& s (
      wait_queue_[
        hash<const atomic_count*> () (&task_count) % wait_queue_size_]);

    // This thread is no longer active.
    //
    deactivate ();

    // Note that the task count is checked while holding the lock. We also
    // have to notify while holding the lock (see resume()). The aim here
    // is not to end up with a notification that happens between the check
    // and the wait.
    //
    size_t tc (0);
    bool collision;
    {
      lock l (s.mutex);

      // We have a collision if there is already a waiter for a different
      // task count.
      //
      collision = (s.waiters++ != 0 && s.task_count != &task_count);

      // This is nuanced: we want to always have the task count of the last
      // thread to join the queue. Otherwise, if threads are leaving and
      // joining the queue simultaneously, we may end up with a task count of
      // a thread group that is no longer waiting.
      //
      s.task_count = &task_count;

      // We could probably relax the atomic access since we use a mutex for
      // synchronization though this has a different tradeoff (calling wait
      // because we don't see the count).
      //
      while (!(s.shutdown ||
               (tc = task_count.load (memory_order_acquire)) <= start_count))
        s.condv.wait (l);

      s.waiters--;
    }

    // This thread is no longer waiting.
    //
    activate (collision);

    return tc;
  }

  void scheduler::
  resume (const atomic_count& tc)
  {
    if (max_active_ == 1) // Serial execution, nobody to wakeup.
      return;

    wait_slot& s (
      wait_queue_[hash<const atomic_count*> () (&tc) % wait_queue_size_]);

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

  size_t scheduler::
  shard_size (size_t mul, size_t div) const
  {
    size_t n (max_threads_ == 1 ? 0 : max_threads_ * mul / div / 4);

    // Experience shows that we want something close to 2x for small numbers,
    // then reduce to 1.5x in-between, and 1x for large ones.
    //
    // Note that Intel Xeons are all over the map when it comes to cores (6,
    // 8, 10, 12, 14, 16, 18, 20, 22).
    //
    return              // HW threads x arch-bits (see max_threads below)
      n ==   0 ?    1 : // serial
      //
      // 2x
      //
      n ==   1 ?    3 :
      n ==   2 ?    5 :
      n ==   4 ?   11 :
      n ==   6 ?   13 :
      n ==   8 ?   17 : // 2 x 4
      n ==  16 ?   31 : // 4 x 4, 2 x 8
      //
      // 1.5x
      //
      n ==  32 ?   47 : // 4 x 8
      n ==  48 ?   53 : // 6 x 8
      n ==  64 ?   67 : // 8 x 8
      n ==  80 ?   89 : // 10 x 8
      //
      // 1x
      //
      n ==  96 ?  101 : // 12 x 8
      n == 112 ?  127 : // 14 x 8
      n == 128 ?  131 : // 16 x 8
      n == 144 ?  139 : // 18 x 8
      n == 160 ?  157 : // 20 x 8
      n == 176 ?  173 : // 22 x 8
      n == 192 ?  191 : // 24 x 8
      n == 224 ?  223 : // 28 x 8
      n == 256 ?  251 : // 32 x 8
      n == 288 ?  271 : // 36 x 8
      n == 320 ?  313 : // 40 x 8
      n == 352 ?  331 : // 44 x 8
      n == 384 ?  367 : // 48 x 8
      n == 512 ?  499 : // 64 x 8
      n - 1;            // Assume it is even.
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

    // Use 8x max_active on 32-bit and 32x max_active on 64-bit. Unless we
    // were asked to run serially.
    //
    if (max_threads == 0)
      max_threads = (max_active == 1    ? 1 :
                     sizeof (void*) < 8 ? 8 : 32) * max_active;

    assert (shutdown_ &&
            init_active != 0 &&
            init_active <= max_active &&
            max_active <= max_threads);

    active_ = init_active_ = init_active;
    max_active_ = orig_max_active_ = max_active;
    max_threads_ = max_threads;

    // This value should be proportional to the amount of hardware concurrency
    // we have (no use queing things up if helpers cannot keep up). Note that
    // the queue entry is quite sizable.
    //
    // The relationship is as follows: we want to have a deeper queue if the
    // tasks take long (e.g., compilation) and shorter if they are quick (e.g,
    // test execution). If the tasks are quick then the synchronization
    // overhead required for queuing/dequeuing things starts to dominate.
    //
    task_queue_depth_ = queue_depth != 0
      ? queue_depth
      : max_active * 4;

    queued_task_count_.store (0, memory_order_relaxed);

    if ((wait_queue_size_ = max_threads == 1 ? 0 : shard_size ()) != 0)
      wait_queue_.reset (new wait_slot[wait_queue_size_]);

    // Reset stats counters.
    //
    stat_max_waiters_     = 0;
    stat_wait_collisions_ = 0;

    for (size_t i (0); i != wait_queue_size_; ++i)
      wait_queue_[i].shutdown = false;

    shutdown_ = false;
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
      // Collect statistics.
      //
      r.thread_helpers = helpers_;

      // Signal shutdown.
      //
      shutdown_ = true;

      for (size_t i (0); i != wait_queue_size_; ++i)
      {
        wait_slot& ws (wait_queue_[i]);
        lock l (ws.mutex);
        ws.shutdown = true;
      }

      for (task_queue& tq: task_queues_)
      {
        lock ql (tq.mutex);
        r.task_queue_full += tq.stat_full;
        tq.shutdown = true;
      }

      // Wait for all the helpers to terminate waking up any thread that
      // sleeps.
      //
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
      r.task_queue_remain     = queued_task_count_.load (memory_order_consume);

      r.wait_queue_slots      = wait_queue_size_;
      r.wait_queue_collisions = stat_wait_collisions_;
    }

    return r;
  }

  scheduler::monitor_guard scheduler::
  monitor (atomic_count& c, size_t t, function<size_t (size_t)> f)
  {
    assert (monitor_count_ == nullptr && t != 0);
    monitor_count_ = &c;
    monitor_tshold_.store (t, memory_order_relaxed);
    monitor_init_ = c.load (memory_order_relaxed);
    monitor_func_ = move (f);
    return monitor_guard (this);
  }

  void scheduler::
  activate_helper (lock& l)
  {
    if (!shutdown_)
    {
      if (idle_ != 0)
        idle_condv_.notify_one ();
      else if (init_active_ + helpers_ < max_threads_ ||
               //
               // Ignore the max_threads value if we have queued tasks but no
               // active threads. This means everyone is waiting for something
               // to happen but nobody is doing anything (e.g., work the
               // queues). This, for example, can happen if a thread waits for
               // a task that is in its queue but is below the mark.
               //
               (active_ == 0 &&
                queued_task_count_.load (memory_order_consume) != 0))
      {
        create_helper (l);
      }
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

    // For some platforms/compilers the default stack size for newly created
    // threads may differ from that of the main thread. Here are the default
    // main/new thread sizes (in KB) for some of them:
    //
    // Linux   :   8192 / 8196
    // FreeBSD : 524288 / 2048
    // MacOS   :   8192 /  512
    // MinGW   :   2048 / 2048
    // VC      :   1024 / 1024
    //
    // We will make sure that the new thread stack size is the same as for the
    // main thread. For FreeBSD we will also cap it at 8MB.
    //
    // On Windows the stack size is the same for all threads and is customized
    // at the linking stage (see build2/buildfile).
    //
    // On Linux, FreeBSD and MacOS there is no way to change it once and for
    // all newly created threads. Thus we will use pthreads, creating threads
    // with the stack size of the current thread. This way all threads will
    // inherit the main thread's stack size (since the first helper is always
    // created by the main thread).
    //
#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
    // Auto-deleter.
    //
    struct attr_deleter
    {
      void
      operator() (pthread_attr_t* a) const
      {
        int r (pthread_attr_destroy (a));

        // We should be able to destroy the valid attributes object, unless
        // something is severely damaged.
        //
        assert (r == 0);
      }
    };

    // Calculate the current thread stack size. Don't forget to update #if
    // conditions above when adding the stack size customization for a new
    // platforms/compilers.
    //
    size_t stack_size;
    {
#ifdef __linux__
      // Note that the attributes must not be initialized.
      //
      pthread_attr_t attr;
      int r (pthread_getattr_np (pthread_self (), &attr));

      if (r != 0)
        throw_system_error (r);

      unique_ptr<pthread_attr_t, attr_deleter> ad (&attr);
      r = pthread_attr_getstacksize (&attr, &stack_size);

      if (r != 0)
        throw_system_error (r);

#elif defined(__FreeBSD__)
      pthread_attr_t attr;
      int r (pthread_attr_init (&attr));

      if (r != 0)
        throw_system_error (r);

      unique_ptr<pthread_attr_t, attr_deleter> ad (&attr);
      r = pthread_attr_get_np (pthread_self (), &attr);

      if (r != 0)
        throw_system_error (r);

      r = pthread_attr_getstacksize (&attr, &stack_size);

      if (r != 0)
        throw_system_error (r);

      // Cap at 8MB.
      //
      if (stack_size > 8388608)
        stack_size = 8388608;

#else // defined(__APPLE__)
      stack_size = pthread_get_stacksize_np (pthread_self ());
#endif
    }

    pthread_attr_t attr;
    int r (pthread_attr_init (&attr));

    if (r != 0)
      throw_system_error (r);

    unique_ptr<pthread_attr_t, attr_deleter> ad (&attr);

    // Create the thread already detached.
    //
    r = pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);

    if (r != 0)
      throw_system_error (r);

    r = pthread_attr_setstacksize (&attr, stack_size);

    if (r != 0)
      throw_system_error (r);

    pthread_t t;
    r = pthread_create (&t, &attr, helper, this);

    if (r != 0)
      throw_system_error (r);
#else
    thread t (helper, this);
    t.detach ();
#endif

    g.l = nullptr; // Disarm.
  }

  void* scheduler::
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

        while (s.queued_task_count_.load (memory_order_consume) != 0)
        {
          // Queues are never removed which means we can get the current range
          // and release the main lock while examining each of them.
          //
          auto it (s.task_queues_.begin ());
          size_t n (s.task_queues_.size ()); // Different to end().
          l.unlock ();

          // Note: we have to be careful not to advance the iterator past the
          // last element (since what's past could be changing).
          //
          for (size_t i (0);; ++it)
          {
            task_queue& tq (*it);

            for (lock ql (tq.mutex); !tq.shutdown && !s.empty_front (tq); )
              s.pop_front (tq, ql);

            if (++i == n)
              break;
          }

          l.lock ();
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
    return nullptr;
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
    task_queue* tq;
    {
      lock l (mutex_);
      task_queues_.emplace_back (task_queue_depth_);
      tq = &task_queues_.back ();
      tq->shutdown = shutdown_;
    }

    task_queue_ = tq;
    return *tq;
  }
}
