// file      : libbuild2/cli/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CLI_INIT_HXX
#define LIBBUILD2_CLI_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/cli/export.hxx>

namespace build2
{
  namespace cli
  {
    // Module `cli` does not require bootstrapping.
    //
    // Submodules:
    //
    // `cli.guess`  -- set variables describing the compiler.
    // `cli.config` -- load `cli.guess` and set the rest of the variables.
    // `cli`        -- load `cli.config` and register targets and rules.
    //
    extern "C" LIBBUILD2_CLI_SYMEXPORT const module_functions*
    build2_cli_load ();
  }
}

#endif // LIBBUILD2_CLI_INIT_HXX
