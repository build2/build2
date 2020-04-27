// file      : build2/cli/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CLI_INIT_HXX
#define BUILD2_CLI_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

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
    extern "C" const module_functions*
    build2_cli_load ();
  }
}

#endif // BUILD2_CLI_INIT_HXX
