// file      : libbuild2/functions-filesystem.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbutl/filesystem.mxx>

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  // Return paths of filesystem entries that match the pattern. See
  // path_search() overloads (below) for details.
  //
  static names
  path_search (const path& pattern, const optional<dir_path>& start)
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

    // Print paths "as is" in the diagnostics.
    //
    try
    {
      if (pattern.absolute ())
        path_search (pattern, add);
      else
      {
        // An absolute start directory must be specified for the relative
        // pattern.
        //
        if (!start || start->relative ())
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

        path_search (pattern, add, *start);
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

  void
  filesystem_functions (function_map& m)
  {
    // @@ Maybe we should have the ability to mark the whole family as not
    //    pure?

    function_family f (m, "filesystem");

    // path_search
    //
    // Return filesystem paths that match the pattern. If the pattern is an
    // absolute path, then the start directory is ignored (if present).
    // Otherwise, the start directory must be specified and be absolute.
    //
    // Note that this function is not pure.
    //
    {
      auto e (f.insert ("path_search", false));

      e += [](path pattern, optional<dir_path> start)
      {
        return path_search (pattern, start);
      };

      e += [](path pattern, names start)
      {
        return path_search (pattern, convert<dir_path> (move (start)));
      };

      e += [](names pattern, optional<dir_path> start)
      {
        return path_search (convert<path> (move (pattern)), start);
      };

      e += [](names pattern, names start)
      {
        return path_search (convert<path>     (move (pattern)),
                            convert<dir_path> (move (start)));
      };
    }

  }
}
