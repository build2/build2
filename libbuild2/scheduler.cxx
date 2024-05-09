// file      : libbuild2/scheduler.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/scheduler.hxx>

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

#include <cerrno>

#include <libbuild2/diagnostics.hxx>

using namespace std;

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
    else if (active_ == 0 && external_ == 0)
    {
      // Note that we tried to handle this directly in this thread but that
      // wouldn't work for the phase lock case where we call deactivate and
      // then go wait on a condition variable: we would be doing deadlock
      // detection while holding the lock that prevents other threads from
      // making progress! So it has to be a separate monitoring thread.
      //
      dead_condv_.notify_one ();
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

    lock l (mutex_);
    active_ -= n;
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
           size_t init_active,
           size_t max_threads,
           size_t queue_depth,
           optional<size_t> max_stack,
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

    max_stack_ = max_stack;

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

    shutdown_ = false;

    // Delay thread startup if serial.
    //
    if (max_active_ != 1)
      dead_thread_ = thread (deadlock_monitor, this);
  }

  size_t scheduler::
  tune (size_t max_active)
  {
    // Note that if we tune a parallel scheduler to run serially, we will
    // still have the deadlock monitoring thread loitering around.

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

      // Start the deadlock thread if its startup was delayed.
      //
      if (max_active_ != 1 && !dead_thread_.joinable ())
        dead_thread_ = thread (deadlock_monitor, this);
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

      // Wait for the deadlock monitor (the only remaining thread).
      //
      if (dead_thread_.joinable ())
      {
        l.unlock ();
        dead_condv_.notify_one ();
        dead_thread_.join ();
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
  monitor (atomic_count& c, size_t t, function<size_t (size_t)> f)
  {
    assert (monitor_count_ == nullptr && t != 0);

    // While the scheduler must not be active, some threads might still be
    // comming off from finishing a task and trying to report progress. So we
    // busy-wait for them (also in ~monitor_guard()).
    //
    lock l (wait_idle ());

    monitor_count_ = &c;
    monitor_tshold_.store (t, memory_order_relaxed);
    monitor_init_ = c.load (memory_order_relaxed);
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
    if (max_stack_)
    {
      if (*max_stack_ != 0 && stack_size > *max_stack_)
        stack_size = *max_stack_;
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
          s.ready_condv_.notify_one ();
        else if (s.active_ == 0 && s.external_ == 0)
          s.dead_condv_.notify_one ();
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
    while (!s.shutdown_)
    {
      s.dead_condv_.wait (l);

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
}
