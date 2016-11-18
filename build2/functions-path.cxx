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
    error << "invalid path: '" << e.path << "'";
    throw failed ();
  }

  void
  path_functions ()
  {
    function_family f ("path", &path_thunk);

    // normalize
    //
    f["normalize"] = [](path p) {p.normalize (); return p;};
    f["normalize"] = [](paths v) {for (auto& p: v) p.normalize (); return v;};

    f["normalize"] = [](dir_path p) {p.normalize (); return p;};
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
  }
}
