// file      : libbuild2/c/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_C_INIT_HXX
#define LIBBUILD2_C_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/c/export.hxx>

namespace build2
{
  namespace c
  {
    // Module `c` does not require bootstrapping.
    //
    // Submodules:
    //
    // `c.types`         -- registers target types.
    // `c.guess`         -- registers and sets some variables.
    // `c.config`        -- loads c.guess and sets more variables.
    // `c`               -- loads c.{types,config} and registers rules and
    //                      functions.
    //
    // `c.objc.types`    -- registers m{} target type.
    // `c.objc`          -- loads c.objc.types and enables Objective-C
    //                      compilation. Must be loaded after c.
    //
    // `c.as-cpp.types`  -- registers S{} target type.
    // `c.as-cpp`        -- loads c.as-cpp.types and enables Assembler with C
    //                      preprocessor compilation. Must be loaded after c.
    //
    // `c.predefs`       -- registers rule for generating a C header with
    //                      predefined compiler macros. Must be loaded after c.
    //
    extern "C" LIBBUILD2_C_SYMEXPORT const module_functions*
    build2_c_load ();
  }
}

#endif // LIBBUILD2_C_INIT_HXX
