// file      : libbuild2/version/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_VERSION_UTILITY_HXX
#define LIBBUILD2_VERSION_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/context.hxx>
#include <libbuild2/filesystem.hxx>

namespace build2
{
  namespace version
  {
    // Re-serialize the manifest fixing up the version. Note that this will
    // not preserve comments. Probably acceptable for snapshots.
    //
    auto_rmfile
    fixup_manifest (context&,
                    const path& in,
                    path out,
                    const standard_version&);
  }
}

#endif // LIBBUILD2_VERSION_UTILITY_HXX
