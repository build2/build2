// file      : libbuild2/functions-string.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  string_functions (function_map& m)
  {
    function_family f (m, "string");

    f["string"] = [](string s)  {return s;};

    // @@ Shouldn't it concatenate elements into the single string?
    // @@ Doesn't seem to be used so far. Can consider removing.
    //
    // f["string"] = [](strings v) {return v;};

    // Compare ASCII strings ignoring case and returning the boolean value.
    //
    f["icasecmp"] = [](string x, string y)
    {
      return icasecmp (x, y) == 0;
    };

    f["icasecmp"] = [](string x, names y)
    {
      return icasecmp (x, convert<string> (move (y))) == 0;
    };

    f["icasecmp"] = [](names x, string y)
    {
      return icasecmp (convert<string> (move (x)), y) == 0;
    };

    f[".icasecmp"] = [](names x, names y)
    {
      return icasecmp (convert<string> (move (x)),
                       convert<string> (move (y))) == 0;
    };

    // String-specific overloads from builtins.
    //
    function_family b (m, "builtin");

    b[".concat"] = [](string l, string r) {l += r; return l;};

    b[".concat"] = [](string l, names ur)
    {
      l += convert<string> (move (ur));
      return l;
    };

    b[".concat"] = [](names ul, string r)
    {
      string l (convert<string> (move (ul)));
      l += r;
      return l;
    };
  }
}
