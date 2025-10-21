// file      : libbuild2/version-snapshot.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/version-snapshot.hxx>

#include <libbuild2/filesystem.hxx>

using namespace std;

namespace build2
{
  version_snapshot
  extract_version_snapshot_git (context&, dir_path, bool committed_version);

  static const path git (".git");

  version_snapshot
  extract_version_snapshot (const scope& rs, bool committed_version)
  {
    // Resolve the path symlink components to make sure that if we are
    // extracting snapshot for a subproject which is symlinked from the git
    // submodule, then we end up with a root of the git submodule repository
    // rather than the containing repository root.
    //
    dir_path d (rs.src_path ());

    try
    {
      d.realize ();
    }
    catch (const invalid_path&) // Some component doesn't exist.
    {
      return version_snapshot ();
    }
    catch (const system_error& e)
    {
      fail << "unable to obtain real path for " << d << ": " << e;
    }

    for (; !d.empty (); d = d.directory ())
    {
      // .git can be either a directory or a file in case of a submodule.
      //
      if (butl::entry_exists (d / git,
                              true /* follow_symlinks */,
                              true /* ignore_errors */))
        return extract_version_snapshot_git (rs.ctx,
                                             move (d),
                                             committed_version);
    }

    return version_snapshot ();
  }
}
