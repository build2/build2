// file      : libbuild2/in/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_IN_INIT_HXX
#define LIBBUILD2_IN_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/in/export.hxx>

namespace build2
{
  namespace in
  {
    // Module `in` does not require bootstrapping.
    //
    // Submodules:
    //
    // `in.base` -- registers variables and target types.
    // `in`      -- loads in.base and registers rules.
    //
    extern "C" LIBBUILD2_IN_SYMEXPORT const module_functions*
    build2_in_load ();
  }
}

#endif // LIBBUILD2_IN_INIT_HXX
