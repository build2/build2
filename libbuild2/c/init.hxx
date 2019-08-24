// file      : libbuild2/c/init.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
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
    // `c.guess`  -- registers and sets some variables.
    // `c.config` -- loads c.guess and sets more variables.
    // `c`        -- loads c.config and registers target types and rules.
    //
    extern "C" LIBBUILD2_C_SYMEXPORT const module_functions*
    build2_c_load ();
  }
}

#endif // LIBBUILD2_C_INIT_HXX
