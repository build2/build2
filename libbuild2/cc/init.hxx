// file      : libbuild2/cc/init.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_INIT_HXX
#define LIBBUILD2_CC_INIT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  namespace cc
  {
    // Module `cc` does not require bootstrapping.
    //
    // Submodules:
    //
    // `cc.core.vars`   -- registers some variables.
    // `cc.core.guess`  -- loads cc.core.vars and sets some variables.
    // `cc.core.config` -- loads cc.core.guess and sets more variables.
    // `cc.core`        -- loads cc.core.config and registers target types and
    //                     rules.
    // `cc.config`      -- loads {c,cxx}.config.
    // `cc`             -- loads c and cxx.
    //
    extern "C" LIBBUILD2_CC_SYMEXPORT const module_functions*
    build2_cc_load ();
  }
}

#endif // LIBBUILD2_CC_INIT_HXX
