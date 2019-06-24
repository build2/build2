// file      : libbuild2/functions-path.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
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

  void
  path_functions ()
  {
    function_family f ("path", &path_thunk);

    // string
    //
    f["string"] = [](path p) {return move (p).string ();};

    f["string"] = [](paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (move (p).string ());
      return r;
    };

    f["string"] = [](dir_paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (move (p).string ());
      return r;
    };

    // representation
    //
    f["representation"] = [](path p) {return move (p).representation ();};

    f["representation"] = [](paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (move (p).representation ());
      return r;
    };

    f["representation"] = [](dir_paths v)
    {
      strings r;
      for (auto& p: v)
        r.push_back (move (p).representation ());
      return r;
    };

    // canonicalize
    //
    f["canonicalize"] = [](path p)     {p.canonicalize (); return p;};
    f["canonicalize"] = [](dir_path p) {p.canonicalize (); return p;};

    f["canonicalize"] = [](paths v)
    {
      for (auto& p: v)
        p.canonicalize ();
      return v;
    };

    f["canonicalize"] = [](dir_paths v)
    {
      for (auto& p: v)
        p.canonicalize ();
      return v;
    };

    f[".canonicalize"] = [](names ns)
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
    f["normalize"] = [](path p, optional<value> a)
    {
      p.normalize (a && convert<bool> (move (*a)));
      return p;
    };

    f["normalize"] = [](dir_path p, optional<value> a)
    {
      p.normalize (a && convert<bool> (move (*a)));
      return p;
    };

    f["normalize"] = [](paths v, optional<value> a)
    {
      bool act (a && convert<bool> (move (*a)));

      for (auto& p: v)
        p.normalize (act);

      return v;
    };
    f["normalize"] = [](dir_paths v, optional<value> a)
    {
      bool act (a && convert<bool> (move (*a)));

      for (auto& p: v)
        p.normalize (act);
      return v;
    };

    f[".normalize"] = [](names ns, optional<value> a)
    {
      bool act (a && convert<bool> (move (*a)));

      // For each path decide based on the presence of a trailing slash
      // whether it is a directory. Return as untyped list of (potentially
      // mixed) paths.
      //
      for (name& n: ns)
      {
        if (n.directory ())
          n.dir.normalize (act);
        else
          n.value = convert<path> (move (n)).normalize (act).string ();
      }
      return ns;
    };

    // directory
    //
    f["directory"] = &path::directory;

    f["directory"] = [](paths v)
    {
      dir_paths r;
      for (const path& p: v)
        r.push_back (p.directory ());
      return r;
    };

    f["directory"] = [](dir_paths v)
    {
      for (dir_path& p: v)
        p = p.directory ();
      return v;
    };

    f[".directory"] = [](names ns)
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

    // base
    //
    f["base"] = &path::base;

    f["base"] = [](paths v)
    {
      for (path& p: v)
        p = p.base ();
      return v;
    };

    f["base"] = [](dir_paths v)
    {
      for (dir_path& p: v)
        p = p.base ();
      return v;
    };

    f[".base"] = [](names ns)
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

    // leaf
    //
    f["leaf"] = &path::leaf;

    f["leaf"] = [](path p, dir_path d)
    {
      return leaf (p, move (d));
    };

    f["leaf"] = [](paths v, optional<dir_path> d)
    {
      for (path& p: v)
        p = leaf (p, d);
      return v;
    };

    f["leaf"] = [](dir_paths v, optional<dir_path> d)
    {
      for (dir_path& p: v)
        p = leaf (p, d);
      return v;
    };

    f[".leaf"] = [](names ns, optional<dir_path> d)
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

    // extension
    //
    f["extension"] = &extension;

    f[".extension"] = [](names ns)
    {
      return extension (convert<path> (move (ns)));
    };

    // Path-specific overloads from builtins.
    //
    function_family b ("builtin", &path_thunk);

    b[".concat"] = &concat_path_string;
    b[".concat"] = &concat_dir_path_string;

    b[".concat"] = [](path l, names ur)
    {
      return concat_path_string (move (l), convert<string> (move (ur)));
    };

    b[".concat"] = [](dir_path l, names ur)
    {
      return concat_dir_path_string (move (l), convert<string> (move (ur)));
    };
  }
}
