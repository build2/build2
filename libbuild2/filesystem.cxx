// file      : libbuild2/filesystem.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/filesystem.hxx>

#include <libbuild2/context.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  void
  touch (const path& p, bool create, uint16_t v)
  {
    if (verb >= v)
      text << "touch " << p;

    if (dry_run)
      return;

    try
    {
      touch_file (p, create);
    }
    catch (const system_error& e)
    {
      fail << "unable to touch file " << p << ": " << e << endf;
    }
  }

  timestamp
  mtime (const char* p)
  {
    try
    {
      return file_mtime (p);
    }
    catch (const system_error& e)
    {
      fail << "unable to obtain file " << p << " modification time: " << e
           << endf;
    }
  }

  fs_status<mkdir_status>
  mkdir (const dir_path& d, uint16_t v)
  {
    // We don't want to print the command if the directory already exists.
    // This makes the below code a bit ugly.
    //
    mkdir_status ms;

    try
    {
      ms = try_mkdir (d);
    }
    catch (const system_error& e)
    {
      if (verb >= v)
        text << "mkdir " << d;

      fail << "unable to create directory " << d << ": " << e << endf;
    }

    if (ms == mkdir_status::success)
    {
      if (verb >= v)
        text << "mkdir " << d;
    }

    return ms;
  }

  fs_status<mkdir_status>
  mkdir_p (const dir_path& d, uint16_t v)
  {
    // We don't want to print the command if the directory already exists.
    // This makes the below code a bit ugly.
    //
    mkdir_status ms;

    try
    {
      ms = try_mkdir_p (d);
    }
    catch (const system_error& e)
    {
      if (verb >= v)
        text << "mkdir -p " << d;

      fail << "unable to create directory " << d << ": " << e << endf;
    }

    if (ms == mkdir_status::success)
    {
      if (verb >= v)
        text << "mkdir -p " << d;
    }

    return ms;
  }

  fs_status<rmfile_status>
  rmsymlink (const path& p, bool d, uint16_t v)
  {
    auto print = [&p, v] ()
    {
      if (verb >= v)
        text << "rm " << p.string ();
    };

    rmfile_status rs;

    try
    {
      rs = dry_run
        ? (butl::entry_exists (p)
           ? rmfile_status::success
           : rmfile_status::not_exist)
        : try_rmsymlink (p, d);
    }
    catch (const system_error& e)
    {
      print ();
      fail << "unable to remove symlink " << p.string () << ": " << e << endf;
    }

    if (rs == rmfile_status::success)
      print ();

    return rs;
  }

  fs_status<butl::rmdir_status>
  rmdir_r (const dir_path& d, bool dir, uint16_t v)
  {
    using namespace butl;

    if (work.sub (d)) // Don't try to remove working directory.
      return rmdir_status::not_empty;

    if (!build2::entry_exists (d))
      return rmdir_status::not_exist;

    if (verb >= v)
      text << "rmdir -r " << d;

    if (!dry_run)
    {
      try
      {
        butl::rmdir_r (d, dir);
      }
      catch (const system_error& e)
      {
        fail << "unable to remove directory " << d << ": " << e;
      }
    }

    return rmdir_status::success;
  }

  bool
  exists (const path& f, bool fs, bool ie)
  {
    try
    {
      return file_exists (f, fs, ie);
    }
    catch (const system_error& e)
    {
      fail << "unable to stat path " << f << ": " << e << endf;
    }
  }

  bool
  exists (const dir_path& d, bool ie)
  {
    try
    {
      return dir_exists (d, ie);
    }
    catch (const system_error& e)
    {
      fail << "unable to stat path " << d << ": " << e << endf;
    }
  }

  bool
  entry_exists (const path& p, bool fs, bool ie)
  {
    try
    {
      return butl::entry_exists (p, fs, ie);
    }
    catch (const system_error& e)
    {
      fail << "unable to stat path " << p << ": " << e << endf;
    }
  }

  bool
  empty (const dir_path& d)
  {
    try
    {
      return dir_empty (d);
    }
    catch (const system_error& e)
    {
      fail << "unable to scan directory " << d << ": " << e << endf;
    }
  }

  fs_status<mkdir_status>
  mkdir_buildignore (const dir_path& d, const path& n, uint16_t verbosity)
  {
    fs_status<mkdir_status> r (mkdir (d, verbosity));

    // Create the .buildignore file if the directory was created (and so is
    // empty) or the file doesn't exist.
    //
    path p (d / n);
    if (r || !exists (p))
      touch (p, true /* create */, verbosity);

    return r;
  }

  bool
  empty_buildignore (const dir_path& d, const path& n)
  {
    try
    {
      for (const dir_entry& de: dir_iterator (d, false /* ignore_dangling */))
      {
        // The .buildignore filesystem entry should be of the regular file
        // type.
        //
        if (de.path () != n || de.ltype () != entry_type::regular)
          return false;
      }
    }
    catch (const system_error& e)
    {
      fail << "unable to scan directory " << d << ": " << e;
    }

    return true;
  }

  fs_status<rmdir_status>
  rmdir_buildignore (const dir_path& d, const path& n, uint16_t verbosity)
  {
    // We should remove the .buildignore file only if the subsequent rmdir()
    // will succeed. In other words if the directory stays after the function
    // call then the .buildignore file must stay also, if present. Thus, we
    // first check that the directory is otherwise empty and doesn't contain
    // the working directory.
    //
    path p (d / n);
    if (exists (p) && empty_buildignore (d, n) && !work.sub (d))
      rmfile (p, verbosity);

    // Note that in case of a system error the directory is likely to stay with
    // the .buildignore file already removed. Trying to restore it feels like
    // an overkill here.
    //
    return rmdir (d, verbosity);
  }
}
