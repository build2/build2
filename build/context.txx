// file      : build/context.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <system_error>

#include <build/diagnostics>

namespace build
{
  template <typename T>
  fs_status<rmfile_status>
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
      if (verb)
        text << "rm " << f;
      else
        text << "rm " << t;

      fail << "unable to remove file " << f << ": " << e.what ();
    }

    if (rs == rmfile_status::success)
    {
      if (verb)
        text << "rm " << f;
      else
        text << "rm " << t;
    }

    return rs;
  }

  template <typename T>
  fs_status<rmdir_status>
  rmdir (const dir_path& d, const T& t)
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
      if (verb)
        text << "rmdir " << d;
      else
        text << "rmdir " << t;

      fail << "unable to remove directory " << d << ": " << e.what ();
    }

    switch (rs)
    {
    case rmdir_status::success:
      {
        if (verb)
          text << "rmdir " << d;
        else
          text << "rmdir " << t;

        break;
      }
    case rmdir_status::not_empty:
      {
        if (verb)
          text << "directory " << d << " is "
               << (w ? "current working directory" : "not empty")
               << ", not removing";

        break;
      }
    case rmdir_status::not_exist:
      break;
    }

    return rs;
  }

  template <typename K>
  basic_path<char, K>
  relative (const basic_path<char, K>& p)
  {
    typedef basic_path<char, K> path;

    const dir_path& b (*relative_base);

    if (b.empty ())
      return p;

    if (p.sub (b))
      return p.leaf (b);

    // If base is a sub-path of {src,out}_root and this path is also a
    // sub-path of it, then use '..' to form a relative path.
    //
    // Don't think this is a good heuristic. For example, why shouldn't
    // we display paths from imported projects as relative if they are
    // more readable than absolute?
    //
    /*
    if ((work.sub (src_root) && p.sub (src_root)) ||
        (work.sub (out_root) && p.sub (out_root)))
      return p.relative (work);
    */

    if (p.root_directory () == b.root_directory ())
    {
      path r (p.relative (b));

      if (r.string ().size () < p.string ().size ())
        return r;
    }

    return p;
  }
}
