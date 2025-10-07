// file      : libbuild2/version-snapshot.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_VERSION_SNAPSHOT_HXX
#define LIBBUILD2_VERSION_SNAPSHOT_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>

namespace build2
{
  struct version_snapshot
  {
    uint64_t sn = 0;
    string id;
    bool committed = false;

    bool
    empty () const {return sn == 0;}
  };

  // Return empty snapshot, if unknown scm, and empty snapshot id, if the
  // repository has no commits or there are some uncommitted or untracked
  // changes. Optionally (committed_version is true), ignore any uncommitted
  // or untracked changes.
  //
  LIBBUILD2_SYMEXPORT version_snapshot
  extract_version_snapshot (const scope& rs, bool committed_version = false);
}

#endif // LIBBUILD2_VERSION_SNAPSHOT_HXX
