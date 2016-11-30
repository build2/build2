// file      : build2/functions-path.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function>
#include <build2/variable>

using namespace std;

namespace build2
{
  static value
  path_thunk (vector_view<value> args, const function_overload& f)
  try
  {
    return function_family::default_thunk (move (args), f);
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
    {
      sr.erase (0, 1);
      path pr (move (sr));
      pr.canonicalize (); // Convert to canonical directory separators.

      // If RHS is syntactically a directory (ends with a trailing slash),
      // then return it as dir_path, not path.
      //
      if (pr.to_directory () || pr.empty ())
        l /= path_cast<dir_path> (move (pr));
      else
        return value (path_cast<path> (move (l)) /= pr);
    }
    else
      l += sr;

    return value (move (l));
  }

  void
  path_functions ()
  {
    function_family f ("path", &path_thunk);

    // string
    //
    f["string"] = [](path p)     {return move (p).string ();};
    f["string"] = [](dir_path p) {return move (p).string ();};

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

    // normalize
    //
    f["normalize"] = [](path p) {p.normalize (); return p;};
    f["normalize"] = [](dir_path p) {p.normalize (); return p;};

    f["normalize"] = [](paths v) {for (auto& p: v) p.normalize (); return v;};
    f["normalize"] = [](dir_paths v) {for (auto& p: v) p.normalize (); return v;};

    f[".normalize"] = [](names ns)
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
