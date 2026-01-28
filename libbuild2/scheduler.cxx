// file      : libbuild2/scheduler.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/scheduler.hxx>

#include <errno.h> // errno, E*

#ifndef _WIN32
#  include <sys/ioctl.h> // ioctl(), FIONREAD
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#  include <pthread.h>
#  if defined(__FreeBSD__)
#    include <pthread_np.h> // pthread_attr_get_np() (in <pthread.h> on NetBSD)
#  elif defined(__OpenBSD__)
#    include <sys/signal.h>
#    include <pthread_np.h> // pthread_stackseg_np()
#  endif
#endif

#ifndef _WIN32
#  include <thread> // this_thread::sleep_for()
#else
#  include <libbutl/win32-utility.hxx>

#  include <chrono>
#endif

#include <libbutl/filesystem.hxx> // try_rmfile()

using namespace std;
using namespace butl;

namespace build2
{
  // TLS cache of thread's task queue.
  //
  // Note that scheduler::task_queue struct is private.
  //
  static
#ifdef __cpp_thread_local
  thread_local
#else
  __thread
#endif
  void* scheduler_queue = nullptr;

  scheduler::task_queue* scheduler::
  queue () noexcept
  {
    return static_cast<scheduler::task_queue*> (scheduler_queue);
  }

  void scheduler::
  queue (scheduler::task_queue* q) noexcept
  {
    scheduler_queue = q;
  }

  optional<size_t> scheduler::
  wait_impl (size_t start_count, const atomic_count& task_count, work_queue wq)
  {
    assert (max_active_ != 1); // Serial execution, nobody to wait for.

    // See if we can run some of our own tasks.
    //
    if (wq != work_none)
    {
      // If we are waiting on someone else's task count then there migh still
      // be no queue (set by async()).
      //
      if (task_queue* tq = queue ())
      {
        size_t tc;

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

    return nullopt;
  }

  void scheduler::
  deactivate_impl (bool external, lock&& rl)
  {
    // Note: assume non-serial execution.

    // Note: increment progress before/after every wait.
    //
    progress_.fetch_add (1, memory_order_relaxed);

    lock l (move (rl)); // Make sure unlocked on exception.

    active_--;
    waiting_++;
    if (external)
      external_++;
    progress_.fetch_add (1, memory_order_relaxed);

    if (waiting_ > stat_max_waiters_)
      stat_max_waiters_ = waiting_;

    // A spare active thread has become available. If there are ready masters
    // or eager helpers, wake someone up.
    //
    if (ready_ != 0)
    {
      ready_condv_.notify_one ();
    }
    else if (queued_task_count_.load (memory_order_consume) != 0 &&
             activate_helper (l))
      ;
    else if (jobserver_debt_)
    {
      // Pay off the jobserver debt.
      //
      active_++;
      jobserver_active_++;
      jobserver_debt_ = false;
      jobserver_condv_.notify_one ();
    }
    else if (active_ == 0 && external_ == 0)
    {
      // Note that we tried to handle this directly in this thread but that
      // wouldn't work for the phase lock case where we call deactivate and
      // then go wait on a condition variable: we would be doing deadlock
      // detection while holding the lock that prevents other threads from
      // making progress! So it has to be a separate monitoring thread.
      //
      deadlock_condv_.notify_one ();
    }
  }

  scheduler::lock scheduler::
  activate_impl (bool external, bool collision)
  {
    // Note: assume non-serial execution.

    // Note: increment progress before/after every wait.
    //
    progress_.fetch_add (1, memory_order_relaxed);

    lock l (mutex_);

    if (collision)
      stat_wait_collisions_++;

    // If we have spare active threads, then become active. Otherwise we enter
    // the ready queue.
    //
    if (external)
      external_--;
    waiting_--;
    ready_++;
    progress_.fetch_add (1, memory_order_relaxed);

    while (!shutdown_ && active_ >= max_active_)
      ready_condv_.wait (l);

    ready_--;
    active_++;
    progress_.fetch_add (1, memory_order_relaxed);

    if (shutdown_)
      throw_generic_error (ECANCELED);

    return l;
  }

  void scheduler::
  sleep (const duration& d)
  {
    deactivate (true /* external */);
    active_sleep (d);
    activate (true /* external */);
  }

  void scheduler::
  active_sleep (const duration& d)
  {
    // MinGW GCC 4.9 doesn't implement this_thread so use Win32 Sleep().
    //
#ifndef _WIN32
    this_thread::sleep_for (d);
#else
    using namespace chrono;
    Sleep (static_cast<DWORD> (duration_cast<milliseconds> (d).count ()));
#endif
  }

  size_t scheduler::
  allocate (size_t n)
  {
    if (max_active_ == 1) // Serial execution.
      return 0;

    lock l (mutex_);

    if (active_ < max_active_)
    {
      size_t d (max_active_ - active_);
      if (n == 0 || d < n)
        n = d;
      active_ += n;
      return n;
    }
    else
      return 0;
  }

  void scheduler::
  deallocate (size_t n)
  {
    if (max_active_ == 1) // Serial execution.
    {
      assert (n == 0);
      return;
    }

    if (n != 0)
    {
      lock l (mutex_);
      active_ -= n;

      if (ready_ != 0)
      {
        if (ready_ > 1 && n > 1)
          ready_condv_.notify_all ();
        else
          ready_condv_.notify_one ();

        n -=  (n > ready_ ? ready_ : n);
      }
      else
      {
        // @@ This is a bit iffy: we may keep starting helpers because none of
        //    them had a chance to pick any tasks yet. But probably better to
        //    start too many than not enough.
        //
        while (n != 0                                              &&
               queued_task_count_.load (memory_order_consume) != 0 &&
               activate_helper (l))
        {
          // Note that activate_helper() may or may not release the lock.
          //
          if (l.owns_lock ())
            n--;
          else
          {
            l.lock ();

            // Let's make sure we activate at most the original allocation.
            //
            size_t d (active_ < max_active_ ? max_active_ - active_ : 0);
            n = (d < n ? d : n - 1);
          }
        }
      }

      if (jobserver_debt_ && n != 0)
      {
        // Pay off the jobserver debt.
        //
        active_++;
        jobserver_active_++;
        jobserver_debt_ = false;
        jobserver_condv_.notify_one ();
      }

      // Note that active_ can never be 0 (since someone must be calling
      // deallocate()) so we don't need the deadlock logic here.
    }
  }

  size_t scheduler::
  suspend (size_t start_count, const atomic_count& task_count)
  {
    assert (max_active_ != 1); // Suspend during serial execution?

    wait_slot& s (
      wait_queue_[
        hash<const atomic_count*> () (&task_count) % wait_queue_size_]);

    // This thread is no longer active.
    //
    deactivate_impl (false /* external */, lock (mutex_));

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
    activate_impl (false /* external */, collision);

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

  auto scheduler::
  wait_idle () -> lock
  {
    lock l (mutex_);

    assert (waiting_ == 0);
    assert (ready_ == 0);

    while (active_ != init_active_ || starting_ != 0)
    {
      l.unlock ();
      this_thread::yield ();
      l.lock ();

      // If we have active threads allocated to the jobserver without any
      // other activity, then it means some jobserver client did not return
      // the tokens as it was supposed to.
      //
      size_t js_active (jobserver_active_ + (jobserver_debt_ ? 1 : 0));
      if (js_active != 0 && active_ == js_active + init_active_)
      {
        error << "jobserver client leaked " << js_active << " tokens";
        terminate (false /* trace */);
      }
    }

    return l;
  }

  size_t scheduler::
  shard_size (size_t mul, size_t div) const
  {
    size_t n (max_threads_ == 1 ? 0 : max_threads_ * mul / div / 4);

    // Return true if the specified number is prime.
    //
    auto prime = [] (size_t n)
    {
      // Check whether any number from 2 to the square root of n evenly
      // divides n, and return false if that's the case.
      //
      // While iterating i till sqrt(n) would be more efficient let's do
      // without floating arithmetic, since it doesn't make much difference
      // for the numbers we evaluate. Note that checking for i <= n / 2 is
      // just as efficient for small numbers but degrades much faster for
      // bigger numbers.
      //
      for (size_t i (2); i * i <= n; ++i)
      {
        if (n % i == 0)
          return false;
      }

      return n > 1;
    };

    // Return a prime number that is not less than the specified number.
    //
    auto next_prime = [&prime] (size_t n)
    {
      // Note that there is always a prime number in [n, 2 * n).
      //
      for (;; ++n)
      {
        if (prime (n))
          return n;
      }
    };

    // Experience shows that we want something close to 2x for small numbers,
    // then reduce to 1.5x in-between, and 1x for large ones.
    //
    // Note that Intel Xeons are all over the map when it comes to cores (6,
    // 8, 10, 12, 14, 16, 18, 20, 22).
    //
    // HW threads x arch-bits (see max_threads below).
    //
    return n ==  0 ? 1                      : // serial
           n ==  1 ? 3                      : // odd prime number
           n <= 16 ? next_prime (n * 2)     : // {2, 4} x 4, 2 x 8
           n <= 80 ? next_prime (n * 3 / 2) : // {4, 6, 8, 10} x 8
                     next_prime (n)         ; // {12, 14, 16, ...} x 8, ...
  }

  void scheduler::
  startup (size_t max_active,
           const path* jobserver,
           size_t init_active,
           size_t max_threads,
           size_t queue_depth,
           size_t orig_max_active)
  {
    if (orig_max_active == 0)
      orig_max_active = max_active;
    else
      assert (max_active <= orig_max_active);

    // Lock the mutex to make sure our changes are visible in (other) active
    // threads.
    //
    lock l (mutex_);

    // Use 8x max_active on 32-bit and 32x max_active on 64-bit. Unless we
    // were asked to run serially.
    //
    if (max_threads == 0)
      max_threads = (orig_max_active == 1
                     ? 1
                     : (sizeof (void*) < 8 ? 8 : 32) * orig_max_active);

    assert (shutdown_ &&
            init_active != 0 &&
            init_active <= max_active &&
            orig_max_active <= max_threads);

    active_ = init_active_ = init_active;
    max_active_ = max_active;
    orig_max_active_ = orig_max_active;
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
      : orig_max_active_ * 8;

    queued_task_count_.store (0, memory_order_relaxed);

    if ((wait_queue_size_ = max_threads == 1 ? 0 : shard_size ()) != 0)
      wait_queue_.reset (new wait_slot[wait_queue_size_]);

    // Reset other state.
    //
    phase_.clear ();

    idle_reserve_         = 0;

    stat_max_waiters_     = 0;
    stat_wait_collisions_ = 0;

    progress_.store (0, memory_order_relaxed);

    for (size_t i (0); i != wait_queue_size_; ++i)
      wait_queue_[i].shutdown = false;

    // Jobserver.
    //
    jobserver_ = jobserver;
    jobserver_active_ = 0;
    jobserver_debt_ = false;

    if (jobserver_ != nullptr)
    {
#ifndef _WIN32
      fdopen_fifo (*jobserver_);

      // Note that opening of a FIFO blocks until the other end is also
      // opened. The only portable exception to this rule is opening for
      // reading in the non-blocking mode (see fifo(7) for details). So the
      // order is important and we have to do this even during serial
      // execution not to hang the jobserver clients.
      //
      // We expect to never get blocked writing so open that end also as
      // non-blocking for good measure.
      //
      jobserver_ifd_ = fdopen (*jobserver_, (fdopen_mode::in |
                                             fdopen_mode::non_blocking));
      jobserver_ofd_ = fdopen (*jobserver_, (fdopen_mode::out |
                                             fdopen_mode::non_blocking));
#else
      assert (false);
#endif
    }

    shutdown_ = false;

    // Delay thread startup if serial.
    //
    if (max_active_ != 1)
    {
      if (jobserver_ != nullptr)
      {
        jobserver_ready_ = false;
        jobserver_thread_ = thread (jobserver_monitor, this);
      }

      deadlock_ready_ = false;
      deadlock_thread_ = thread (deadlock_monitor, this);

      // Wait for the threads to become ready.
      //
      do
      {
        l.unlock ();
        this_thread::yield ();
        l.lock ();
      }
      while (!(deadlock_ready_ && (jobserver_ == nullptr || jobserver_ready_)));
    }
  }

  size_t scheduler::
  tune (size_t max_active)
  {
    // Note that if we tune a parallel scheduler to run serially, we will
    // still have the deadlock monitoring thread idling around (it will not be
    // signalled). We will also have the jobserver thread, which we signal to
    // adjust its behavior.

    // With multiple initial active threads we will need to make changes to
    // max_active_ visible to other threads and which we currently say can be
    // accessed between startup and shutdown without a lock.
    //
    assert (init_active_ == 1);

    if (max_active == 0)
      max_active = orig_max_active_;

    if (max_active != max_active_)
    {
      assert (max_active >= init_active_ &&
              max_active <= orig_max_active_);

      // The scheduler must not be active though some threads might still be
      // comming off from finishing a task. So we busy-wait for them.
      //
      lock l (wait_idle ());

      swap (max_active_, max_active);

      // Start the jobserver and deadlock threads if their startup was
      // delayed.
      //
      if (max_active_ != 1)
      {
        if (jobserver_ != nullptr)
        {
          if (!jobserver_thread_.joinable ())
          {
            jobserver_ready_ = false;
            jobserver_thread_ = thread (jobserver_monitor, this);
          }
          else if (!jobserver_ready_) // Idling from previous serial execution.
            jobserver_condv_.notify_one ();
        }

        if (!deadlock_thread_.joinable ())
        {
          deadlock_ready_ = false;
          deadlock_thread_ = thread (deadlock_monitor, this);
        }
        else
          assert (deadlock_ready_);

        // Wait for the threads to become ready.
        //
        while (!(deadlock_ready_ && (jobserver_ == nullptr || jobserver_ready_)))
        {
          l.unlock ();
          this_thread::yield ();
          l.lock ();
        }
      }
      else // Serial execution.
      {
        if (jobserver_thread_.joinable () && jobserver_ready_) // Not idling.
        {
          jobserver_condv_.notify_one ();

          // Wait for the jobserver thread to become idle.
          //
          do
          {
            l.unlock ();
            this_thread::yield ();
            l.lock ();
          }
          while (jobserver_ready_);
        }
      }
    }

    return max_active == orig_max_active_ ? 0 : max_active;
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

      assert (external_ == 0);

      // Wait for the jobserver and deadlock threads (the only remaining
      // threads besides the initial).
      //
      l.unlock ();

      if (jobserver_thread_.joinable ()) jobserver_condv_.notify_one ();
      if (deadlock_thread_.joinable ())  deadlock_condv_.notify_one ();

      if (jobserver_thread_.joinable ()) jobserver_thread_.join ();
      if (deadlock_thread_.joinable ())  deadlock_thread_.join ();

      // Free the memory.
      //
      wait_queue_.reset ();
      task_queues_.clear ();

      if (jobserver_ != nullptr)
      {
#ifndef _WIN32
        jobserver_ofd_.close ();
        jobserver_ifd_.close ();
#else
        assert (false);
#endif
        try_rmfile (*jobserver_, true /* ignore_error */);
        jobserver_ = nullptr;
      }

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

  void scheduler::
  push_phase ()
  {
    if (max_active_ == 1) // Serial execution.
      return;

    // Note that we cannot "wait out" until all the old phase threads
    // deactivate themselves because we are called while holding the phase
    // transition lock which may prevent that from happening.
    //
    lock l (mutex_);

    // Here is the problem: the old phase is likely to have a bunch of waiting
    // threads with non-empty queues and after switching the phase new helpers
    // are going to start working those queues (and immediately get blocked
    // trying to lock the "old" phase). This is further exacerbated by the
    // fact that helpers get tasks from the front of the queue while new tasks
    // are added at the back. Which means helpers won't see any "new" phase
    // tasks until enough of them get "sacrificed" (i.e., blocked) to clear
    // the old phase backlog (or more like front-log in this case).
    //
    // Since none of the old phase tasks can make any progress until we return
    // to the old phase, we need to somehow "hide" their tasks from the new
    // phase helpers. The way we are going to do it is to temporarily (until
    // pop) replace such queues with empty ones. This should be ok since a
    // thread with such a "shadowed" queue won't wake up until we return to
    // the old phase (but the shadow queue may be used if the thread in
    // question is also switching to the new phase).
    //
    // Note also that the assumption here is that while we may still have
    // "phase-less" threads milling around (e.g., transitioning from active to
    // waiting), they do not access the queue (helpers are a special case that
    // we deal with by locking the queue).
    //
    phase_.emplace_back (task_queues_.size ());
    vector<task_queue_data>& ph (phase_.back ());

    auto j (ph.begin ());
    for (auto i (task_queues_.begin ()); i != task_queues_.end (); ++i, ++j)
    {
      task_queue& tq (*i);
      lock ql (tq.mutex);

      if (tq.size != 0)
      {
        // Note that task_queue::data will be allocated lazily (there is a
        // good chance this queue is not going to be used in the new phase).
        //
        queued_task_count_.fetch_sub (tq.size, memory_order_release);
        tq.swap (*j);
      }
    }

    assert (queued_task_count_.load (memory_order_consume) == 0);

    // Boost the max_threads limit for the first sub-phase.
    //
    // Ideally/long-term we want to redo this by waking up one of the old
    // phase waiting threads to serve as a helper.
    //
    if (phase_.size () == 1)
    {
      size_t cur_threads (init_active_ + helpers_ - idle_reserve_);

      old_eff_max_threads_ = (cur_threads > max_threads_
                              ? cur_threads
                              : max_threads_);
      old_max_threads_ = max_threads_;

      max_threads_ = old_eff_max_threads_ + max_threads_ / 2;
      idle_reserve_ = 0;
    }
  }

  void scheduler::
  pop_phase ()
  {
    if (max_active_ == 1) // Serial execution.
      return;

    lock l (mutex_);
    assert (!phase_.empty ());

    // Restore the queue sizes.
    //
    assert (queued_task_count_.load (memory_order_consume) == 0);

    vector<task_queue_data>& ph (phase_.back ());

    auto i (task_queues_.begin ());
    for (auto j (ph.begin ()); j != ph.end (); ++i, ++j)
    {
      if (j->size != 0)
      {
        task_queue& tq (*i);
        lock ql (tq.mutex);
        tq.swap (*j);
        queued_task_count_.fetch_add (tq.size, memory_order_release);
      }
    }

    phase_.pop_back ();

    // Restore the original limit and reserve idle helpers that we created
    // above the old (effective) limit.
    //
    if (phase_.size () == 0)
    {
      size_t cur_threads (init_active_ + helpers_);

      if (cur_threads > old_eff_max_threads_)
      {
        idle_reserve_ = cur_threads - old_eff_max_threads_;

        // Not necessarily the case since some helpers may have still picked
        // up tasks from the old phase and are now in waiting_.
        //
        //assert (idle_reserve_ <= idle_);
      }

      max_threads_ = old_max_threads_;
    }
  }

  scheduler::monitor_guard scheduler::
  monitor (atomic_count& c, size_t t, function<size_t (size_t, size_t)> f)
  {
    assert (monitor_count_ == nullptr && t != 0);

    // While the scheduler must not be active, some threads might still be
    // comming off from finishing a task and trying to report progress. So we
    // busy-wait for them (also in ~monitor_guard()).
    //
    lock l (wait_idle ());

    monitor_count_ = &c;
    monitor_tshold_.store (t, memory_order_relaxed);
    monitor_prev_ = c.load (memory_order_relaxed);
    monitor_func_ = move (f);

    return monitor_guard (this);
  }

  bool scheduler::
  activate_helper (lock& l)
  {
    if (shutdown_)
      return false;

    if (idle_ > idle_reserve_)
    {
      idle_condv_.notify_one ();
    }
    //
    // Ignore the max_threads value if we have queued tasks but no active
    // threads. This means everyone is waiting for something to happen but
    // nobody is doing anything (e.g., working the queues). This, for example,
    // can happen if a thread waits for a task that is in its queue but is
    // below the mark.
    //
    else if (init_active_ + helpers_ - idle_reserve_ < max_threads_ ||
             (active_ == 0 &&
              queued_task_count_.load (memory_order_consume) != 0))
    {
      create_helper (l);
    }
    else
      return false;

    return true;
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
    // Provided the main thread size is less-equal than
    // LIBBUILD2_SANE_STACK_SIZE (which defaults to
    // sizeof(void*)*LIBBUILD2_DEFAULT_STACK_SIZE), we make sure that the new
    // thread stack is the same as for the main thread. Otherwise, we cap it
    // at LIBBUILD2_DEFAULT_STACK_SIZE (default: 8MB). This can also be
    // overridden at runtime with the --max-stack build2 driver option
    // (remember to update its documentation of changing anything here).
    //
    // On Windows the stack size is the same for all threads and is customized
    // at the linking stage (see build2/buildfile). Thus neither *_STACK_SIZE
    // nor --max-stack have any effect here.
    //
    // On Linux, *BSD and MacOS there is no way to change it once and for
    // all newly created threads. Thus we will use pthreads, creating threads
    // with the stack size of the current thread. This way all threads will
    // inherit the main thread's stack size (since the first helper is always
    // created by the main thread).
    //
    // Note also the interaction with our backtrace functionality: in order to
    // get the complete stack trace we let unhandled exceptions escape the
    // thread function expecting the runtime to still call std::terminate. In
    // particular, having a noexcept function anywhere on the exception's path
    // causes the stack trace to be truncated, at least on Linux.
    //
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__APPLE__)

  // NOTE: don't forget to update async_impl() in libbutl if changing anything
  //       here.
  //
#ifndef LIBBUILD2_DEFAULT_STACK_SIZE
#  define LIBBUILD2_DEFAULT_STACK_SIZE 8388608 // 8MB
#endif

#ifndef LIBBUILD2_SANE_STACK_SIZE
#  define LIBBUILD2_SANE_STACK_SIZE (sizeof(void*) * LIBBUILD2_DEFAULT_STACK_SIZE)
#endif

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

#elif defined(__FreeBSD__) || defined(__NetBSD__)
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

#elif defined(__OpenBSD__)
      stack_t s;
      int r (pthread_stackseg_np (pthread_self (), &s));

      if (r != 0)
        throw_system_error (r);

      stack_size = s.ss_size;

#else // defined(__APPLE__)
      stack_size = pthread_get_stacksize_np (pthread_self ());
#endif
    }

    // Cap the size if necessary.
    //
    if (max_stack)
    {
      if (*max_stack != 0 && stack_size > *max_stack)
        stack_size = *max_stack;
    }
    else if (stack_size > LIBBUILD2_SANE_STACK_SIZE)
      stack_size = LIBBUILD2_DEFAULT_STACK_SIZE;

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

        // Note: see the push_phase() logic if changing anything here.
        //
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

        // While executing the tasks a thread might have become ready
        // (equivalent logic to deactivate()).
        //
        if (s.ready_ != 0)
        {
          s.ready_condv_.notify_one ();
        }
        else if (s.jobserver_debt_)
        {
          // Pay off the jobserver debt.
          //
          s.active_++;
          s.jobserver_active_++;
          s.jobserver_debt_ = false;
          s.jobserver_condv_.notify_one ();
        }
        else if (s.active_ == 0 && s.external_ == 0)
          s.deadlock_condv_.notify_one ();
      }

      // Become idle and wait for a notification.
      //
      s.idle_++;
      s.idle_condv_.wait (l);
      s.idle_--;
    }

    s.helpers_--;
    return nullptr;
  }

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

    queue (tq);
    return *tq;
  }

  void* scheduler::
  deadlock_monitor (void* d)
  {
    using namespace chrono;

    scheduler& s (*static_cast<scheduler*> (d));

    lock l (s.mutex_);
    s.deadlock_ready_ = true;

    while (!s.shutdown_)
    {
      s.deadlock_condv_.wait (l);

      while (s.active_ == 0 && s.external_ == 0 && !s.shutdown_)
      {
        // We may have a deadlock which can happen because of dependency
        // cycles.
        //
        // Relying on the active_ count alone is not precise enough, however:
        // some threads might be transitioning between active/waiting/ready
        // states. Carefully accounting for this is not trivial, to say the
        // least (especially in the face of spurious wakeups). So we are going
        // to do a "fuzzy" deadlock detection by measuring "progress". The
        // idea is that those transitions should be pretty short-lived and so
        // if we wait for a few thousand context switches, then we should be
        // able to distinguish a real deadlock from the transition case.
        //
        size_t op (s.progress_.load (memory_order_relaxed)), np (op);

        l.unlock ();
        for (size_t i (0), n (10000), m (9990); op == np && i != n; ++i)
        {
          // On the last few iterations sleep a bit instead of yielding (in
          // case yield() is a noop; we use the consume order for the same
          // reason).
          //
          if (i <= m)
            this_thread::yield ();
          else
            active_sleep ((i - m) * 20ms);

          np = s.progress_.load (memory_order_consume);
        }
        l.lock ();

        // Re-check active/external counts for good measure (in case we were
        // spinning too fast).
        //
        if (np == op &&
            s.active_ == 0 && s.external_ == 0 && !s.shutdown_ &&
            s.progress_.load (memory_order_consume) == op)
        {
          // Shutting things down cleanly is tricky: we could have handled it
          // in the scheduler (e.g., by setting a flag and then waking
          // everyone up, similar to shutdown). But there could also be
          // "external waiters" that have called deactivate() -- we have no
          // way to wake those up. So for now we are going to abort (the nice
          // thing about abort is if this is not a dependency cycle, then we
          // have a core to examine).
          //
          error << "deadlock suspected, aborting" <<
            info << "deadlocks are normally caused by dependency cycles" <<
            info << "re-run with -s to diagnose dependency cycles";

          terminate (false /* trace */);
        }
      }
    }

    return nullptr;
  }

  void* scheduler::
  jobserver_monitor (void* d)
  {
    using namespace chrono;

    tracer trace ("scheduler::jobserver_monitor");

    scheduler& s (*static_cast<scheduler*> (d));

    assert (s.jobserver_ != nullptr);

#ifndef _WIN32
    try
    {
      // The jobserver documentation in the GNU make manual suggests that at
      // the start the jobserver pipe is pre-loaded with all the available
      // tokens and everyone then uses it as the single source of truth about
      // the available concurrency.
      //
      // This approach, however, doesn't sit well with what we are doing here
      // for multiple reasons. Firstly, we don't want to replace our well
      // established (and well debugged) scheduler that is based on
      // lightweight and reliable mutexes and condition variables with a
      // heavy-handled pipe-based cross-process semaphore, which, as admitted
      // by the jobserver documentation itself, is quite brittle (clients may
      // not return tokens for various reasons, etc). Plus, we would like to
      // be able to give priority to internal jobs (the feeling is that this
      // should result in better performance for our typical use-cases, such
      // as LTO).
      //
      // So instead of pre-loading the pipe with all the available tokens, we
      // are going to feed it one token at a time, depending on demand.
      // Specifically, we will "advance" one token into the pipe without
      // counting it against our active thread count. If we detect that it was
      // consumed by a jobserver client, then we will attempt to "allocate" it
      // to the active count. If we could (active < max_active), then we
      // advance another token. If we could not, then we mark the jobserver as
      // being "in debt" and until this debt is paid off (allocated), we don't
      // advance any further tokens. On the other hand, if we notice that a
      // token was returned by a jobserver client back to the pipe, we
      // "deallocate" it and, unless it is the only remaining token (the
      // advance) "withdraw" it from the pipe. To put it another way, unless
      // we are in debt, we aim to always keep a single advance token in the
      // pipe.
      //
      // To implement this we need to detect when the size of the buffered
      // data in the pipe goes below or above 1 byte. Unfortunately there are
      // no mechanisms that would notify us about such high/low watermark
      // events and our only option is to periodically poll, which we combine
      // with some heuristics about when to poll more or less frequently.
      // Testing on real workloads (GCC LTO) showed that this implementation
      // provides a reasonably responsive jobserver without imposing a
      // noticeable overhead when the jobserver is not used.
      //
      // As mentioned above, the jobserver protocol is quite brittle with
      // buggy clients potentially not returning the tokens (for example, when
      // terminated abnormally). While it would be great if we could detect
      // and handle this gracefully, that's not easy and would increase the
      // complexity quite a bit. So for now our strategy is to assume that
      // clients are well behaved and try to limp along with reduced
      // concurrency if they are not. There are, however, "chokepoints" (like
      // wait_idle(), for example), where we wait for the active_ count to
      // drop to 1 and which will never happen if the jobserver leaked some
      // tokens. So at such points we terminate if we detect a jobserver leak.
      // So the refined strategy is "limp along but don't hang".
      //
      size_t  js_issued (0);                   // Tokens issued to the pipe.
      size_t  js_remaining;                    // Tokens remaining in the pipe.
      size_t& js_active (s.jobserver_active_); // Tokens allocated.
      bool&   js_debt   (s.jobserver_debt_);   // One token in debt.

      auto write = [&s, &js_issued] ()
      {
        uint8_t t (0x01);
        if (fdwrite (s.jobserver_ofd_.get (), &t, 1) == -1)
          throw_generic_ios_failure (errno);

        js_issued++;
      };

      auto read = [&s, &js_issued] () -> bool
      {
        uint8_t t;
        if (fdread (s.jobserver_ifd_.get (), &t, 1) == -1)
        {
          if (errno == EAGAIN)
            return false;

          throw_generic_ios_failure (errno);
        }

        js_issued--;
        return true;
      };

      auto show = [&s] () -> size_t
      {
        int n;
        if (ioctl (s.jobserver_ifd_.get (), FIONREAD, &n) == -1)
          throw_generic_ios_failure (errno);
        return static_cast<size_t> (n);
      };

#if 1
      auto trace_state = [&trace,
                          &js_issued,
                          &js_active,
                          &js_remaining,
                          &js_debt] (const char* when)
      {
        if (verb >= 6)
        {
          trace << when
                << ": issued " << js_issued
                << ", active " << js_active
                << ", remain " << js_remaining
                << (js_debt ? ", in debt" : "");
        }
      };
#else
      auto trace_state = [] (const char*) {};
#endif

      lock l (s.mutex_);
      while (!s.shutdown_) // Outer ready/idle loop.
      {
        if (s.max_active_ == 1)
        {
          // Idle until signalled by tune() or shutdown().
          //
          s.jobserver_ready_ = false;
          s.jobserver_condv_.wait (l);
          continue;
        }

        s.jobserver_ready_ = true;

        // Write the initial "advance" token.
        //
        l.unlock ();
        write ();
        l.lock ();

        js_remaining = 1; // Presumably (for tracing).
        trace_state ("startup");

        const duration max_delay (4ms);
        const duration min_delay (200us); // Note: also a step.

        // GCC prior to version 12 and Clang prior to version 12 do not handle
        // wait_for() correctly in their TSAN implementations. See GCC bug
        // #101978 for background and further references.
        //
        // Note: the Clang check must come first since it also defines
        // __GNUC__.
        //
#if defined(__SANITIZE_THREAD__)                && \
  ((defined(__clang__) && __clang_major__ < 12) || \
   (defined(__GNUC__) && __GNUC__ < 12))
        auto wait_for = [&l] (duration d)
        {
          l.unlock ();
          active_sleep (d);
          l.lock ();
        };
#else
        auto wait_for = [&s, &l] (duration d)
        {
          s.jobserver_condv_.wait_for (l, d);
        };
#endif

        // Inner jobserver "session" loop.
        //
        for (duration delay (max_delay);
             !(s.shutdown_ || s.max_active_ == 1);
             wait_for (delay))
        {
          if (delay < max_delay)
            delay += min_delay;

          // Let's not keep the scheduler state locked while we work with the
          // pipe. We just need to be careful not to carry decisions across
          // the unlock/lock boundary.
          //
        requery:
          l.unlock ();
          js_remaining = show ();
          l.lock ();
          if (s.shutdown_ || s.max_active_ == 1) break;

        reeval:
          // There should always be an active thread that waits on the process
          // which uses the jobserver. And we should only be advancing at most
          // one token.
          //
          assert (js_active == 0 || js_active < s.active_);
          assert (js_active == js_issued || js_active + 1 == js_issued);

          if (js_remaining == 0)
          {
            // All the issued tokens are consumed.

            if (js_active < js_issued) // In debt.
            {
              // Allocate if we can, indicate we are in debt otherwise.
              //
              // Here we reasonably assume that if (active < max_active), then
              // there are no ready or helper threads (if that were not the
              // case, then we wouldn't have observed active going below
              // max_active; see deactivate_impl() for background).
              //
              if (s.active_ < s.max_active_)
              {
                s.active_++;
                js_active++;
                js_debt = false;
              }
              else
              {
                if (js_debt)
                  continue; // Skip repeating trace below if already in debt.

                js_debt = true;
              }

              trace_state ("allocate");

              // Do not advance any more tokens until the debt is paid off or
              // cancelled. Also reset the delay to max.
              //
              if (js_debt)
              {
                delay = max_delay;
                continue;
              }
            }
            else
              js_debt = false;

            // Write another advance token.
            //
            // Note that we do this even if we are at max_active (which means
            // we will not be able to allocate it) since the other parts of
            // the scheduler machinery only deal with the "in debt" situation.
            // In other words, if we don't advance it now, we won't get
            // notified to do it later.
            //
            l.unlock ();
            write ();
            l.lock ();
            if (s.shutdown_ || s.max_active_ == 1) break;

            js_remaining = 1; // Presumably (for tracing).
            trace_state ("advance");

            // Requery the pipe without delay in case this token was
            // immediately consumed. Also reduce the subsequent delays.
            //
            delay = min_delay;
            goto requery;
          }
          else
          {
            // One or more issued tokens remain unconsumed in (or got returned
            // to) the pipe.

            if (js_debt)
            {
              js_debt = false;

              // Trace this even if there won't be deallocate or withdraw trace.
              // failed that, we may observe puzzling state transitions (like
              // two in debt allocations in a row).
              //
              if (js_remaining == 1)
                trace_state ("cancel");
            }

            if (js_active == js_issued) // In credit.
            {
              // Deallocate one token, turning it to advanced.
              //
              js_active--;
              s.active_--;

              trace_state ("deallocate");

              // Similar logic to deactivate_impl().
              //
              if (s.ready_ != 0)
                s.ready_condv_.notify_one ();
              else if (s.queued_task_count_.load (memory_order_consume) != 0 &&
                       s.activate_helper (l))
              {
                // Note that activate_helper() may or may not release the lock.
                //
                if (!l.owns_lock ())
                {
                  l.lock ();
                  if (s.shutdown_ || s.max_active_ == 1) break;
                }
              }
              else if (s.active_ == 0 && s.external_ == 0)
                s.deadlock_condv_.notify_one ();
            }

            // Remove excess tokens from the pipe. We do it one at a time for
            // simplicity.
            //
            if (js_remaining > 1)
            {
              l.unlock ();
              bool r (read ());
              l.lock ();
              if (s.shutdown_ || s.max_active_ == 1) break;

              if (r)
                js_remaining--; // Presumably (for tracing).
              else
                js_remaining = 0; // For reeval.

              trace_state ("withdraw");

              if (r)
              {
                // Requery the pipe without delay in case we need to
                // deallocate or remove another token.
                //
                goto requery;
              }
              else
              {
                // Add back the advance token without re-querying the pipe
                // since we know all the tokens were consumed.
                //
                goto reeval;
              }
            }
          }
        }

        // Note: scheduler mutex is locked.

        // Note that we cannot assume anything about the jobserver state since
        // on abnormal termination clients may leak tokens (see above for
        // background).
        //
        trace_state (s.shutdown_ ? "shutdown" : "suspend");

        if (!s.shutdown_)
        {
          assert (s.max_active_ == 1);

          // This is re-tuning to serial execution and we can only end up here
          // if (js_active + js_debt) is 0 (since otherwise, wait_idle()
          // wouldn't have returned). Which means we should have a single
          // advance token in the pipe (it should be there since no jobserver
          // client should be active during re-tuning). And so we just need to
          // remove it.
          //
          if (js_active != 0 || js_debt || js_issued != 1 || !read ())
            throw_generic_error (
              EINVAL, "jobserver in unexpected state after shutdown");
        }
      }
    }
    catch (const system_error& e)
    {
      error << "jobserver system error: " << e;
      terminate (false /* trace */);
    }
#else
    assert (false);
#endif // _WIN32

    return nullptr;
  }
}
