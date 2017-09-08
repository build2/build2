// file      : build2/functions-path.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function.hxx>
#include <build2/variable.hxx>

using namespace std;

namespace build2
{
  static value
  path_thunk (const scope& base,
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
    if (path::traits::is_separator (sr[0])) // '\0' if empty.
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
    if (path::traits::is_separator (sr[0])) // '\0' if empty.
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
