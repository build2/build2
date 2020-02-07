// file      : libbuild2/bash/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BASH_INIT_HXX
#define LIBBUILD2_BASH_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/bash/export.hxx>

namespace build2
{
  namespace bash
  {
    // Module `bash` does not require bootstrapping.
    //
    // `bash` -- registers variables, target types, and rules.
    //
    extern "C" LIBBUILD2_BASH_SYMEXPORT const module_functions*
    build2_bash_load ();
  }
}

#endif // LIBBUILD2_BASH_INIT_HXX
