// file      : build2/cli/init.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
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
    // `cli.config` -- registers variables.
    // `cli`        -- loads cli.config and registers target types and rules.
    //
    extern "C" const module_functions*
    build2_cli_load ();
  }
}

#endif // BUILD2_CLI_INIT_HXX
