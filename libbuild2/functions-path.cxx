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

  void
  path_functions (function_map& m)
  {
    function_family f (m, "path", &path_thunk);

    // string
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

    // posix_string
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

    // representation
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

    // posix_representation
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

    // canonicalize
    //
    // @@ TODO: add ability to specify alternative separator.
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

    // normalize
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

    // actualize
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

    // $directory(<path>)
    // $directory(<paths>)
    //
    // Return the directory part of the path or empty path if there is no
    // directory. Directory of a root directory is an empty path.
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

    // $root_directory(<path>)
    // $root_directory(<paths>)
    //
    // Return the root directory of the path or empty path if the directory is
    // not absolute.
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

    // $leaf(<path>)
    //
    f["leaf"] += &path::leaf;

    // $leaf(<path>, <dir-path>)
    // $leaf(<paths>, <dir-path>)
    //
    // Return the path without the specified directory part. Return empty path
    // if the paths are the same. Issue diagnostics and fail if the directory
    // is not a prefix of the path. Note: expects both paths to be normalized.
    //
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

    // $relative(<path>, <dir-path>)
    // $relative(<paths>, <dir-path>)
    //
    // Return a path relative to the specified directory that is equivalent to
    // the specified path. Issue diagnostics and fail if a relative path
    // cannot be derived (for example, paths are on different drives on
    // Windows).
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

    // base
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

    // extension
    //
    f["extension"] += &extension;

    f[".extension"] += [](names ns)
    {
      return extension (convert<path> (move (ns)));
    };

    // $size(<paths>)
    // $size(<dir_paths>)
    //
    // Return the number of elements in the sequence.
    //
    f["size"] += [] (paths v) {return v.size ();};
    f["size"] += [] (dir_paths v) {return v.size ();};

    // $size(<path>)
    // $size(<dir_path>)
    //
    // Return the number of characters (bytes) in the path. Note that for
    // dir_path the result does not include the trailing directory separator
    // (except for the POSIX root directory).
    //
    f["size"] += [] (path v) {return v.size ();};
    f["size"] += [] (dir_path v) {return v.size ();};

    // $sort(<paths> [, <flags>])
    // $sort(<dir_paths> [, <flags>])
    //
    // Sort paths in ascending order. Note that on hosts with a case-
    // insensitive filesystem the order is case-insensitive.
    //
    // The following flags are supported:
    //
    //   dedup - in addition to sorting also remove duplicates
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
    // $find(<dir_paths>, <dir_path>)
    //
    // Return true if the path sequence contains the specified path. Note that
    // on hosts with a case-insensitive filesystem the comparison is
    // case-insensitive.
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
    // $find_index(<dir_paths>, <dir_path>)
    //
    // Return the index of the first element in the path sequence that is
    // equal to the specified path or $size(<paths>) if none is found. Note
    // that on hosts with a case-insensitive filesystem the comparison is
    // case-insensitive.
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

    // $path.match(<val>, <pat> [, <start>])
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
