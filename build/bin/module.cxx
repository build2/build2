// file      : build/bin/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/bin/module>

#include <build/scope>
#include <build/variable>
#include <build/diagnostics>

#include <build/config/utility>

#include <build/bin/rule>
#include <build/bin/target>

using namespace std;

namespace build
{
  namespace bin
  {
    static obj_rule obj_;
    static lib_rule lib_;

    // Default config.bin.*.lib values.
    //
    static const list_value exe_lib (names {name ("shared"), name ("static")});
    static const list_value liba_lib ("static");
    static const list_value libso_lib ("shared");

    extern "C" void
    bin_init (scope& root,
              scope& base,
              const location& l,
              std::unique_ptr<module>&,
              bool)
    {
      tracer trace ("bin::init");
      level4 ([&]{trace << "for " << base.path ();});

      // Register target types.
      //
      {
        auto& tts (base.target_types);
        tts.insert<obja>  ();
        tts.insert<objso> ();
        tts.insert<obj>   ();
        tts.insert<exe>   ();
        tts.insert<liba>  ();
        tts.insert<libso> ();
        tts.insert<lib>   ();
      }

      // Register rules.
      //
      {
        auto& rs (base.rules);

        rs.insert<obj> (default_id, "bin.obj", obj_);
        rs.insert<obj> (update_id, "bin.obj", obj_);
        rs.insert<obj> (clean_id, "bin.obj", obj_);

        rs.insert<lib> (default_id, "bin.lib", lib_);
        rs.insert<lib> (update_id, "bin.lib", lib_);
        rs.insert<lib> (clean_id, "bin.lib", lib_);
      }

      // Configure.
      //
      using config::required;

      // The idea here is as follows: if we already have one of
      // the bin.* variables set, then we assume this is static
      // project configuration and don't bother setting the
      // corresponding config.bin.* variable.
      //
      //@@ Need to validate the values. Would be more efficient
      //   to do it once on assignment than every time on query.
      //   Custom var type?
      //

      // config.bin.lib
      //
      {
        auto v (base.assign ("bin.lib"));
        if (!v)
          v = required (root, "config.bin.lib", "shared").first;
      }

      // config.bin.exe.lib
      //
      {
        auto v (base.assign ("bin.exe.lib"));
        if (!v)
          v = required (root, "config.bin.exe.lib", exe_lib).first;
      }

      // config.bin.liba.lib
      //
      {
        auto v (base.assign ("bin.liba.lib"));
        if (!v)
          v = required (root, "config.bin.liba.lib", liba_lib).first;
      }

      // config.bin.libso.lib
      //
      {
        auto v (base.assign ("bin.libso.lib"));
        if (!v)
          v = required (root, "config.bin.libso.lib", libso_lib).first;
      }
    }
  }
}
