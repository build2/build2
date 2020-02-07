// file      : libbuild2/dist/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DIST_INIT_HXX
#define LIBBUILD2_DIST_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  namespace dist
  {
    // Module `dist` requires bootstrapping.
    //
    // `dist` -- registers the dist meta-operation, registers/sets variables,
    //           and registers rules.
    //
    extern "C" LIBBUILD2_SYMEXPORT const module_functions*
    build2_dist_load ();
  }
}

#endif // LIBBUILD2_DIST_INIT_HXX
