// file      : build2/version/snapshot.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/version/snapshot.hxx>

#include <build2/filesystem.hxx>

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
      const dir_path& src_root (rs.src_path ());

      // .git can be either a directory or a file in case of a submodule.
      //
      if (build2::entry_exists (src_root / git, true /* follow_symlinks */))
        return extract_snapshot_git (src_root);

      return snapshot ();
    }
  }
}
