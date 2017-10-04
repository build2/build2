// file      : build2/filesystem.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/filesystem.hxx>

#include <build2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  bool
  touch (const path& p, bool create, uint16_t v)
  {
    if (verb >= v)
      text << "touch " << p;

    try
    {
      return touch_file (p, create);
    }
    catch (const system_error& e)
    {
      fail << "unable to touch file " << p << ": " << e << endf;
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

    try
    {
      butl::rmdir_r (d, dir);
    }
    catch (const system_error& e)
    {
      fail << "unable to remove directory " << d << ": " << e;
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
}
