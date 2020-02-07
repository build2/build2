// file      : libbuild2/version/snapshot.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_VERSION_SNAPSHOT_HXX
#define LIBBUILD2_VERSION_SNAPSHOT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>

namespace build2
{
  namespace version
  {
    struct snapshot
    {
      uint64_t sn = 0;
      string id;
      bool committed = false;

      bool
      empty () const {return sn == 0;}
    };

    // Return empty snapshot if unknown scm or uncommitted.
    //
    snapshot
    extract_snapshot (const scope& rs);
  }
}

#endif // LIBBUILD2_VERSION_SNAPSHOT_HXX
