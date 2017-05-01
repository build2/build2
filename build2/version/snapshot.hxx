// file      : build2/version/snapshot.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_VERSION_SNAPSHOT_HXX
#define BUILD2_VERSION_SNAPSHOT_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/scope.hxx>

namespace build2
{
  namespace version
  {
    struct snapshot
    {
      uint64_t sn = 0;
      string id;

      bool
      empty () const {return sn == 0;}
    };

    // Return empty snapshot if unknown scm or uncommitted.
    //
    snapshot
    extract_snapshot (const scope& rs);
  }
}

#endif // BUILD2_VERSION_SNAPSHOT_HXX
