// file      : build2/filesystem.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/filesystem>

#include <build2/diagnostics>

using namespace std;
using namespace butl;

namespace build2
{
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

      error << "unable to create directory " << d << ": " << e.what ();
      throw failed ();
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

      error << "unable to create directory " << d << ": " << e.what ();
      throw failed ();
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

    if (!exists (d))
      return rmdir_status::not_exist;

    if (verb >= v)
      text << "rmdir -r " << d;

    try
    {
      butl::rmdir_r (d, dir);
    }
    catch (const system_error& e)
    {
      fail << "unable to remove directory " << d << ": " << e.what ();
    }

    return rmdir_status::success;
  }

  bool
  exists (const path& f, bool fs)
  {
    try
    {
      return file_exists (f, fs);
    }
    catch (const system_error& e)
    {
      error << "unable to stat path " << f << ": " << e.what ();
      throw failed ();
    }
  }

  bool
  exists (const dir_path& d)
  {
    try
    {
      return dir_exists (d);
    }
    catch (const system_error& e)
    {
      error << "unable to stat path " << d << ": " << e.what ();
      throw failed ();
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
      error << "unable to scan directory " << d << ": " << e.what ();
      throw failed ();
    }
  }
}
