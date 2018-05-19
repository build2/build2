// file      : build2/functions-filesystem.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbutl/filesystem.mxx>

#include <build2/function.hxx>
#include <build2/variable.hxx>

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

  using butl::path_match;

  // Return true if a path for a filesystem entry matches the pattern. See
  // path_match() overloads (below) for details.
  //
  static bool
  path_match (const path& pattern,
              const path& entry,
              const optional<dir_path>& start)
  {
    // If pattern and entry are both either absolute or relative and
    // non-empty, and the first pattern component is not a self-matching
    // wildcard, then ignore the start directory.
    //
    bool rel (pattern.relative () == entry.relative () &&
              !pattern.empty () && !entry.empty ());

    bool self (!pattern.empty () &&
               (*pattern.begin ()).find ("***") != string::npos);

    if (rel && !self)
      return path_match (pattern, entry);

    // The start directory must be specified and be absolute.
    //
    if (!start || start->relative ())
    {
      diag_record dr (fail);

      // Print paths "as is".
      //
      if (!start)
        dr << "start directory is not specified";
      else
        dr << "start directory path '" << start->representation ()
           << "' is relative";

      dr << info << "pattern: '" << pattern.representation () << "'"
         << info << "entry: '" << entry.representation () << "'";
    }

    return path_match (pattern, entry, *start);
  }

  void
  filesystem_functions ()
  {
    function_family f ("filesystem");

    // path_search
    //
    // Return filesystem paths that match the pattern. If the pattern is an
    // absolute path, then the start directory is ignored (if present).
    // Otherwise, the start directory must be specified and be absolute.
    //
    f["path_search"] = [](path pattern, optional<dir_path> start)
    {
      return path_search (pattern, start);
    };

    f["path_search"] = [](path pattern, names start)
    {
      return path_search (pattern, convert<dir_path> (move (start)));
    };

    f["path_search"] = [](names pattern, optional<dir_path> start)
    {
      return path_search (convert<path> (move (pattern)), start);
    };

    f["path_search"] = [](names pattern, names start)
    {
      return path_search (convert<path>     (move (pattern)),
                          convert<dir_path> (move (start)));
    };

    // path_match
    //
    // Match a filesystem entry name against a name pattern (both are strings),
    // or a filesystem entry path against a path pattern. For the latter case
    // the start directory may also be required (see below). The semantics of
    // the pattern and name/entry arguments is determined according to the
    // following rules:
    //
    // - The arguments must be of the string or path types, or be untyped.
    //
    // - If one of the arguments is typed, then the other one must be of the
    //   same type or be untyped. In the later case, an untyped argument is
    //   converted to the type of the other argument.
    //
    // - If both arguments are untyped and the start directory is specified,
    //   then the arguments are converted to the path type.
    //
    // - If both arguments are untyped and the start directory is not
    //   specified, then, if one of the arguments is syntactically a path (the
    //   value contains a directory separator), convert them to the path type,
    //   otherwise to the string type (match as names).
    //
    // If pattern and entry paths are both either absolute or relative and
    // non-empty, and the first pattern component is not a self-matching
    // wildcard (doesn't contain ***), then the start directory is not
    // required, and is ignored if specified. Otherwise, the start directory
    // must be specified and be an absolute path.
    //
    // Name matching.
    //
    f["path_match"] = [](string pattern, string name)
    {
      return path_match (pattern, name);
    };

    f["path_match"] = [](string pattern, names name)
    {
      return path_match (pattern, convert<string> (move (name)));
    };

    f["path_match"] = [](names pattern, string name)
    {
      return path_match (convert<string> (move (pattern)), name);
    };

    // Path matching.
    //
    //                   path      path      *
    //
    f["path_match"] = [](path pat, path ent, optional<dir_path> start)
    {
      return path_match (pat, ent, start);
    };

    f["path_match"] = [](path pat, path ent, names start)
    {
      return path_match (pat, ent, convert<dir_path> (move (start)));
    };

    //                   path      untyped    *
    //
    f["path_match"] = [](path pat, names ent, optional<dir_path> start)
    {
      return path_match (pat, convert<path> (move (ent)), start);
    };

    f["path_match"] = [](path pat, names ent, names start)
    {
      return path_match (pat,
                         convert<path> (move (ent)),
                         convert<dir_path> (move (start)));
    };

    //                   untyped    path      *
    //
    f["path_match"] = [](names pat, path ent, optional<dir_path> start)
    {
      return path_match (convert<path> (move (pat)), ent, start);
    };

    f["path_match"] = [](names pat, path ent, names start)
    {
      return path_match (convert<path> (move (pat)),
                         ent,
                         convert<dir_path> (move (start)));
    };

    // The semantics depends on the presence of the start directory or the
    // first two argument syntactic representation.
    //
    //                   untyped    untyped    *
    //
    f["path_match"] = [](names pat, names ent, optional<dir_path> start)
    {
      auto path_arg = [] (const names& a) -> bool
      {
        return a.size () == 1 &&
        (a[0].directory () ||
         a[0].value.find_first_of (path::traits::directory_separators) !=
         string::npos);
      };

      return start || path_arg (pat) || path_arg (ent)
        ? path_match (convert<path> (move (pat)),   // Match as paths.
                      convert<path> (move (ent)),
                      start)
        : path_match (convert<string> (move (pat)), // Match as names.
                      convert<string> (move (ent)));
    };

    f["path_match"] = [](names pat, names ent, names start)
    {
      // Match as paths.
      //
      return path_match (convert<path> (move (pat)),
                         convert<path> (move (ent)),
                         convert<dir_path> (move (start)));
    };
  }
}
