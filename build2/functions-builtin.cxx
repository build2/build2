// file      : build2/functions-builtin.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cstdlib> // getenv()

#include <build2/function.hxx>
#include <build2/variable.hxx>

using namespace std;

namespace build2
{
  // Return NULL value if an environment variable is not set, untyped value
  // otherwise.
  //
  static inline value
  getenv (const string& name)
  {
    const char* v (::getenv (name.c_str ()));

    if (v == nullptr)
      return value ();

    names r;
    r.emplace_back (to_name (v));
    return value (move (r));
  }

  void
  builtin_functions ()
  {
    function_family f ("builtin");

    f["type"] = [](value* v) {return v->type != nullptr ? v->type->name : "";};

    f["null"]  = [](value* v) {return v->null;};
    f["empty"] = [](value* v)  {return v->null || v->empty ();};

    f["identity"] = [](value* v) {return move (*v);};

    // string
    //
    f["string"] = [](bool b) {return b ? "true" : "false";};
    f["string"] = [](uint64_t i) {return to_string (i);};
    f["string"] = [](name n) {return to_string (n);};

    // getenv
    //
    f["getenv"] = [](string name)
    {
      return getenv (name);
    };

    f["getenv"] = [](names name)
    {
      return getenv (convert<string> (move (name)));
    };
  }
}
