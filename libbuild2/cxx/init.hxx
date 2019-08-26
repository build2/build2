// file      : libbuild2/cxx/init.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
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
    // `cxx.guess`  -- registers and sets some variables.
    // `cxx.config` -- loads cxx.guess and sets more variables.
    // `cxx`        -- loads cxx.config and registers target types and rules.
    //
    extern "C" LIBBUILD2_CXX_SYMEXPORT const module_functions*
    build2_cxx_load ();
  }
}

#endif // LIBBUILD2_CXX_INIT_HXX
