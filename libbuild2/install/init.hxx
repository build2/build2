// file      : libbuild2/install/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_INSTALL_INIT_HXX
#define LIBBUILD2_INSTALL_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  namespace install
  {
    // Module `install` requires bootstrapping.
    //
    // `install` -- registers the install, uninstall, and update-for-install
    //              operations, registers/sets variables, and registers
    //              functions and rules.
    //
    extern "C" LIBBUILD2_SYMEXPORT const module_functions*
    build2_install_load ();
  }
}

#endif // LIBBUILD2_INSTALL_INIT_HXX
