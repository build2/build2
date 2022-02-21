// file      : tests/libbuild2/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/scheduler.hxx>
#include <libbuild2/file-cache.hxx>

#include <libbuild2/dist/init.hxx>
#include <libbuild2/test/init.hxx>
#include <libbuild2/config/init.hxx>
#include <libbuild2/install/init.hxx>

#include <libbuild2/in/init.hxx>
#include <libbuild2/bin/init.hxx>
#include <libbuild2/c/init.hxx>
#include <libbuild2/cc/init.hxx>
#include <libbuild2/cxx/init.hxx>
#include <libbuild2/version/init.hxx>

#undef NDEBUG
#include <cassert>

using namespace build2;

int
main (int, char* argv[])
{
  // Fake build system driver, default verbosity.
  //
  init_diag (1);
  init (nullptr, argv[0], true);

  load_builtin_module (&config::build2_config_load);
  load_builtin_module (&dist::build2_dist_load);
  load_builtin_module (&test::build2_test_load);
  load_builtin_module (&install::build2_install_load);

  load_builtin_module (&bin::build2_bin_load);
  load_builtin_module (&cc::build2_cc_load);
  load_builtin_module (&c::build2_c_load);
  load_builtin_module (&cxx::build2_cxx_load);
  load_builtin_module (&version::build2_version_load);
  load_builtin_module (&in::build2_in_load);

  // Serial execution.
  //
  scheduler sched (1);
  global_mutexes mutexes (1);
  file_cache fcache (true);
  context ctx (sched, mutexes, fcache);

  return 0;
}
