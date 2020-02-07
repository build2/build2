// file      : libbuild2/config/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CONFIG_INIT_HXX
#define LIBBUILD2_CONFIG_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  namespace config
  {
    // Module `config` requires bootstrapping.
    //
    // `config` -- registers the configure and disfigure meta-operations,
    //             registers variables, and sources the config.build file.
    //
    extern "C" LIBBUILD2_SYMEXPORT const module_functions*
    build2_config_load ();
  }
}

#endif // LIBBUILD2_CONFIG_INIT_HXX
