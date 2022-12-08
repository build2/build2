// file      : libbuild2/filesystem.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/filesystem.hxx>

#include <libbuild2/context.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  void
  touch (context& ctx, const path& p, bool create, uint16_t v)
  {
    if (verb >= v)
    {
      if (verb >= 2)
        text << "touch " << p;
      else if (verb)
        print_diag ("touch", p);
    }

    if (ctx.dry_run)
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
    auto print = [v, &d] (bool ovr)
    {
      if (verb >= v || ovr)
      {
        if (verb >= 2)
          text << "mkdir " << d;
        else if (verb)
          print_diag ("mkdir", d);
      }
    };

    mkdir_status ms;
    try
    {
      ms = try_mkdir (d);
    }
    catch (const system_error& e)
    {
      print (true);
      fail << "unable to create directory " << d << ": " << e << endf;
    }

    if (ms == mkdir_status::success)
      print (false);

    return ms;
  }

  fs_status<mkdir_status>
  mkdir_p (const dir_path& d, uint16_t v)
  {
    // We don't want to print the command if the directory already exists.
    // This makes the below code a bit ugly.
    //
    auto print = [v, &d] (bool ovr)
    {
      if (verb >= v || ovr)
      {
        if (verb >= 2)
          text << "mkdir -p " << d;
        else if (verb)
          print_diag ("mkdir -p", d);
      }
    };

    mkdir_status ms;
    try
    {
      ms = try_mkdir_p (d);
    }
    catch (const system_error& e)
    {
      print (true);
      fail << "unable to create directory " << d << ": " << e << endf;
    }

    if (ms == mkdir_status::success)
      print (false);

    return ms;
  }

  void
  mvfile (const path& f, const path& t, uint16_t v)
  {
    if (verb >= v)
    {
      if (verb >= 2)
        text << "mv " << f << ' ' << t;
      else if (verb)
        print_diag ("mv", f, t);
    }

    try
    {
      butl::mvfile (f, t, (cpflags::overwrite_content |
                           cpflags::overwrite_permissions));
    }
    catch (const io_error& e)
    {
      fail << "unable to overwrite " << t << " with " << f << ": " << e;
    }
    catch (const system_error& e) // EACCES, etc.
    {
      fail << "unable to move " << f << " to " << t << ": " << e;
    }
  }

  fs_status<rmfile_status>
  rmsymlink (context& ctx, const path& p, bool d, uint16_t v)
  {
    auto print = [&p, v] (bool ovr)
    {
      if (verb >= v || ovr)
      {
        // Note: strip trailing directory separator (but keep as path for
        // relative).
        //
        if (verb >= 2)
          text << "rm " << p.string ();
        else if (verb)
          print_diag ("rm", p.to_directory () ? path (p.string ()) : p);
      }
    };

    rmfile_status rs;

    try
    {
      rs = ctx.dry_run
        ? (butl::entry_exists (p)
           ? rmfile_status::success
           : rmfile_status::not_exist)
        : try_rmsymlink (p, d);
    }
    catch (const system_error& e)
    {
      print (true);
      fail << "unable to remove symlink " << p.string () << ": " << e << endf;
    }

    if (rs == rmfile_status::success)
      print (false);

    return rs;
  }

  fs_status<butl::rmdir_status>
  rmdir_r (context& ctx, const dir_path& d, bool dir, uint16_t v)
  {
    using namespace butl;

    if (work.sub (d)) // Don't try to remove working directory.
      return rmdir_status::not_empty;

    if (!build2::entry_exists (d))
      return rmdir_status::not_exist;

    if (verb >= v)
    {
      if (verb >= 2)
        text << "rmdir -r " << d;
      else if (verb)
        print_diag ("rmdir -r", d);
    }

    if (!ctx.dry_run)
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
  mkdir_buildignore (context& ctx,
                     const dir_path& d,
                     const path& n,
                     uint16_t verbosity)
  {
    fs_status<mkdir_status> r (mkdir (d, verbosity));

    // Create the .buildignore file if the directory was created (and so is
    // empty) or the file doesn't exist.
    //
    path p (d / n);
    if (r || !exists (p))
      touch (ctx, p, true /* create */, verbosity);

    return r;
  }

  bool
  empty_buildignore (const dir_path& d, const path& n)
  {
    try
    {
      for (const dir_entry& de: dir_iterator (d, dir_iterator::no_follow))
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
  rmdir_buildignore (context& ctx,
                     const dir_path& d,
                     const path& n,
                     uint16_t verbosity)
  {
    // We should remove the .buildignore file only if the subsequent rmdir()
    // will succeed. In other words if the directory stays after the function
    // call then the .buildignore file must stay also, if present. Thus, we
    // first check that the directory is otherwise empty and doesn't contain
    // the working directory.
    //
    path p (d / n);
    if (exists (p) && empty_buildignore (d, n) && !work.sub (d))
      rmfile (ctx, p, verbosity);

    // Note that in case of a system error the directory is likely to stay with
    // the .buildignore file already removed. Trying to restore it feels like
    // an overkill here.
    //
    return rmdir (ctx, d, verbosity);
  }

  permissions
  path_perms (const path& p)
  {
    try
    {
      return path_permissions (p);
    }
    catch (const system_error& e)
    {
      fail << "unable to obtain path " << p << " permissions: " << e << endf;
    }
  }

  void
  path_perms (const path& p, permissions f)
  {
    try
    {
      path_permissions (p, f);
    }
    catch (const system_error& e)
    {
      fail << "unable to set path " << p << " permissions: " << e;
    }
  }

  void
  normalize_external (path& f, const char* what)
  {
    // The main motivating case for this logic are C/C++ headers.
    //
    // Interestingly, on most paltforms and with most compilers (Clang on
    // Linux being a notable exception) most system/compiler headers are
    // already normalized.
    //
    path_abnormality a (f.abnormalities ());
    if (a != path_abnormality::none)
    {
      // While we can reasonably expect this path to exit, things do go south
      // from time to time (like compiling under wine with file wlantypes.h
      // included as WlanTypes.h).
      //
      try
      {
        // If we have any parent components, then we have to verify the
        // normalized path matches realized.
        //
        path r;
        if ((a & path_abnormality::parent) == path_abnormality::parent)
        {
          r = f;
          r.realize ();
        }

        try
        {
          f.normalize ();

          // Note that we might still need to resolve symlinks in the
          // normalized path.
          //
          if (!r.empty () && f != r && path (f).realize () != r)
            f = move (r);
        }
        catch (const invalid_path&)
        {
          assert (!r.empty ()); // Shouldn't have failed if no `..`.
          f = move (r);         // Fallback to realize.
        }
      }
      catch (const invalid_path&)
      {
        fail << "invalid " << what << " path '" << f.string () << "'";
      }
      catch (const system_error& e)
      {
        fail << "invalid " << what << " path '" << f.string () << "': " << e;
      }
    }
  }
}
