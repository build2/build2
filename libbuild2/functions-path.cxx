// file      : libbuild2/functions-path.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbutl/path-pattern.hxx>

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  extern bool
  functions_sort_flags (optional<names>); // functions-builtin.cxx

  static value
  path_thunk (const scope* base,
              vector_view<value> args,
              const function_overload& f)
  try
  {
    return function_family::default_thunk (base, move (args), f);
  }
  catch (const invalid_path& e)
  {
    fail << "invalid path: '" << e.path << "'" << endf;
  }

  static value
  concat_path_string (path l, string sr)
  {
    if (path::traits_type::is_separator (sr[0])) // '\0' if empty.
    {
      sr.erase (0, 1);
      path pr (move (sr));
      pr.canonicalize (); // Convert to canonical directory separators.

      // If RHS is syntactically a directory (ends with a trailing slash),
      // then return it as dir_path, not path.
      //
      if (pr.to_directory () || pr.empty ())
        return value (
          path_cast<dir_path> (move (l)) /= path_cast<dir_path> (move (pr)));
      else
        l /= pr;
    }
    else
      l += sr;

    return value (move (l));
  }

  static value
  concat_dir_path_string (dir_path l, string sr)
  {
    if (path::traits_type::is_separator (sr[0])) // '\0' if empty.
      sr.erase (0, 1);

    path pr (move (sr));
    pr.canonicalize (); // Convert to canonical directory separators.

    // If RHS is syntactically a directory (ends with a trailing slash), then
    // return it as dir_path, not path.
    //
    return pr.to_directory () || pr.empty ()
      ? value (move (l /= path_cast<dir_path> (move (pr))))
      : value (path_cast<path> (move (l)) /= pr);
  }

  // Return untyped value or NULL value if extension is not present.
  //
  static inline value
  extension (path p)
  {
    const char* e (p.extension_cstring ());

    if (e == nullptr)
      return value ();

    names r;
    r.emplace_back (e);
    return value (move (r));
  }

  template <typename P>
  static inline P
  leaf (const P& p, const optional<dir_path>& d)
  {
    if (!d)
      return p.leaf ();

    try
    {
      return p.leaf (*d);
    }
    catch (const invalid_path&)
    {
      fail << "'" << *d << "' is not a prefix of '" << p << "'" << endf;
    }
  }

  template <typename P>
  static inline P
  relative (const P& p, const dir_path& d)
  {
    try
    {
      return p.relative (d); // Note: cannot move due to diagnostics.
    }
    catch (const invalid_path&)
    {
      fail << "'" << p << "' cannot be made relative to '" << d << "'" << endf;
    }
  }

  using butl::path_match;

  // Return true if a path matches the pattern. See path_match() overloads
  // (below) for details.
  //
  static bool
  path_match (const path& entry,
              const path& pattern,
              const optional<dir_path>& start)
  {
    // If pattern and entry are both either absolute or relative and
    // non-empty, and the first pattern component is not a self-matching
    // wildcard, then ignore the start directory.
    //
    bool rel (pattern.relative () == entry.relative () &&
              !pattern.empty () && !entry.empty ());

    if (rel && !path_pattern_self_matching (pattern))
      return path_match (entry, pattern);

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

    return path_match (entry, pattern, *start);
  }

  // Don't fail for absolute paths on Windows and, for example, just return
  // c:/foo for c:\foo.
  //
  template <typename P>
  static inline string
  posix_string (P&& p)
  {
#ifndef _WIN32
    return move (p).posix_string ();
#else
    if (p.relative ())
      return move (p).posix_string ();

    // Note: also handles root directories.
    //
    dir_path d (p.root_directory ());
    return d.string () + '/' + p.leaf (d).posix_string ();
#endif
  }

  // Similar to the above don't fail for absolute paths on Windows.
  //
  template <typename P>
  static inline string
  posix_representation (P&& p)
  {
#ifndef _WIN32
    return move (p).posix_representation ();
#else
    if (p.relative ())
      return move (p).posix_representation ();

    // Note: also handles root directories.
    //
    dir_path d (p.root_directory ());
    return d.string () + '/' + p.leaf (d).posix_representation ();
#endif
  }

  template <typename P>
  static bool
  try_normalize (P& p)
  {
    try
    {
      p.normalize ();
      return true;
    }
    catch (const invalid_path&) {}

    return false;
  }

  template <typename P>
  static bool
  try_actualize (P& p)
  {
    try
    {
      p.normalize (true);
      return true;
    }
    catch (const invalid_path&) {}
    catch (const system_error&) {}

    return false;
  }

  void
  path_functions (function_map& m)
  {
    function_family f (m, "path", &path_thunk);

    // $string(<paths>)
    //
    // Return the traditional string representation of a path (or a list of
    // string representations for a list of paths). In particular, for
    // directory paths, the traditional representation does not include the
    // trailing directory separator (except for the POSIX root directory). See
    // `$representation()` below for the precise string representation.
    //

    // Note that we must handle NULL values (relied upon by the parser
    // to provide conversion semantics consistent with untyped values).
    //
    f["string"] += [](path* p)
    {
      return p != nullptr ? move (*p).string () : string ();
    };

    f["string"] += [](paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (move (p).string ());
      return r;
    };

    f["string"] += [](dir_paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (move (p).string ());
      return r;
    };

    // $posix_string(<paths>)
    // $path.posix_string(<untyped>)
    //
    // Return the traditional string representation of a path (or a list of
    // string representations for a list of paths) using the POSIX directory
    // separators (forward slashes).
    //
    f["posix_string"] += [](path p)     {return posix_string (move (p));};
    f["posix_string"] += [](dir_path p) {return posix_string (move (p));};

    f["posix_string"] += [](paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (posix_string (move (p)));
      return r;
    };

    f["posix_string"] += [](dir_paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (posix_string (move (p)));
      return r;
    };

    f[".posix_string"] += [](names ns)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of strings.
      //
      for (name& n: ns)
      {
        n = n.directory ()
            ? posix_string (move (n.dir))
            : posix_string (convert<path> (move (n)));
      }
      return ns;
    };

    // $representation(<paths>)
    //
    // Return the precise string representation of a path (or a list of string
    // representations for a list of paths). In particular, for directory
    // paths, the precise representation includes the trailing directory
    // separator. See `$string()` above for the traditional string
    // representation.
    //
    f["representation"] += [](path p) {return move (p).representation ();};

    f["representation"] += [](paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (move (p).representation ());
      return r;
    };

    f["representation"] += [](dir_paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (move (p).representation ());
      return r;
    };

    // $posix_representation(<paths>)
    // $path.posix_representation(<untyped>)
    //
    // Return the precise string representation of a path (or a list of string
    // representations for a list of paths) using the POSIX directory
    // separators (forward slashes).
    //
    f["posix_representation"] += [](path p)
    {
      return posix_representation (move (p));
    };

    f["posix_representation"] += [](dir_path p)
    {
      return posix_representation (move (p));
    };

    f["posix_representation"] += [](paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (posix_representation (move (p)));
      return r;
    };

    f["posix_representation"] += [](dir_paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (posix_representation (move (p)));
      return r;
    };

    f[".posix_representation"] += [](names ns)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of strings.
      //
      for (name& n: ns)
      {
        n = n.directory ()
            ? posix_representation (move (n.dir))
            : posix_representation (convert<path> (move (n)));
      }
      return ns;
    };

    // $absolute(<path>)
    // $path.absolute(<untyped>)
    //
    // Return true if the path is absolute and false otherwise.
    //
    f["absolute"] += [](path p)
    {
      return p.absolute ();
    };

    f[".absolute"] += [](names ns)
    {
      return convert<path> (move (ns)).absolute ();
    };

    // $simple(<path>)
    // $path.simple(<untyped>)
    //
    // Return true if the path is simple, that is, has no direcrory component,
    // and false otherwise.
    //
    // Note that on POSIX `/foo` is not a simple path (it is `foo` in the root
    // directory) while `/` is (it is the root directory).
    //
    f["simple"] += [](path p)
    {
      return p.simple ();
    };

    f[".simple"] += [](names ns)
    {
      return convert<path> (move (ns)).simple ();
    };

    // $sub_path(<path>, <path>)
    // $path.sub_path(<untyped>, <untyped>)
    //
    // Return true if the path specified as the first argument is a sub-path
    // of the one specified as the second argument (in other words, the second
    // argument is a prefix of the first) and false otherwise. Both paths are
    // expected to be normalized. Note that this function returns true if the
    // paths are equal. Empty path is considered a prefix of any path.
    //
    f["sub_path"] += [](path p, value v)
    {
      return p.sub (convert_to_base<path> (move (v)));
    };

    f[".sub_path"] += [](names ns, value v)
    {
      return convert<path> (move (ns)).sub (convert_to_base<path> (move (v)));
    };

    // $super_path(<path>, <path>)
    // $path.super_path(<untyped>, <untyped>)
    //
    // Return true if the path specified as the first argument is a super-path
    // of the one specified as the second argument (in other words, the second
    // argument is a suffix of the first) and false otherwise. Both paths are
    // expected to be normalized. Note that this function returns true if the
    // paths are equal. Empty path is considered a suffix of any path.
    //
    f["super_path"] += [](path p, value v)
    {
      return p.sup (convert_to_base<path> (move (v)));
    };

    f[".super_path"] += [](names ns, value v)
    {
      return convert<path> (move (ns)).sup (convert_to_base<path> (move (v)));
    };

    // $directory(<paths>)
    // $path.directory(<untyped>)
    //
    // Return the directory part of a path (or a list of directory parts for a
    // list of paths) or an empty path if there is no directory. A directory of
    // a root directory is an empty path.
    //
    f["directory"] += &path::directory;

    f["directory"] += [](paths v)
    {
      dir_paths r;
      for (const path& p: v)
        r.push_back (p.directory ());
      return r;
    };

    f["directory"] += [](dir_paths v)
    {
      for (dir_path& p: v)
        p = p.directory ();
      return v;
    };

    f[".directory"] += [](names ns)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as list of directory names.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir = n.dir.directory ();
        else
          n = convert<path> (move (n)).directory ();
      }
      return ns;
    };

    // $root_directory(<paths>)
    // $path.root_directory(<untyped>)
    //
    // Return the root directory of a path (or a list of root directories for
    // a list of paths) or an empty path if the specified path is not
    // absolute.
    //
    f["root_directory"] += &path::root_directory;

    f["root_directory"] += [](paths v)
    {
      dir_paths r;
      for (const path& p: v)
        r.push_back (p.root_directory ());
      return r;
    };

    f["root_directory"] += [](dir_paths v)
    {
      for (dir_path& p: v)
        p = p.root_directory ();
      return v;
    };

    f[".root_directory"] += [](names ns)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as list of directory names.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir = n.dir.root_directory ();
        else
          n = convert<path> (move (n)).root_directory ();
      }
      return ns;
    };

    // $leaf(<paths>)
    // $path.leaf(<untyped>)
    // $leaf(<paths>, <dir-path>)
    // $path.leaf(<untyped>, <dir-path>)
    //
    // First form (one argument): return the last component of a path (or a
    // list of last components for a list of paths).
    //
    // Second form (two arguments): return a path without the specified
    // directory part (or a list of paths without the directory part for a
    // list of paths). Return an empty path if the paths are the same. Issue
    // diagnostics and fail if the directory is not a prefix of the
    // path. Note: expects both paths to be normalized.
    //
    f["leaf"] += &path::leaf;

    f["leaf"] += [](path p, dir_path d)
    {
      return leaf (p, move (d));
    };

    f["leaf"] += [](paths v, optional<dir_path> d)
    {
      for (path& p: v)
        p = leaf (p, d);
      return v;
    };

    f["leaf"] += [](dir_paths v, optional<dir_path> d)
    {
      for (dir_path& p: v)
        p = leaf (p, d);
      return v;
    };

    f[".leaf"] += [](names ns, optional<dir_path> d)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of (potentially
      // mixed) paths.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir = leaf (n.dir, d);
        else
          n.value = leaf (convert<path> (move (n)), d).string ();
      }
      return ns;
    };

    // $relative(<paths>, <dir-path>)
    // $path.relative(<untyped>, <dir-path>)
    //
    // Return the path relative to the specified directory that is equivalent
    // to the specified path (or a list of relative paths for a list of
    // specified paths). Issue diagnostics and fail if a relative path cannot
    // be derived (for example, paths are on different drives on Windows).
    //
    // Note: to check if a path if relative, use `$path.absolute()`.
    //
    f["relative"] += [](path p, dir_path d)
    {
      return relative (p, d);
    };

    f["relative"] += [](paths v, dir_path d)
    {
      for (path& p: v)
        p = relative (p, d);
      return v;
    };

    f["relative"] += [](dir_paths v, dir_path d)
    {
      for (dir_path& p: v)
        p = relative (p, d);
      return v;
    };

    f[".relative"] += [](names ns, dir_path d)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of (potentially
      // mixed) paths.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir = relative (n.dir, d);
        else
          n.value = relative (convert<path> (move (n)), d).string ();
      }
      return ns;
    };

    // $base(<paths>)
    // $path.base(<untyped>)
    //
    // Return the base part (without the extension) of a path (or a list of
    // base parts for a list of paths).
    //
    f["base"] += &path::base;

    f["base"] += [](paths v)
    {
      for (path& p: v)
        p = p.base ();
      return v;
    };

    f["base"] += [](dir_paths v)
    {
      for (dir_path& p: v)
        p = p.base ();
      return v;
    };

    f[".base"] += [](names ns)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of (potentially
      // mixed) paths.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir = n.dir.base ();
        else
          n.value = convert<path> (move (n)).base ().string ();
      }
      return ns;
    };

    // $extension(<path>)
    // $path.extension(<untyped>)
    //
    // Return the extension part (without the dot) of a path or empty string
    // if there is no extension.
    //
    f["extension"] += &extension;

    f[".extension"] += [](names ns)
    {
      return extension (convert<path> (move (ns)));
    };

    // $complete(<paths>)
    // $path.complete(<untyped>)
    //
    // Complete the path (or list of paths) by prepending the current working
    // directory unless the path is already absolute.
    //
    f["complete"] += [](path p)     {p.complete (); return p;};
    f["complete"] += [](dir_path p) {p.complete (); return p;};

    f["complete"] += [](paths v)
    {
      for (auto& p: v)
        p.complete ();
      return v;
    };

    f["complete"] += [](dir_paths v)
    {
      for (auto& p: v)
        p.complete ();
      return v;
    };

    f[".complete"] += [](names ns)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of (potentially
      // mixed) paths.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir.complete ();
        else
          n.value = convert<path> (move (n)).complete ().string ();
      }
      return ns;
    };

    // $canonicalize(<paths>)
    // $path.canonicalize(<untyped>)
    //
    // Canonicalize the path (or list of paths) by converting all the
    // directory separators to the canonical form for the host platform. Note
    // that multiple directory separators are not collapsed.
    //
    f["canonicalize"] += [](path p)     {p.canonicalize (); return p;};
    f["canonicalize"] += [](dir_path p) {p.canonicalize (); return p;};

    f["canonicalize"] += [](paths v)
    {
      for (auto& p: v)
        p.canonicalize ();
      return v;
    };

    f["canonicalize"] += [](dir_paths v)
    {
      for (auto& p: v)
        p.canonicalize ();
      return v;
    };

    f[".canonicalize"] += [](names ns)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of (potentially
      // mixed) paths.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir.canonicalize ();
        else
          n.value = convert<path> (move (n)).canonicalize ().string ();
      }
      return ns;
    };

    // $normalize(<paths>)
    // $path.normalize(<untyped>)
    // $try_normalize(<path>)
    // $path.try_normalize(<untyped>)
    //
    // Normalize the path (or list of paths) by collapsing the `.` and `..`
    // components if possible, collapsing multiple directory separators, and
    // converting all the directory separators to the canonical form for the
    // host platform.
    //
    // If the resulting path would be invalid, the `$normalize()` version
    // issues diagnostics and fails while the `$try_normalize()` version
    // returns `null`. Note that `$try_normalize()` only accepts a single
    // path.
    //
    f["normalize"] += [](path p)     {p.normalize (); return p;};
    f["normalize"] += [](dir_path p) {p.normalize (); return p;};

    f["normalize"] += [](paths v)
    {
      for (auto& p: v)
        p.normalize ();
      return v;
    };

    f["normalize"] += [](dir_paths v)
    {
      for (auto& p: v)
        p.normalize ();
      return v;
    };

    f[".normalize"] += [](names ns)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of (potentially
      // mixed) paths.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir.normalize ();
        else
          n.value = convert<path> (move (n)).normalize ().string ();
      }
      return ns;
    };

    f["try_normalize"] += [](path p)
    {
      return try_normalize (p) ? value (move (p)) : value (nullptr);
    };

    f["try_normalize"] += [](dir_path p)
    {
      return try_normalize (p) ? value (move (p)) : value (nullptr);
    };

    f[".try_normalize"] += [](names ns)
    {
      if (ns.size () != 1)
        throw invalid_argument ("multiple paths");

      name& n (ns.front ());

      bool r;
      if (n.directory ())
        r = try_normalize (n.dir);
      else
      {
        path p (convert<path> (move (n)));
        if ((r = try_normalize (p)))
          n.value = move (p).string ();
      }

      return r ? value (move (ns)) : value (nullptr);
    };

    // $actualize(<paths>)
    // $path.actualize(<untyped>)
    // $try_actualize(<path>)
    // $path.try_actualize(<untyped>)
    //
    // Actualize the path (or list of paths) by first normalizing it and then
    // for host platforms with case-insensitive filesystems obtaining the
    // actual spelling of the path.
    //
    // Only an absolute path can be actualized. If a path component does not
    // exist, then its (and all subsequent) spelling is unchanged. Note that
    // this is a potentially expensive operation.
    //
    // If the resulting path would be invalid or in case of filesystem errors
    // (other than non-existent component), the `$actualize()` version issues
    // diagnostics and fails while the `$try_actualize()` version returns
    // `null`. Note that `$try_actualize()` only accepts a single path.
    //
    // Note that this function is not pure.
    //
    {
      auto e (f.insert ("actualize", false));

      e += [](path p)     {p.normalize (true); return p;};
      e += [](dir_path p) {p.normalize (true); return p;};

      e += [](paths v)
      {
        for (auto& p: v)
          p.normalize (true);
        return v;
      };

      e += [](dir_paths v)
      {
        for (auto& p: v)
          p.normalize (true);
        return v;
      };
    }

    f.insert (".actualize", false) += [](names ns)
    {
      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of (potentially
      // mixed) paths.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir.normalize (true);
        else
          n.value = convert<path> (move (n)).normalize (true).string ();
      }
      return ns;
    };

    {
      auto e (f.insert ("try_actualize", false));

      e += [](path p)
      {
        return try_actualize (p) ? value (move (p)) : value (nullptr);
      };

      e += [](dir_path p)
      {
        return try_actualize (p) ? value (move (p)) : value (nullptr);
      };
    }

    f.insert (".try_actualize", false) += [](names ns)
    {
      if (ns.size () != 1)
        throw invalid_argument ("multiple paths");

      name& n (ns.front ());

      bool r;
      if (n.directory ())
        r = try_actualize (n.dir);
      else
      {
        path p (convert<path> (move (n)));
        if ((r = try_actualize (p)))
          n.value = move (p).string ();
      }

      return r ? value (move (ns)) : value (nullptr);
    };


    // Note that we currently do not expose realize(). For one, it might be
    // tricky to handle CWD overrides (on POSIX we just call realize(3)).
    // Also, our implementation for Windows currently does not handle
    // symlinks.


    // $size(<paths>)
    // $size(<path>)
    //
    // First form: return the number of elements in the paths sequence.
    //
    // Second form: return the number of characters (bytes) in the path. Note
    // that for `dir_path` the result does not include the trailing directory
    // separator (except for the POSIX root directory).
    //
    //
    f["size"] += [] (paths v) {return v.size ();};
    f["size"] += [] (dir_paths v) {return v.size ();};

    f["size"] += [] (path v) {return v.size ();};
    f["size"] += [] (dir_path v) {return v.size ();};

    // $sort(<paths>[, <flags>])
    //
    // Sort paths in ascending order. Note that on host platforms with a
    // case-insensitive filesystem the order is case-insensitive.
    //
    // The following flags are supported:
    //
    //     dedup - in addition to sorting also remove duplicates
    //
    f["sort"] += [](paths v, optional<names> fs)
    {
      sort (v.begin (), v.end ());

      if (functions_sort_flags (move (fs)))
        v.erase (unique (v.begin(), v.end()), v.end ());

      return v;
    };

    f["sort"] += [](dir_paths v, optional<names> fs)
    {
      sort (v.begin (), v.end ());

      if (functions_sort_flags (move (fs)))
        v.erase (unique (v.begin(), v.end()), v.end ());

      return v;
    };

    // $find(<paths>, <path>)
    //
    // Return true if the paths sequence contains the specified path. Note
    // that on host platforms with a case-insensitive filesystem the
    // comparison is case-insensitive.
    //
    f["find"] += [](paths vs, value v)
    {
      return find (vs.begin (), vs.end (),
                   convert<path> (move (v))) != vs.end ();
    };

    f["find"] += [](dir_paths vs, value v)
    {
      return find (vs.begin (), vs.end (),
                   convert<dir_path> (move (v))) != vs.end ();
    };

    // $find_index(<paths>, <path>)
    //
    // Return the index of the first element in the paths sequence that is
    // equal to the specified path or `$size(paths)` if none is found. Note
    // that on host platforms with a case-insensitive filesystem the
    // comparison is case-insensitive.
    //
    f["find_index"] += [](paths vs, value v)
    {
      auto i (find (vs.begin (), vs.end (), convert<path> (move (v))));
      return i != vs.end () ? i - vs.begin () : vs.size ();
    };

    f["find_index"] += [](dir_paths vs, value v)
    {
      auto i (find (vs.begin (), vs.end (), convert<dir_path> (move (v))));
      return i != vs.end () ? i - vs.begin () : vs.size ();
    };

    // $path.match(<entry>, <pattern>[, <start-dir>])
    //
    // Match a filesystem entry name against a name pattern (both are
    // strings), or a filesystem entry path against a path pattern. For the
    // latter case the start directory may also be required (see below). The
    // pattern is a shell-like wildcard pattern. The semantics of the
    // <pattern> and <entry> arguments is determined according to the
    // following rules:
    //
    // 1. The arguments must be of the string or path types, or be untyped.
    //
    // 2. If one of the arguments is typed, then the other one must be of the
    // same type or be untyped. In the later case, an untyped argument is
    // converted to the type of the other argument.
    //
    // 3. If both arguments are untyped and the start directory is specified,
    // then the arguments are converted to the path type.
    //
    // 4. If both arguments are untyped and the start directory is not
    // specified, then, if one of the arguments is syntactically a path (the
    // value contains a directory separator), then they are converted to the
    // path type, otherwise -- to the string type (match as names).
    //
    // If pattern and entry paths are both either absolute or relative and not
    // empty, and the first pattern component is not a self-matching wildcard
    // (doesn't contain `***`), then the start directory is not required, and
    // is ignored if specified. Otherwise, the start directory must be
    // specified and be an absolute path.
    //

    // Name matching.
    //
    f[".match"] += [](string name, string pattern)
    {
      return path_match (name, pattern);
    };

    // Path matching.
    //
    f["match"] += [](path ent, path pat, optional<dir_path> start)
    {
      return path_match (ent, pat, start);
    };

    f["match"] += [](path ent, names pat, optional<names> start)
    {
      return path_match (ent,
                         convert<path> (move (pat)),
                         start
                         ? convert<dir_path> (move (*start))
                         : optional<dir_path> ());
    };

    f["match"] += [](names ent, path pat, optional<names> start)
    {
      return path_match (convert<path> (move (ent)),
                         pat,
                         start
                         ? convert<dir_path> (move (*start))
                         : optional<dir_path> ());
    };

    // The semantics depends on the presence of the start directory or the
    // first two argument syntactic representation.
    //
    f[".match"] += [](names ent, names pat, optional<names> start)
    {
      auto path_arg = [] (const names& a) -> bool
      {
        return a.size () == 1 &&
        (a[0].directory () ||
         a[0].value.find_first_of (path::traits_type::directory_separators) !=
         string::npos);
      };

      return start || path_arg (pat) || path_arg (ent)
        ? path_match (convert<path> (move (ent)),   // Match as paths.
                      convert<path> (move (pat)),
                      start
                      ? convert<dir_path> (move (*start))
                      : optional<dir_path> ())
        : path_match (convert<string> (move (ent)), // Match as strings.
                      convert<string> (move (pat)));
    };

    // Path-specific overloads from builtins.
    //
    function_family b (m, "builtin", &path_thunk);

    // Note that while we should normally handle NULL values (relied upon by
    // the parser to provide concatenation semantics consistent with untyped
    // values), the result will unlikely be what the user expected, especially
    // if the NULL value is on the LHS. So for now we keep it a bit tighter.
    //
    b[".concat"] += &concat_path_string;
    b[".concat"] += &concat_dir_path_string;

    b[".concat"] += [](path l, names ur)
    {
      return concat_path_string (move (l), convert<string> (move (ur)));
    };

    b[".concat"] += [](dir_path l, names ur)
    {
      return concat_dir_path_string (move (l), convert<string> (move (ur)));
    };

    b[".concat"] += [](dir_path l, dir_path r)
    {
      return value (move (l /= r));
    };

    b[".concat"] += [](dir_path l, path r)
    {
      return value (path_cast<path> (move (l)) /= r);
    };
  }
}
