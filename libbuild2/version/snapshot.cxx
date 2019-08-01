// file      : libbuild2/version/snapshot.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/version/snapshot.hxx>

#include <libbuild2/filesystem.hxx>

using namespace std;

namespace build2
{
  namespace version
  {
    snapshot
    extract_snapshot_git (const dir_path&);

    static const path git (".git");

    snapshot
    extract_snapshot (const scope& rs)
    {
      // Ignore errors when checking for existence since we may be iterating
      // over directories past any reasonable project boundaries.
      //
      for (dir_path d (rs.src_path ()); !d.empty (); d = d.directory ())
      {
        // .git can be either a directory or a file in case of a submodule.
        //
        if (butl::entry_exists (d / git,
                                true /* follow_symlinks */,
                                true /* ignore_errors */))
          return extract_snapshot_git (d);
      }

      return snapshot ();
    }
  }
}
