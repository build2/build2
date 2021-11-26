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

  void
  path_functions (function_map& m)
  {
    function_family f (m, "path", &path_thunk);

    // string
    //
    f["string"] += [](path p) {return move (p).string ();};

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

    // directory
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

    // leaf
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
    // Sort paths in ascending order. Note that on case-insensitive filesystem
    // the order is case-insensitive.
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
  }
}
