// file      : libbuild2/functions-filesystem.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbutl/filesystem.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/filesystem.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // Return paths of filesystem entries that match the pattern. See
  // path_search() overloads (below) for details.
  //
  static names
  path_search (const scope* s,
               const path& pattern,
               optional<dir_path>&& start)
  {
    names r;
    auto add = [&r] (path&& p, const std::string&, bool interm) -> bool
    {
      // Canonicalizing paths seems to be the right thing to do. Otherwise, we
      // can end up with different separators in the same path on Windows.
      //
      if (!interm)
        r.emplace_back (
          value_traits<path>::reverse (move (p.canonicalize ())));

      return true;
    };

    auto dangling = [] (const dir_entry& de)
    {
      bool sl (de.ltype () == entry_type::symlink);

      warn << "skipping "
           << (sl ? "dangling symlink" : "inaccessible entry") << ' '
           << de.base () / de.path ();

      return true;
    };

    auto search = [&pattern, &add, &dangling] (const dir_path& start)
    {
      path_search (pattern,
                   add,
                   start,
                   path_match_flags::follow_symlinks,
                   dangling);
    };

    // Print paths "as is" in the diagnostics.
    //
    try
    {
      if (pattern.absolute ())
      {
        search (empty_dir_path /* start */);
      }
      else
      {
        // If the start directory is not specified or is relative, then deduce
        // it based on the current working directory for Shellscript and fail
        // otherwise. Assume Shellscript if the context defines the
        // shellscript.syntax variable.
        //
        if (!start || start->relative ())
        {
          if (s != nullptr && s->ctx.var_shellscript_syntax != nullptr)
          {
            // Note: can also be used in diagnostics.
            //
            start = !start ? work : work / *start;
          }
          else
          {
            diag_record dr (fail);

            if (!start)
              dr << "start directory is not specified";
            else
              dr << "start directory '" << start->representation ()
                 << "' is relative";

            dr << info << "pattern '" << pattern.representation ()
               << "' is relative";
          }
        }

        search (*start);
      }
    }
    catch (const system_error& e)
    {
      diag_record d (fail);
      d << "unable to scan";

      // If the pattern is absolute, then the start directory is not used, and
      // so printing it would be misleading.
      //
      if (start && pattern.relative ())
        d << " '" << start->representation () << "'";

      d << ": " << e
        << info << "pattern: '" << pattern.representation () << "'";
    }

    return r;
  }

  static bool
  file_exists (path&& f)
  {
    if (f.relative () && path_traits::thread_current_directory () != nullptr)
      f.complete ();

    return exists (f);
  }

  static bool
  directory_exists (dir_path&& d)
  {
    if (d.relative () && path_traits::thread_current_directory () != nullptr)
      d.complete ();

    return exists (d);
  }

  void
  filesystem_functions (function_map& m)
  {
    // NOTE: anything that depends on relative path must handle the
    //       thread-specific curren directory override explicitly.

    function_family f (m, "filesystem");

    // $file_exists(<path>)
    //
    // Return true if a filesystem entry at the specified path exists and is a
    // regular file (or is a symlink to a regular file) and false otherwise.
    //
    // Note that this function is not pure.
    //
    {
      auto e (f.insert ("file_exists", false));

      e += [](path f) {return file_exists (move (f));};
      e += [](names ns) {return file_exists (convert<path> (move (ns)));};
    }

    // $directory_exists(<path>)
    //
    // Return true if a filesystem entry at the specified path exists and is a
    // directory (or is a symlink to a directory) and false otherwise.
    //
    // Note that this function is not pure.
    //
    {
      auto e (f.insert ("directory_exists", false));

      e += [](path f) {return directory_exists (path_cast<dir_path> (move (f)));};
      e += [](names ns) {return directory_exists (convert<dir_path> (move (ns)));};
    }

    // $path_search(<pattern>[, <start-dir>])
    //
    // Return filesystem paths that match the shell-like wildcard pattern. If
    // the pattern is an absolute path, then the start directory is ignored
    // (if present). Otherwise, the start directory must be specified and be
    // absolute, except for Shellscript. For Shellscript, if the start
    // directory is not specified, then the current working directory is
    // assumed, and if the relative start directory is specified, then the
    // current working directory is used as a base.
    //
    // Note that this function is not pure.
    //

    // @@ In the future we may want to add a flag that controls the
    //    dangling/inaccessible treatment.
    //
    {
      auto e (f.insert ("path_search", false));

      e += [](const scope* s, path pattern, optional<dir_path> start)
      {
        return path_search (s, pattern, move (start));
      };

      e += [](const scope* s, path pattern, names start)
      {
        return path_search (s, pattern, convert<dir_path> (move (start)));
      };

      e += [](const scope* s, names pattern, optional<dir_path> start)
      {
        return path_search (s, convert<path> (move (pattern)), move (start));
      };

      e += [](const scope* s, names pattern, names start)
      {
        return path_search (s,
                            convert<path>     (move (pattern)),
                            convert<dir_path> (move (start)));
      };
    }
  }
}
