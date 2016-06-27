// file      : build2/bin/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/module>

#include <build2/scope>
#include <build2/variable>
#include <build2/diagnostics>

#include <build2/config/utility>
#include <build2/install/utility>

#include <build2/bin/rule>
#include <build2/bin/guess>
#include <build2/bin/target>

using namespace std;

namespace build2
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

    bool
    init (scope& r,
          scope& b,
          const location&,
          unique_ptr<module_base>&,
          bool first,
          bool,
          const variable_map& config_hints)
    {
      tracer trace ("bin::init");
      l5 ([&]{trace << "for " << b.out_path ();});

      assert (config_hints.empty ()); // We don't known any hints.

      // Enter module variables.
      //
      if (first)
      {
        auto& v (var_pool);

        // Note: some overridable, some not.
        //
        v.insert<path>      ("config.bin.ar",        true);
        v.insert<path>      ("config.bin.ranlib",    true);

        v.insert<string>    ("config.bin.lib",       true);
        v.insert<strings>   ("config.bin.exe.lib",   true);
        v.insert<strings>   ("config.bin.liba.lib",  true);
        v.insert<strings>   ("config.bin.libso.lib", true);
        v.insert<dir_paths> ("config.bin.rpath",     true);

        v.insert<string>    ("bin.lib");
        v.insert<strings>   ("bin.exe.lib");
        v.insert<strings>   ("bin.liba.lib");
        v.insert<strings>   ("bin.libso.lib");
        v.insert<dir_paths> ("bin.rpath");

        v.insert<string>    ("bin.libprefix",        true);
      }

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

      // config.bin.rpath
      //
      // This one is optional and we merge it into bin.rpath, if any.
      // See the cxx module for details on merging.
      //
      b.assign ("bin.rpath") += cast_null<dir_paths> (
        config::optional (r, "config.bin.rpath"));

      // config.bin.ar
      // config.bin.ranlib
      //
      // For config.bin.ar we default to 'ar' while ranlib should be explicitly
      // specified by the user in order for us to use it (most targets support
      // the -s option to ar).
      //
      if (first)
      {
        auto p (config::required (r, "config.bin.ar", path ("ar")));
        auto& v (config::optional (r, "config.bin.ranlib"));

        const path& ar (cast<path> (p.first));
        const path& ranlib (v ? cast<path> (v) : path ());

        bin_info bi (guess (ar, ranlib));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          //@@ Print project out root or name? See cxx.

          text << ar << ":\n"
               << "  id         " << bi.ar_id << "\n"
               << "  signature  " << bi.ar_signature << "\n"
               << "  checksum   " << bi.ar_checksum;

          if (!ranlib.empty ())
          {
            text << ranlib << ":\n"
                 << "  id         " << bi.ranlib_id << "\n"
                 << "  signature  " << bi.ranlib_signature << "\n"
                 << "  checksum   " << bi.ranlib_checksum;
          }
        }

        r.assign<string> ("bin.ar.id") = move (bi.ar_id);
        r.assign<string> ("bin.ar.signature") = move (bi.ar_signature);
        r.assign<string> ("bin.ar.checksum") = move (bi.ar_checksum);

        if (!ranlib.empty ())
        {
          r.assign<string> ("bin.ranlib.id") = move (bi.ranlib_id);
          r.assign<string> ("bin.ranlib.signature") =
            move (bi.ranlib_signature);
          r.assign<string> ("bin.ranlib.checksum") = move (bi.ranlib_checksum);
        }
      }

      // Configure "installability" of our target types.
      //
      using namespace install;

      install_path<exe> (b, dir_path ("bin"));  // Install into install.bin.

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
      install_path<libso> (b, dir_path ("lib")); // Install into install.lib.

      //@@ For Windows, libso{} is an import library and shouldn't be exec.

      install_path<liba> (b, dir_path ("lib"));  // Install into install.lib.
      install_mode<liba> (b, "644");

      return true;
    }
  }
}
