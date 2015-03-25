// file      : build/context.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <system_error>

#include <build/diagnostics>

namespace build
{
  template <typename T>
  rmfile_status
  rmfile (const path& f, const T& t)
  {
    // We don't want to print the command if we couldn't remove the
    // file because it does not exist (just like we don't print the
    // update command if the file is up to date). This makes the
    // below code a bit ugly.
    //
    rmfile_status rs;

    try
    {
      rs = try_rmfile (f);
    }
    catch (const std::system_error& e)
    {
      if (verb >= 1)
        text << "rm " << f.string ();
      else
        text << "rm " << t;

      fail << "unable to remove file " << f.string () << ": " << e.what ();
    }

    if (rs == rmfile_status::success)
    {
      if (verb >= 1)
        text << "rm " << f.string ();
      else
        text << "rm " << t;
    }

    return rs;
  }

  template <typename T>
  rmdir_status
  rmdir (const path& d, const T& t)
  {
    bool w (d == work); // Don't try to remove working directory.
    rmdir_status rs;

    // We don't want to print the command if we couldn't remove the
    // directory because it does not exist (just like we don't print
    // mkdir if it already exists) or if it is not empty. This makes
    // the below code a bit ugly.
    //
    try
    {
      rs = !w ? try_rmdir (d) : rmdir_status::not_empty;
    }
    catch (const std::system_error& e)
    {
      if (verb >= 1)
        text << "rmdir " << d.string ();
      else
        text << "rmdir " << t;

      fail << "unable to remove directory " << d.string () << ": "
           << e.what ();
    }

    switch (rs)
    {
    case rmdir_status::success:
      {
        if (verb >= 1)
          text << "rmdir " << d.string ();
        else
          text << "rmdir " << t;

        break;
      }
    case rmdir_status::not_empty:
      {
        if (verb >= 1)
          text << "directory " << d.string () << " is "
               << (w ? "current working directory" : "not empty")
               << ", not removing";

        break;
      }
    case rmdir_status::not_exist:
      break;
    }

    return rs;
  }
}
