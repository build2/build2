// file      : libbuild2/functions-builtin.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <sstream>

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  // Return NULL value if an environment variable is not set, untyped value
  // otherwise.
  //
  static inline value
  getenvvar (const string& name)
  {
    optional<string> v (getenv (name));

    if (!v)
      return value ();

    names r;
    r.emplace_back (to_name (move (*v)));
    return value (move (r));
  }

  void
  builtin_functions (function_map& m)
  {
    function_family f (m, "builtin");

    f["type"] = [](value* v) {return v->type != nullptr ? v->type->name : "";};

    f["null"]  = [](value* v) {return v->null;};
    f["empty"] = [](value* v)  {return v->null || v->empty ();};

    f["identity"] = [](value* v) {return move (*v);};

    // string
    //
    f["string"] = [](bool b) {return b ? "true" : "false";};
    f["string"] = [](uint64_t i) {return to_string (i);};
    f["string"] = [](name n) {return to_string (n);};

    // Quote a value returning its string representation. If escape is true,
    // then also escape (with a backslash) the quote characters being added
    // (this is useful if the result will be re-parsed, for example as a
    // Testscript command line).
    //
    f["quote"] = [](value* v, optional<value> escape)
    {
      if (v->null)
        return string ();

      untypify (*v); // Reverse to names.

      ostringstream os;
      to_stream (os,
                 v->as<names> (),
                 true /* quote */,
                 '@'  /* pair */,
                 escape && convert<bool> (move (*escape)));
      return os.str ();
    };

    // getenv
    //
    f["getenv"] = [](string name)
    {
      return getenvvar (name);
    };

    f["getenv"] = [](names name)
    {
      return getenvvar (convert<string> (move (name)));
    };
  }
}
