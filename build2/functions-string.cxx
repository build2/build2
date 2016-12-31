// file      : build2/functions-string.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function>
#include <build2/variable>

using namespace std;

namespace build2
{
  void
  string_functions ()
  {
    function_family f ("string");

    f["string"] = [](string s)  {return s;};

    // @@ Shouldn't it concatenate elements into the single string?
    // @@ Doesn't seem to be used so far. Can consider removing.
    //
    // f["string"] = [](strings v) {return v;};

    // String-specific overloads from builtins.
    //
    function_family b ("builtin");

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
