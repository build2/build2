// file      : libbuild2/functions-project-name.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  project_name_functions (function_map& m)
  {
    function_family f (m, "project_name");

    // $string(<project-name>)
    //
    // Return the string representation of a project name. See also the
    // `$variable()` function below.
    //

    // Note that we must handle NULL values (relied upon by the parser
    // to provide conversion semantics consistent with untyped values).
    //
    f["string"] += [](project_name* p)
    {
      return p != nullptr ? move (*p).string () : string ();
    };

    // $base(<project-name>[, <extension>])
    //
    // Return the base part (without the extension) of a project name.
    //
    // If <extension> is specified, then only remove that extension. Note that
    // <extension> should not include the dot and the comparison is always
    // case-insensitive.
    //
    f["base"] += [](project_name p, optional<string> ext)
    {
      return ext ? p.base (ext->c_str ()) : p.base ();
    };

    f["base"] += [](project_name p, names ext)
    {
      return p.base (convert<string> (move (ext)).c_str ());
    };

    // $extension(<project-name>)
    //
    // Return the extension part (without the dot) of a project name or empty
    // string if there is no extension.
    //
    f["extension"] += &project_name::extension;

    // $variable(<project-name>)
    //
    // Return the string representation of a project name that is sanitized to
    // be usable as a variable name. Specifically, `.`, `-`, and `+` are
    // replaced with `_`.
    //
    f["variable"]  += &project_name::variable;

    // Project name-specific overloads from builtins.
    //
    function_family b (m, "builtin");

    // Note that while we should normally handle NULL values (relied upon by
    // the parser to provide concatenation semantics consistent with untyped
    // values), the result will unlikely be what the user expected. So for now
    // we keep it a bit tighter.
    //
    b[".concat"] += [](project_name n, string s)
    {
      string r (move (n).string ());
      r += s;
      return r;
    };

    b[".concat"] += [](string s, project_name n)
    {
      s += n.string ();
      return s;
    };

    b[".concat"] += [](project_name n, names ns)
    {
      string r (move (n).string ());
      r += convert<string> (move (ns));
      return r;
    };

    b[".concat"] += [](names ns, project_name n)
    {
      string r (convert<string> (move (ns)));
      r += n.string ();
      return r;
    };
  }
}
