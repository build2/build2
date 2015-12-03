// file      : build/bin/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/bin/module>

#include <build/scope>
#include <build/variable>
#include <build/diagnostics>

#include <build/config/utility>
#include <build/install/utility>

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
    static const strings exe_lib {"shared", "static"};
    static const strings liba_lib {"static"};
    static const strings libso_lib {"shared"};

    extern "C" bool
    bin_init (scope& r,
              scope& b,
              const location&,
              std::unique_ptr<module>&,
              bool first,
              bool)
    {
      tracer trace ("bin::init");
      level5 ([&]{trace << "for " << b.out_path ();});

      // Register target types.
      //
      {
        auto& t (b.target_types);

        t.insert<obja>  ();
        t.insert<objso> ();
        t.insert<obj>   ();
        t.insert<exe>   ();
        t.insert<liba>  ();
        t.insert<libso> ();
        t.insert<lib>   ();
      }

      // Register rules.
      //
      {
        auto& r (b.rules);

        r.insert<obj> (perform_update_id, "bin.obj", obj_);
        r.insert<obj> (perform_clean_id, "bin.obj", obj_);

        r.insert<lib> (perform_update_id, "bin.lib", lib_);
        r.insert<lib> (perform_clean_id, "bin.lib", lib_);

        // Configure member.
        //
        r.insert<lib> (configure_update_id, "bin.lib", lib_);

        //@@ Should we check if the install module was loaded
        //   (by checking if install operation is registered
        //   for this project)? If we do that, then install
        //   will have to be loaded before bin. Perhaps we
        //   should enforce loading of all operation-defining
        //   modules before all others?
        //
        r.insert<lib> (perform_install_id, "bin.lib", lib_);
      }

      // Enter module variables.
      //
      if (first)
      {
        auto& v (var_pool);

        v.find ("config.bin.lib", string_type);
        v.find ("config.bin.exe.lib", strings_type);
        v.find ("config.bin.liba.lib", strings_type);
        v.find ("config.bin.libso.lib", strings_type);

        v.find ("bin.lib", string_type);
        v.find ("bin.exe.lib", strings_type);
        v.find ("bin.liba.lib", strings_type);
        v.find ("bin.libso.lib", strings_type);
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
        value& v (b.assign ("bin.lib"));
        if (!v)
          v = required (r, "config.bin.lib", "both").first;
      }

      // config.bin.exe.lib
      //
      {
        value& v (b.assign ("bin.exe.lib"));
        if (!v)
          v = required (r, "config.bin.exe.lib", exe_lib).first;
      }

      // config.bin.liba.lib
      //
      {
        value& v (b.assign ("bin.liba.lib"));
        if (!v)
          v = required (r, "config.bin.liba.lib", liba_lib).first;
      }

      // config.bin.libso.lib
      //
      {
        value& v (b.assign ("bin.libso.lib"));
        if (!v)
          v = required (r, "config.bin.libso.lib", libso_lib).first;
      }

      // Configure "installability" of our target types.
      //
      install::path<exe> (b, dir_path ("bin"));  // Install into install.bin.

      // Should shared libraries have executable bit? That depends on
      // who you ask. In Debian, for example, it should not unless, it
      // really is executable (i.e., has main()). On the other hand, on
      // some systems, this may be required in order for the dynamic
      // linker to be able to load the library. So, by default, we will
      // keep it executable, especially seeing that this is also the
      // behavior of autotools. At the same time, it is easy to override
      // this, for example:
      //
      // config.install.lib.mode=644
      //
      // And a library that wants to override any such overrides (e.g.,
      // because it does have main()) can do:
      //
      // libso{foo}: install.mode=755
      //
      // Everyone is happy then?
      //
      install::path<libso> (b, dir_path ("lib")); // Install into install.lib.

      install::path<liba> (b, dir_path ("lib"));  // Install into install.lib.
      install::mode<liba> (b, "644");

      return true;
    }
  }
}
