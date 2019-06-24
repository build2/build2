// file      : tests/libbuild2/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/context.hxx>   // sched, reset()
#include <libbuild2/scheduler.hxx>

using namespace build2;

int
main (int, char* argv[])
{
  // Fake build system driver, default verbosity.
  //
  init_diag (1);
  init (argv[0]);
  sched.startup (1);  // Serial execution.
  reset (strings ()); // No command line variables.

  return 0;
}
