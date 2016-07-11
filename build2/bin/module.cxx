// file      : build2/bin/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/module>

#include <butl/triplet>

#include <build2/scope>
#include <build2/variable>
#include <build2/diagnostics>

#include <build2/config/utility>
#include <build2/install/utility>

#include <build2/bin/rule>
#include <build2/bin/guess>
#include <build2/bin/target>

using namespace std;
using namespace butl;

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

    // Apply the specified stem to the config.bin.pattern. If there is no
    // pattern, then return the stem itself. Assume the pattern is valid,
    // i.e., contains single '*'.
    //
    static string
    apply (const lookup& pattern, const char* stem)
    {
      if (!pattern)
        return stem;

      const string& p (cast<string> (pattern));
      size_t i (p.find ('*'));
      assert (i != string::npos);

      string r (p, 0, i++);
      r.append (stem);
      r.append (p, i, p.size () - i);
      return r;
    }

    bool
    init (scope& r,
          scope& b,
          const location& loc,
          unique_ptr<module_base>&,
          bool first,
          bool,
          const variable_map& config_hints)
    {
      tracer trace ("bin::init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Enter module variables.
      //
      if (first)
      {
        auto& v (var_pool);

        // Note: some overridable, some not.
        //
        v.insert<string>    ("config.bin.target",    true);
        v.insert<string>    ("config.bin.pattern",   true);

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

      // Configure.
      //
      using config::required;
      using config::optional;

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
        optional (r, "config.bin.rpath"));

      if (first)
      {
        bool new_val (false); // Set any new values?

        // config.bin.target
        //
        {
          const variable& var (var_pool.find ("config.bin.target"));

          // We first see if the value was specified via the configuration
          // mechanism.
          //
          auto p (required (r, var));
          const value* v (p.first);

          // Then see if there is a config hint (e.g., from the C++ module).
          //
          bool hint (false);
          if (v == nullptr)
          {
            if (auto l = config_hints[var])
            {
              v = l.value;
              hint = true;
            }
          }

          if (v == nullptr)
            fail (loc) << "unable to determine binutils target" <<
              info << "consider specifying it with " << var.name <<
              info << "or first load a module that can provide it as a hint, "
                       << "such as c or cxx";

          // Split/canonicalize the target.
          //
          string s (cast<string> (*v));

          // Did the user ask us to use config.sub? If this is a hinted value,
          // then we assume it has already been passed through config.sub.
          //
          if (!hint && ops.config_sub_specified ())
          {
            s = run<string> (ops.config_sub (),
                             s.c_str (),
                             [] (string& l) {return move (l);});
            l5 ([&]{trace << "config.sub target: '" << s << "'";});
          }

          try
          {
            string canon;
            triplet t (s, canon);

            l5 ([&]{trace << "canonical target: '" << canon << "'; "
                          << "class: " << t.class_;});

            assert (!hint || s == canon);

            // Enter as bin.target.{cpu,vendor,system,version,class}.
            //
            r.assign<string> ("bin.target") = move (canon);
            r.assign<string> ("bin.target.cpu") = move (t.cpu);
            r.assign<string> ("bin.target.vendor") = move (t.vendor);
            r.assign<string> ("bin.target.system") = move (t.system);
            r.assign<string> ("bin.target.version") = move (t.version);
            r.assign<string> ("bin.target.class") = move (t.class_);
          }
          catch (const invalid_argument& e)
          {
            // This is where we suggest that the user specifies --config-sub
            // to help us out.
            //
            fail << "unable to parse binutils target '" << s << "': "
                 << e.what () <<
              info << "consider using the --config-sub option";
          }

          new_val = new_val || p.second; // False for a hinted value.
        }

        // config.bin.pattern
        //
        {
          const variable& var (var_pool.find ("config.bin.pattern"));

          // We first see if the value was specified via the configuration
          // mechanism.
          //
          auto p (required (r, var));
          const value* v (p.first);

          // Then see if there is a config hint (e.g., from the C++ module).
          //
          if (v == nullptr)
          {
            if (auto l = config_hints[var])
              v = l.value;
          }

          // For ease of use enter it as bin.pattern (it can come from
          // different places).
          //
          if (v != nullptr)
          {
            const string& s (cast<string> (*v));

            if (s.find ('*') == string::npos)
              fail << "missing '*' in binutils pattern '" << s << "'";

            r.assign<string> ("bin.pattern") = s;
            new_val = new_val || p.second; // False for a hinted value.
          }
        }

        // If we set any new values (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (new_val ? 2 : 3))
        {
          diag_record dr (text);

          dr << "bin\n"
             << "  target     " << cast<string> (r["bin.target"]);

          if (auto l = r["bin.pattern"])
            dr << '\n'
               << "  pattern    " << cast<string> (l);
        }

        // config.bin.ar
        // config.bin.ranlib
        //
        // For config.bin.ar we have the default (plus the pattern) while
        // ranlib should be explicitly specified by the user in order for us
        // to use it (all targets that we currently care to support have the
        // ar -s option but if that changes we can always force the use of
        // ranlib for certain targets).
        //
        // Another idea is to refuse to use default 'ar' (without the pattern)
        // if the host/build targets don't match. On the other hand, a cross-
        // toolchain can be target-unprefixed. Also, without canonicalization,
        // comparing targets will be unreliable.
        //
        auto pattern (r["bin.pattern"]);

        // Use the target to decide on the default binutils program names.
        //
        const string& tsys (cast<string> (r["bin.target.system"]));
        const char* ar_d (tsys == "win32-msvc" ? "lib" : "ar");

        auto p (required (r, "config.bin.ar", path (apply (pattern, ar_d))));
        auto& v (optional (r, "config.bin.ranlib"));

        const path& ar (cast<path> (p.first));
        const path& ranlib (v ? cast<path> (v) : path ());

        ar_info ai (guess_ar (ar, ranlib));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          //@@ Print project out root or name? See cxx.

          text << "bin.ar\n"
               << "  exe        " << ar << '\n'
               << "  id         " << ai.ar_id << '\n'
               << "  signature  " << ai.ar_signature << '\n'
               << "  checksum   " << ai.ar_checksum;

          if (!ranlib.empty ())
          {
            text << "bin.ranlib\n"
                 << "  exe        " << ranlib << '\n'
                 << "  id         " << ai.ranlib_id << '\n'
                 << "  signature  " << ai.ranlib_signature << '\n'
                 << "  checksum   " << ai.ranlib_checksum;
          }
        }

        r.assign<string> ("bin.ar.id") = move (ai.ar_id);
        r.assign<string> ("bin.ar.signature") = move (ai.ar_signature);
        r.assign<string> ("bin.ar.checksum") = move (ai.ar_checksum);

        if (!ranlib.empty ())
        {
          r.assign<string> ("bin.ranlib.id") = move (ai.ranlib_id);
          r.assign<string> ("bin.ranlib.signature") =
            move (ai.ranlib_signature);
          r.assign<string> ("bin.ranlib.checksum") = move (ai.ranlib_checksum);
        }
      }

      // Cache some config values we will be needing below.
      //
      const string& tclass (cast<string> (r["bin.target.class"]));

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
      // Everyone is happy then? Not Windows users. When targeting Windows
      // libso{} is an import library and shouldn't be exec.
      //
      install_path<libso> (b, dir_path ("lib")); // Install into install.lib.

      if (tclass == "windows")
        install_mode<libso> (b, "644");

      install_path<liba> (b, dir_path ("lib"));  // Install into install.lib.
      install_mode<liba> (b, "644");

      return true;
    }

    bool
    ld_init (scope& r,
             scope& b,
             const location& loc,
             unique_ptr<module_base>&,
             bool first,
             bool,
             const variable_map& config_hints)
    {
      tracer trace ("bin::ld_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Make sure the bin core is loaded.
      //
      if (!cast_false<bool> (b["bin.loaded"]))
        load_module ("bin", r, b, loc, false, config_hints);

      // Enter module variables.
      //
      if (first)
      {
        auto& v (var_pool);

        v.insert<path> ("config.bin.ld", true);
      }

      // Configure.
      //
      if (first)
      {
        // config.bin.ld
        //
        // Use the target to decide on the default ld name.
        //
        const string& tsys (cast<string> (r["bin.target.system"]));
        const char* ld_d (tsys == "win32-msvc" ? "link" : "ld");

        auto p (config::required (r,
                                  "config.bin.ld",
                                  path (apply (r["bin.pattern"], ld_d))));

        const path& ld (cast<path> (p.first));
        ld_info li (guess_ld (ld));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          text << "bin.ld\n"
               << "  exe        " << ld << '\n'
               << "  id         " << li.ld_id << '\n'
               << "  signature  " << li.ld_signature << '\n'
               << "  checksum   " << li.ld_checksum;
        }

        r.assign<string> ("bin.ld.id") = move (li.ld_id);
        r.assign<string> ("bin.ld.signature") = move (li.ld_signature);
        r.assign<string> ("bin.ld.checksum") = move (li.ld_checksum);
      }

      return true;
    }
  }
}
