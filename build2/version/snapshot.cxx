// file      : build2/version/snapshot.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/version/snapshot>

#include <build2/filesystem>

using namespace std;

namespace build2
{
  namespace version
  {
    snapshot
    extract_snapshot_git (const dir_path&);

    static const dir_path git (".git");

    snapshot
    extract_snapshot (const scope& rs)
    {
      const dir_path& src_root (rs.src_path ());

      if (exists (src_root / git))
        return extract_snapshot_git (src_root);

      return snapshot ();
    }
  }
}
