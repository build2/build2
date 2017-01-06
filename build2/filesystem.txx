// file      : build2/filesystem.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/context>     // work
#include <build2/diagnostics>

namespace build2
{
  template <typename T>
  fs_status<butl::rmfile_status>
  rmfile (const path& f, const T& t, uint16_t v)
  {
    using namespace butl;

    // We don't want to print the command if we couldn't remove the file
    // because it does not exist (just like we don't print the update command
    // if the file is up to date). This makes the below code a bit ugly.
    //
    auto print = [&f, &t, v] ()
    {
      if (verb >= v)
      {
        if (verb >= 2)
          text << "rm " << f;
        else if (verb)
          text << "rm " << t;
      }
    };

    rmfile_status rs;

    try
    {
      rs = try_rmfile (f);
    }
    catch (const system_error& e)
    {
      print ();
      error << "unable to remove file " << f << ": " << e;
      throw failed ();
    }

    if (rs == rmfile_status::success)
      print ();

    return rs;
  }

  template <typename T>
  fs_status<butl::rmdir_status>
  rmdir (const dir_path& d, const T& t, uint16_t v)
  {
    using namespace butl;

    bool w (work.sub (d)); // Don't try to remove working directory.
    rmdir_status rs;

    // We don't want to print the command if we couldn't remove the directory
    // because it does not exist (just like we don't print mkdir if it already
    // exists) or if it is not empty. This makes the below code a bit ugly.
    //
    auto print = [&d, &t, v] ()
    {
      if (verb >= v)
      {
        if (verb >= 2)
          text << "rm " << d;
        else if (verb)
          text << "rm " << t;
      }
    };

    try
    {
      rs = !w ? try_rmdir (d) : rmdir_status::not_empty;
    }
    catch (const system_error& e)
    {
      print ();
      error << "unable to remove directory " << d << ": " << e;
      throw failed ();
    }

    switch (rs)
    {
    case rmdir_status::success:
      {
        print ();
        break;
      }
    case rmdir_status::not_empty:
      {
        if (verb >= v && verb >= 2)
        {
          text << d << " is "
               << (w ? "current working directory" : "not empty")
               << ", not removing";
        }
        break;
      }
    case rmdir_status::not_exist:
      break;
    }

    return rs;
  }
}
