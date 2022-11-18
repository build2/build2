// file      : libbuild2/filesystem.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/diagnostics.hxx>

namespace build2
{
  template <typename T>
  fs_status<butl::rmfile_status>
  rmfile (context& ctx, const path& f, const T& t, uint16_t v)
  {
    using namespace butl;

    // We don't want to print the command if we couldn't remove the file
    // because it does not exist (just like we don't print the update command
    // if the file is up to date). But we always want to print some command
    // before we issue diagnostics. This makes the below code a bit ugly.
    //
    auto print = [&f, &t, v] (bool ovr)
    {
      if (verb >= v || ovr)
      {
        if (verb >= 2)
          text << "rm " << f;
        else if (verb)
          print_diag ("rm", t); // T can be target or path.
      }
    };

    rmfile_status rs;

    try
    {
      rs = ctx.dry_run
        ? file_exists (f) ? rmfile_status::success : rmfile_status::not_exist
        : try_rmfile (f);
    }
    catch (const system_error& e)
    {
      print (true);
      fail << "unable to remove file " << f << ": " << e << endf;
    }

    if (rs == rmfile_status::success)
      print (false);

    return rs;
  }

  template <typename T>
  fs_status<butl::rmdir_status>
  rmdir (context& ctx, const dir_path& d, const T& t, uint16_t v)
  {
    using namespace butl;

    // We don't want to print the command if we couldn't remove the directory
    // because it does not exist (just like we don't print mkdir if it already
    // exists) or if it is not empty. But we always want to print some command
    // before we issue diagnostics. This makes the below code a bit ugly.
    //
    auto print = [&d, &t, v] (bool ovr)
    {
      if (verb >= v || ovr)
      {
        if (verb >= 2)
          text << "rmdir " << d;
        else if (verb)
          print_diag ("rmdir", t); // T can be target or dir_path.
      }
    };

    bool w (false); // Don't try to remove working directory.
    rmdir_status rs;
    try
    {
      rs = ctx.dry_run
        ? dir_exists (d) ? rmdir_status::success : rmdir_status::not_exist
        : !(w = work.sub (d)) ? try_rmdir (d) : rmdir_status::not_empty;
    }
    catch (const system_error& e)
    {
      print (true);
      fail << "unable to remove directory " << d << ": " << e << endf;
    }

    switch (rs)
    {
    case rmdir_status::success:
      {
        print (false);
        break;
      }
    case rmdir_status::not_empty:
      {
        if (verb >= v && verb >= 2)
        {
          info << d << " is "
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
