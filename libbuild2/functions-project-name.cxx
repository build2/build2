// file      : libbuild2/functions-project-name.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  project_name_functions ()
  {
    function_family f ("project_name");

    f["string"]    = [](project_name p) {return move (p).string ();};

    f["base"] = [](project_name p, optional<string> ext)
    {
      return ext ? p.base (ext->c_str ()) : p.base ();
    };

    f["base"] = [](project_name p, names ext)
    {
      return p.base (convert<string> (move (ext)).c_str ());
    };

    f["extension"] = &project_name::extension;
    f["variable"]  = &project_name::variable;

    // Project name-specific overloads from builtins.
    //
    function_family b ("builtin");

    b[".concat"] = [](project_name n, string s)
    {
      string r (move (n).string ());
      r += s;
      return r;
    };

    b[".concat"] = [](string s, project_name n)
    {
      s += n.string ();
      return s;
    };

    b[".concat"] = [](project_name n, names ns)
    {
      string r (move (n).string ());
      r += convert<string> (move (ns));
      return r;
    };

    b[".concat"] = [](names ns, project_name n)
    {
      string r (convert<string> (move (ns)));
      r += n.string ();
      return r;
    };
  }
}
