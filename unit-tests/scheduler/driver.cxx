// file      : unit-tests/scheduler/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <chrono>
#include <thread>

#include <cassert>
#include <iostream>

#include <build2/types>
#include <build2/utility>

#include <build2/scheduler>

using namespace std;

namespace build2
{
  // Usage argv[0] <max-active-threads>
  //
  int
  main (int argc, char* argv[])
  {
    bool verb (false);
    size_t max_active (0);

    if (argc > 1)
    {
      verb = true;
      max_active = stoul (argv[1]);
    }

    if (max_active == 0)
      max_active = scheduler::hardware_concurrency ();

    scheduler s (max_active);

    auto inner = [] (size_t x, size_t y, size_t& out)
    {
      out = x + y;
      this_thread::sleep_for (chrono::microseconds (out * 10));
    };

    auto outer = [&s, &inner] (size_t n, size_t& out)
    {
      vector<size_t> result (2 * n, 0);
      scheduler::atomic_count task_count (0);

      for (size_t i (0); i != 2 * n; ++i)
      {
        s.async (task_count,
                 inner,
                 i,
                 i,
                 std::ref (result[i]));
      }

      s.wait (task_count);
      assert (task_count == 0);

      for (size_t i (0); i != n; ++i)
        out += result[i];

      this_thread::sleep_for (chrono::microseconds (out * 10));
    };

    const size_t tasks (50);

    vector<size_t> result (tasks, 0);
    scheduler::atomic_count task_count (0);

    for (size_t i (0); i != tasks; ++i)
    {
      s.async (task_count,
               outer,
               i,
               std::ref (result[i]));
    }

    s.wait (task_count);
    assert (task_count == 0);

    scheduler::stat st (s.shutdown ());

    if (verb)
    {
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
}

int
main (int argc, char* argv[])
{
  return build2::main (argc, argv);
}
