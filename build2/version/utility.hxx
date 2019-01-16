// file      : build2/version/utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_VERSION_UTILITY_HXX
#define BUILD2_VERSION_UTILITY_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/filesystem.hxx>

namespace build2
{
  namespace version
  {
    // Re-serialize the manifest fixing up the version. Note that this will
    // not preserve comments. Probably acceptable for snapshots.
    //
    auto_rmfile
    fixup_manifest (const path& in, path out, const standard_version&);
  }
}

#endif // BUILD2_VERSION_UTILITY_HXX
