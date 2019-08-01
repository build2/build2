// file      : libbuild2/version/init.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_VERSION_INIT_HXX
#define LIBBUILD2_VERSION_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/version/export.hxx>

namespace build2
{
  namespace version
  {
    // Module `version` requires bootstrapping.
    //
    // No submodules.
    //
    extern "C" LIBBUILD2_VERSION_SYMEXPORT const module_functions*
    build2_version_load ();
  }
}

#endif // LIBBUILD2_VERSION_INIT_HXX
