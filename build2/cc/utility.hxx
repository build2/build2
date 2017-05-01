// file      : build2/cc/utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_UTILITY_HXX
#define BUILD2_CC_UTILITY_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/target.hxx>
#include <build2/bin/target.hxx>

#include <build2/cc/types.hxx>

namespace build2
{
  struct variable;

  namespace cc
  {
    // Compile/link output type.
    //
    otype
    compile_type (const target&);

    otype
    link_type (const target&);

    // Library link order.
    //
    // The reason we pass scope and not the target is because this function is
    // called not only for exe/lib but also for obj as part of the library
    // meta-information protocol implementation. Normally the bin.*.lib values
    // will be project-wide. With this scheme they can be customized on the
    // per-directory basis but not per-target which means all exe/lib in the
    // same directory have to have the same link order.
    //
    lorder
    link_order (const scope& base, otype);

    // Given the link order return the library member (liba or libs) to link.
    //
    const target&
    link_member (const bin::lib&, action, lorder);
  }
}

#include <build2/cc/utility.ixx>

#endif // BUILD2_CC_UTILITY_HXX
