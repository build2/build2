// file      : build/bin/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/bin/module>

#include <build/scope>
#include <build/variable>
#include <build/diagnostics>

#include <build/config/utility>

using namespace std;

namespace build
{
  namespace bin
  {
    // Default config.bin.*.lib values.
    //
    static const list_value exe_lib (names {name ("shared"), name ("static")});
    static const list_value liba_lib ("static");
    static const list_value libso_lib ("shared");

    void
    init (scope& root, scope& base, const location& l)
    {
      //@@ TODO: avoid multiple inits (generally, for modules).
      //
      tracer trace ("bin::init");

      //@@ Should it be this way?
      //
      if (&root != &base)
        fail (l) << "bin module must be initialized in project root scope";

      //@@ TODO: need to register target types, rules here instead of main().

      const dir_path& out_root (root.path ());
      level4 ([&]{trace << "for " << out_root;});

      // Configure.
      //
      using config::required;

      //@@ Need to validate the values. Would be more efficient
      //   to do it once on assignment than every time on query.
      //   Custom var type?
      //

      // config.bin.lib
      //
      {
        auto v (root.vars.assign ("bin.lib"));
        if (!v)
          v = required (root, "config.bin.lib", "shared").first;
      }

      // config.bin.exe.lib
      //
      {
        auto v (root.vars.assign ("bin.exe.lib"));
        if (!v)
          v = required (root, "config.bin.exe.lib", exe_lib).first;
      }

      // config.bin.liba.lib
      //
      {
        auto v (root.vars.assign ("bin.liba.lib"));
        if (!v)
          v = required (root, "config.bin.liba.lib", liba_lib).first;
      }

      // config.bin.libso.lib
      //
      {
        auto v (root.vars.assign ("bin.libso.lib"));
        if (!v)
          v = required (root, "config.bin.libso.lib", libso_lib).first;
      }
    }
  }
}
