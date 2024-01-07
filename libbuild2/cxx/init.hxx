// file      : libbuild2/cxx/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CXX_INIT_HXX
#define LIBBUILD2_CXX_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/cxx/export.hxx>

namespace build2
{
  namespace cxx
  {
    // Module `cxx` does not require bootstrapping.
    //
    // Submodules:
    //
    // `cxx.types`         -- registers target types.
    // `cxx.guess`         -- registers and sets some variables.
    // `cxx.config`        -- loads cxx.guess and sets more variables.
    // `cxx`               -- loads cxx.{types,config} and registers rules
    //                        and functions.
    //
    // `cxx.objcxx.types`  -- registers mm{} target type.
    // `cxx.objcxx`        -- loads cxx.objcxx and enables Objective-C++
    //                        compilation.
    //
    // `cxx.predefs`       -- registers rule for generating a C++ header with
    //                        predefined compiler macros. Must be loaded after
    //                        cxx.
    //
    extern "C" LIBBUILD2_CXX_SYMEXPORT const module_functions*
    build2_cxx_load ();
  }
}

#endif // LIBBUILD2_CXX_INIT_HXX
