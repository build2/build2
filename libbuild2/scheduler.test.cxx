// file      : libbuild2/scheduler.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <chrono>

#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scheduler.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;

namespace build2
{
  // Usage argv[0] [-v <volume>] [-d <difficulty>] [-c <concurrency>]
  //               [-q <queue-depth>]
  //
  // -v  task tree volume (affects both depth and width), for example 100
  // -d  computational difficulty of each task, for example 10
  // -c  max active threads, if unspecified or 0, then hardware concurrency
  // -q  task queue depth, if unspecified or 0, then appropriate default used
  //
  // Specifying any option also turns on the verbose mode.
  //
  // Notes on testing:
  //
  // 1. Ideally you would want to test things on an SMP machine.
  //
  // 2. When need to compare performance, disable turbo boost since its
  //    availability depends on CPU utilization/temperature:
  //
  //    # echo '1' >/sys/devices/system/cpu/intel_pstate/no_turbo
  //
  // 3. Use turbostat(1) to see per-CPU details (utlization, frequency):
  //
  //    $ sudo turbostat --interval 1 ./driver -d 8 -v 300
  //
  static bool
  prime (uint64_t);

  // Find # of primes in the [x, y) range.
  //
  static void
  inner (uint64_t x, uint64_t y, uint64_t& r)
  {
    for (; x != y; ++x)
      if (prime (x))
        r++;
  };

  int
  main (int argc, char* argv[])
  {
    bool verb (false);

    // Adjust assert() below if changing these defaults.
    //
    size_t volume (100);
    uint32_t difficulty (10);

    size_t max_active (0);
    size_t queue_depth (0);

    for (int i (1); i != argc; ++i)
    {
      string a (argv[i]);

      if (a == "-v")
        volume = stoul (argv[++i]);
      else if (a == "-d")
        difficulty = stoul (argv[++i]);
      else if (a == "-c")
        max_active = stoul (argv[++i]);
      else if (a == "-q")
        queue_depth = stoul (argv[++i]);
      else
        assert (false);

      verb = true;
    }

    if (max_active == 0)
      max_active = scheduler::hardware_concurrency ();

    scheduler s (max_active, 1, 0, queue_depth);

    // Find # prime counts of primes in [i, d*i*i) ranges for i in (0, n].
    //
    auto outer = [difficulty, &s] (size_t n, vector<uint64_t>& o, uint64_t& r)
    {
      scheduler::atomic_count task_count (0);

      for (size_t i (1); i <= n; ++i)
      {
        o[i - 1] = 0;
        s.async (task_count,
                 inner,
                 i,
                 i * i * difficulty,
                 ref (o[i - 1]));
      }

      s.wait (task_count);
      assert (task_count == 0);

      for (uint64_t v: o)
        r += prime (v) ? 1 : 0;
    };

    vector<uint64_t> r (volume, 0);
    vector<vector<uint64_t>> o (volume, vector<uint64_t> ());

    scheduler::atomic_count task_count (0);

    for (size_t i (0); i != volume; ++i)
    {
      o[i].resize (i);
      s.async (task_count,
               outer,
               i,
               ref (o[i]),
               ref (r[i]));
    }

    s.wait (task_count);
    assert (task_count == 0);

    uint64_t n (0);
    for (uint64_t v: r)
      n += v;

    if (volume == 100 && difficulty == 10)
      assert (n == 580);

    scheduler::stat st (s.shutdown ());

    if (verb)
    {
      cerr << "result                 " << n                       << endl
           << endl;

      cerr << "thread_max_active      " << st.thread_max_active     << endl
           << "thread_max_total       " << st.thread_max_total      << endl
           << "thread_helpers         " << st.thread_helpers        << endl
           << "thread_max_waiting     " << st.thread_max_waiting    << endl
           << endl
           << "task_queue_depth       " << st.task_queue_depth      << endl
           << "task_queue_full        " << st.task_queue_full       << endl
           << endl
           << "wait_queue_slots       " << st.wait_queue_slots      << endl
           << "wait_queue_collisions  " << st.wait_queue_collisions << endl;
    }

    return 0;
  }

  static bool
  prime (uint64_t x)
  {
    if (x == 2 || x == 3)
      return true;

    if (x < 2 || x % 2 == 0 || x % 3 == 0)
      return false;

    // Test divisors starting from 5 and incrementing alternatively by 2/4.
    //
    for (uint64_t d (5), i (2); d * d <= x; d += i, i = 6 - i)
    {
      if (x % d == 0)
        return false;
    }

    return true;
  }
}

int
main (int argc, char* argv[])
{
  return build2::main (argc, argv);
}
