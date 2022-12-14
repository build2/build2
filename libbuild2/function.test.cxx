// file      : libbuild2/function.test.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <iostream>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/parser.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/scheduler.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/file-cache.hxx>
#include <libbuild2/diagnostics.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;

namespace build2
{
  static const optional<const value_type*> arg_bool[1] =
  {
    &value_traits<bool>::value_type
  };

  static dir_path
  scoped (const scope*, dir_path d)
  {
    return d;
  }

  static void
  scoped_void (const scope*, dir_path)
  {
  }

  int
  main (int, char* argv[])
  {
    // Fake build system driver, default verbosity.
    //
    init_diag (1);
    init (nullptr, argv[0], true);

    // Serial execution.
    //
    scheduler sched (1);
    global_mutexes mutexes (1);
    file_cache fcache (true);
    context ctx (sched, mutexes, fcache);

    auto& functions (ctx.functions);

    function_family f (functions, "dummy");

    f["fail"]     += []()        {fail << "failed" << endf;};
    f["fail_arg"] += [](names a) {return convert<uint64_t> (move (a[0]));};

    f["nullable"] += [](names* a)          {return a == nullptr;};
    f["optional"] += [](optional<names> a) {return !a;};

    f["dummy0"] += []()         {return "abc";};
    f["dummy1"] += [](string s) {return s;};
    f["dummy2"] += [](uint64_t x, uint64_t y) {return x + y;};

    f["ambig"] += [](names a, optional<string>)   {return a;};
    f["ambig"] += [](names a, optional<uint64_t>) {return a;};

    f["reverse"] += [](names a) {return a;};

    f["scoped"]      += [](const scope*, names a) {return a;};
    f["scoped_void"] += [](const scope*, names) {};
    f["scoped"]      += &scoped;
    f["scoped_void"] += &scoped_void;

    f[".qual"] += []() {return "abc";};

    f[".length"] += &path::size; // Member function.
    f[".type"]   += &name::type; // Data member.

    f[".abs"] += [](dir_path d) {return d.absolute ();};

    // Variadic function with first required argument of type bool. Returns
    // number of arguments passed.
    //
    functions.insert ("variadic", true).insert (
      function_overload (
        nullptr,
        1,
        function_overload::arg_variadic,
        function_overload::types (arg_bool, 1),
        [] (const scope*, vector_view<value> args, const function_overload&)
        {
          return value (static_cast<uint64_t> (args.size ()));
        }));

    // Dump arguments.
    //
    functions.insert ("dump", true).insert (
      function_overload (
        nullptr,
        0,
        function_overload::arg_variadic,
        function_overload::types (),
        [] (const scope*, vector_view<value> args, const function_overload&)
        {
          for (value& a: args)
          {
            if (a.null)
              cout << "[null]";
            else if (!a.empty ())
            {
              names storage;
              cout << reverse (a, storage, true /* reduce */);
            }
            cout << endl;
          }
          return value (nullptr);
        }));

    try
    {
      // Use temp scope for the private variable pool.
      //
      temp_scope s (ctx.global_scope.rw ());

      parser p (ctx);
      p.parse_buildfile (cin, path_name ("buildfile"), &s, s);
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
