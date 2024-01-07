// file      : libbuild2/bin/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BIN_INIT_HXX
#define LIBBUILD2_BIN_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/bin/export.hxx>

namespace build2
{
  namespace bin
  {
    // Module `bin` does not require bootstrapping.
    //
    // Submodules:
    //
    // `bin.vars`      -- registers some variables.
    // `bin.types`     -- registers target types.
    // `bin.config`    -- loads bin.vars and sets some variables.
    // `bin`           -- loads bin.{types,config} and registers rules and
    //                    functions.
    //
    // `bin.ar.config` -- loads bin.config and registers/sets more variables.
    // `bin.ar`        -- loads bin and bin.ar.config.
    //
    // `bin.ld.config` -- loads bin.config and registers/sets more variables.
    // `bin.ld`        -- loads bin and bin.ld.config and registers more
    //                    target types for msvc.
    //
    // `bin.rc.config` -- loads bin.config and registers/sets more variables.
    // `bin.rc`        -- loads bin and bin.rc.config.
    //
    // `bin.nm.config` -- loads bin.config and registers/sets more variables.
    // `bin.nm`        -- loads bin and bin.nm.config.
    //
    // `bin.def`       -- loads bin, bin.nm.config unless using MSVC link.exe,
    //                    and registers the .def file generation rule.
    //
    extern "C" LIBBUILD2_BIN_SYMEXPORT const module_functions*
    build2_bin_load ();
  }
}

#endif // LIBBUILD2_BIN_INIT_HXX
