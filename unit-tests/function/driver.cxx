// file      : unit-tests/function/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <iostream>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/parser.hxx>
#include <build2/context.hxx>
#include <build2/function.hxx>
#include <build2/variable.hxx>
#include <build2/diagnostics.hxx>

using namespace std;

namespace build2
{
  static const optional<const value_type*> arg_bool[1] =
  {
    &value_traits<bool>::value_type
  };

  int
  main (int, char* argv[])
  {
    init (argv[0], 1);  // Fake build system driver, default verbosity.
    reset (strings ()); // No command line variables.

    function_family f ("dummy");

    f["fail"]     = []()        {fail << "failed" << endf;};
    f["fail_arg"] = [](names a) {return convert<uint64_t> (move (a[0]));};

    f["nullable"] = [](names* a)          {return a == nullptr;};
    f["optional"] = [](optional<names> a) {return !a;};

    f["dummy0"] = []()         {return "abc";};
    f["dummy1"] = [](string s) {return s;};
    f["dummy2"] = [](uint64_t x, uint64_t y) {return x + y;};

    f["ambig"] = [](names a, optional<string>)   {return a;};
    f["ambig"] = [](names a, optional<uint64_t>) {return a;};

    f[".qual"] = []() {return "abc";};

    f[".length"] = &path::size; // Member function.
    f[".type"]   = &name::type; // Data member.

    f[".abs"] = [](dir_path d) {return d.absolute ();};

    // Variadic function with first required argument of type bool. Returns
    // number of arguments passed.
    //
    functions.insert (
      "variadic",
      function_overload (
        nullptr,
        1,
        function_overload::arg_variadic,
        function_overload::types (arg_bool, 1),
        [] (vector_view<value> args, const function_overload&)
        {
          return value (static_cast<uint64_t> (args.size ()));
        }));

    // Dump arguments.
    //
    functions.insert (
      "dump",
      function_overload (
        nullptr,
        0,
        function_overload::arg_variadic,
        function_overload::types (),
        [] (vector_view<value> args, const function_overload&)
        {
          for (value& a: args)
          {
            if (a.null)
              cout << "[null]";
            else if (!a.empty ())
            {
              names storage;
              cout << reverse (a, storage);
            }
            cout << endl;
          }
          return value (nullptr);
        }));

    try
    {
      scope& s (*scope::global_);

      parser p;
      p.parse_buildfile (cin, path ("buildfile"), s, s);
    }
    catch (const failed&)
    {
      return 1;
    }

    return 0;
  }
}

int
main (int argc, char* argv[])
{
  return build2::main (argc, argv);
}
