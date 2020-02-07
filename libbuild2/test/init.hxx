// file      : libbuild2/test/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_INIT_HXX
#define LIBBUILD2_TEST_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  namespace test
  {
    // Module `test` requires bootstrapping.
    //
    // `test` -- registers the test and update-for-test operations, registers/
    //           sets variables, and registers target types and rules.
    //
    extern "C" LIBBUILD2_SYMEXPORT const module_functions*
    build2_test_load ();
  }
}

#endif // LIBBUILD2_TEST_INIT_HXX
