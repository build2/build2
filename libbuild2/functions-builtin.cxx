// file      : libbuild2/functions-builtin.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <sstream>

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  builtin_functions (function_map& m)
  {
    function_family f (m, "builtin");

    // Note that we may want to extend the scope argument to a more general
    // notion of "lookup context" (scope, target, prerequisite).
    //
    f["defined"] = [](const scope* s, names name)
    {
      if (s == nullptr)
        fail << "defined() called out of scope" << endf;

      return (*s)[convert<string> (move (name))].defined ();
    };

    // Return variable visibility if it has been entered and NULL otherwise.
    //
    f["visibility"] = [](const scope* s, names name)
    {
      if (s == nullptr)
        fail << "visibility() called out of scope" << endf;

      const variable* var (
        s->ctx.var_pool.find (convert<string> (move (name))));

      return (var != nullptr
              ? optional<string> (to_string (var->visibility))
              : nullopt);
    };

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
    // Return NULL if the environment variable is not set, untyped value
    // otherwise.
    //
    f["getenv"] = [](names name)
    {
      optional<string> v (getenv (convert<string> (move (name))));

      if (!v)
        return value ();

      names r;
      r.emplace_back (to_name (move (*v)));
      return value (move (r));
    };
  }
}
