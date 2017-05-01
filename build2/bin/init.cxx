// file      : build2/bin/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/init.hxx>

#include <map>

#include <build2/scope.hxx>
#include <build2/variable.hxx>
#include <build2/diagnostics.hxx>

#include <build2/config/utility.hxx>
#include <build2/install/utility.hxx>

#include <build2/bin/rule.hxx>
#include <build2/bin/guess.hxx>
#include <build2/bin/target.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace bin
  {
    static const obj_rule obj_;
    static const lib_rule lib_;

    // Default config.bin.*.lib values.
    //
    static const strings exe_lib {"shared", "static"};
    static const strings liba_lib {"static", "shared"};
    static const strings libs_lib {"shared", "static"};

    bool
    vars_init (scope& r,
               scope&,
               const location&,
               unique_ptr<module_base>&,
               bool first,
               bool,
               const variable_map&)
    {
      tracer trace ("cc::core_vars_init");
      l5 ([&]{trace << "for " << r.out_path ();});

      assert (first);

      // Enter variables. Note: some overridable, some not.
      //
      // Target is a string and not target_triplet because it can be
      // specified by the user.
      //
      auto& vp (var_pool.rw (r));

      vp.insert<string>    ("config.bin.target",   true);
      vp.insert<string>    ("config.bin.pattern",  true);

      vp.insert<string>    ("config.bin.lib",      true);
      vp.insert<strings>   ("config.bin.exe.lib",  true);
      vp.insert<strings>   ("config.bin.liba.lib", true);
      vp.insert<strings>   ("config.bin.libs.lib", true);
      vp.insert<dir_paths> ("config.bin.rpath",    true);

      vp.insert<string>    ("config.bin.prefix", true);
      vp.insert<string>    ("config.bin.suffix", true);
      vp.insert<string>    ("config.bin.lib.prefix", true);
      vp.insert<string>    ("config.bin.lib.suffix", true);
      vp.insert<string>    ("config.bin.exe.prefix", true);
      vp.insert<string>    ("config.bin.exe.suffix", true);

      vp.insert<string>    ("bin.lib");
      vp.insert<strings>   ("bin.exe.lib");
      vp.insert<strings>   ("bin.liba.lib");
      vp.insert<strings>   ("bin.libs.lib");
      vp.insert<dir_paths> ("bin.rpath");

      vp.insert<string>    ("bin.lib.prefix");
      vp.insert<string>    ("bin.lib.suffix");
      vp.insert<string>    ("bin.exe.prefix");
      vp.insert<string>    ("bin.exe.suffix");

      vp.insert<map<string, string>> ("bin.lib.version",
                                      variable_visibility::project);

      return true;
    }

    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 unique_ptr<module_base>&,
                 bool first,
                 bool,
                 const variable_map& hints)
    {
      tracer trace ("bin::config_init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      // Load bin.vars.
      //
      if (!cast_false<bool> (rs["bin.vars.loaded"]))
        load_module (rs, rs, "bin.vars", loc);

      // Configure.
      //
      using config::required;
      using config::optional;
      using config::omitted;

      // Adjust module priority (binutils).
      //
      config::save_module (rs, "bin", 350);

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
        value& v (bs.assign ("bin.lib"));
        if (!v)
          v = *required (rs, "config.bin.lib", "both").first;
      }

      // config.bin.exe.lib
      //
      {
        value& v (bs.assign ("bin.exe.lib"));
        if (!v)
          v = *required (rs, "config.bin.exe.lib", exe_lib).first;
      }

      // config.bin.liba.lib
      //
      {
        value& v (bs.assign ("bin.liba.lib"));
        if (!v)
          v = *required (rs, "config.bin.liba.lib", liba_lib).first;
      }

      // config.bin.libs.lib
      //
      {
        value& v (bs.assign ("bin.libs.lib"));
        if (!v)
          v = *required (rs, "config.bin.libs.lib", libs_lib).first;
      }

      // config.bin.rpath
      //
      // This one is optional and we merge it into bin.rpath, if any.
      // See the cxx module for details on merging.
      //
      bs.assign ("bin.rpath") += cast_null<dir_paths> (
        optional (rs, "config.bin.rpath"));

      // config.bin.{lib,exe}.{prefix,suffix}
      //
      // These ones are not used very often so we will omit them from the
      // config.build if not specified. We also override any existing value
      // that might have been specified before loading the module.
      //
      {
        lookup p (omitted (rs, "config.bin.prefix").first);
        lookup s (omitted (rs, "config.bin.suffix").first);

        auto set = [&rs, &bs] (const char* bv, const char* cv, lookup l)
        {
          if (lookup o = omitted (rs, cv).first)
            l = o;

          if (l)
            bs.assign (bv) = *l;
        };

        set ("bin.lib.prefix", "config.bin.lib.prefix", p);
        set ("bin.lib.suffix", "config.bin.lib.suffix", s);

        set ("bin.exe.prefix", "config.bin.exe.prefix", p);
        set ("bin.exe.suffix", "config.bin.exe.suffix", s);
      }

      if (first)
      {
        bool new_val (false); // Set any new values?

        // config.bin.target
        //
        {
          const variable& var (var_pool["config.bin.target"]);

          // We first see if the value was specified via the configuration
          // mechanism.
          //
          auto p (omitted (rs, var));
          lookup l (p.first);

          // Then see if there is a config hint (e.g., from the C++ module).
          //
          bool hint (false);
          if (!l)
          {
            if (auto hl = hints[var])
            {
              l = hl;
              hint = true;
            }
          }

          if (!l)
            fail (loc) << "unable to determine binutils target" <<
              info << "consider specifying it with " << var <<
              info << "or first load a module that can provide it as a hint, "
                       << "such as c or cxx";

          // Split/canonicalize the target.
          //
          string s (cast<string> (l));

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
            target_triplet t (s);

            l5 ([&]{trace << "canonical target: '" << t.string () << "'; "
                          << "class: " << t.class_;});

            assert (!hint || s == t.string ());

            // Also enter as bin.target.{cpu,vendor,system,version,class}
            // for convenience of access.
            //
            rs.assign<string> ("bin.target.cpu")     = t.cpu;
            rs.assign<string> ("bin.target.vendor")  = t.vendor;
            rs.assign<string> ("bin.target.system")  = t.system;
            rs.assign<string> ("bin.target.version") = t.version;
            rs.assign<string> ("bin.target.class")   = t.class_;

            rs.assign<target_triplet> ("bin.target") = move (t);
          }
          catch (const invalid_argument& e)
          {
            // This is where we suggest that the user specifies --config-sub
            // to help us out.
            //
            fail << "unable to parse binutils target '" << s << "': " << e <<
              info << "consider using the --config-sub option";
          }

          new_val = new_val || p.second; // False for a hinted value.
        }

        // config.bin.pattern
        //
        {
          const variable& var (var_pool["config.bin.pattern"]);

          // We first see if the value was specified via the configuration
          // mechanism.
          //
          auto p (omitted (rs, var));
          lookup l (p.first);

          // Then see if there is a config hint (e.g., from the C++ module).
          //
          if (!l)
          {
            if (auto hl = hints[var])
              l = hl;
          }

          // For ease of use enter it as bin.pattern (since it can come from
          // different places).
          //
          if (l)
          {
            const string& s (cast<string> (l));

            if (s.empty () ||
                (!path::traits::is_separator (s.back ()) &&
                 s.find ('*') == string::npos))
            {
              fail << "missing '*' in binutils pattern '" << s << "'";
            }

            rs.assign<string> ("bin.pattern") = s;
            new_val = new_val || p.second; // False for a hinted value.
          }
        }

        // If we set any new values (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (new_val ? 2 : 3))
        {
          diag_record dr (text);

          dr << "bin " << project (rs) << '@' << rs.out_path () << '\n'
             << "  target     " << cast<target_triplet> (rs["bin.target"]);

          if (auto l = rs["bin.pattern"])
            dr << '\n'
               << "  pattern    " << cast<string> (l);
        }
      }

      return true;
    }

    bool
    init (scope& rs,
          scope& bs,
          const location& loc,
          unique_ptr<module_base>&,
          bool,
          bool,
          const variable_map& hints)
    {
      tracer trace ("bin::init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      // Load bin.config.
      //
      if (!cast_false<bool> (bs["bin.config.loaded"]))
        load_module (rs, bs, "bin.config", loc, false, hints);

      // Cache some config values we will be needing below.
      //
      const string& tclass (cast<string> (rs["bin.target.class"]));

      // Register target types and configure their default "installability".
      //
      bool install_loaded (cast_false<bool> (rs["install.loaded"]));

      {
        using namespace install;

        auto& t (bs.target_types);

        t.insert<obj>  ();
        t.insert<obje> ();
        t.insert<obja> ();
        t.insert<objs> ();

        t.insert<lib>  ();
        t.insert<liba> ();
        t.insert<libs> ();

        if (install_loaded)
        {
          install_path<liba> (bs, dir_path ("lib")); // Install in install.lib.
          install_mode<liba> (bs, "644");
        }

        // Should shared libraries have the executable bit? That depends on
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
        // libs{foo}: install.mode=755
        //
        // Everyone is happy then? On Windows libs{} is the DLL and goes to
        // bin/, not lib/.
        //
        if (install_loaded)
          install_path<libs> (bs,
                              dir_path (tclass == "windows" ? "bin" : "lib"));

        // Create additional target types for certain targets.
        //
        if (tclass == "windows")
        {
          // Import library.
          //
          t.insert<libi> ();

          if (install_loaded)
          {
            install_path<libi> (bs, dir_path ("lib"));
            install_mode<libi> (bs, "644");
          }
        }
      }

      // Register rules.
      //
      {
        auto& r (bs.rules);

        r.insert<obj> (perform_update_id, "bin.obj", obj_);
        r.insert<obj> (perform_clean_id, "bin.obj", obj_);

        r.insert<lib> (perform_update_id, "bin.lib", lib_);
        r.insert<lib> (perform_clean_id, "bin.lib", lib_);

        // Configure member.
        //
        r.insert<lib> (configure_update_id, "bin.lib", lib_);

        if (install_loaded)
        {
          r.insert<lib> (perform_install_id, "bin.lib", lib_);
          r.insert<lib> (perform_uninstall_id, "bin.lib", lib_);
        }
      }

      return true;
    }

    bool
    ar_config_init (scope& r,
                    scope& b,
                    const location& loc,
                    unique_ptr<module_base>&,
                    bool first,
                    bool,
                    const variable_map& hints)
    {
      tracer trace ("bin::ar_config_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Make sure bin.config is loaded.
      //
      if (!cast_false<bool> (b["bin.config.loaded"]))
        load_module (r, b, "bin.config", loc, false, hints);

      // Enter configuration variables.
      //
      if (first)
      {
        auto& v (var_pool.rw (r));

        v.insert<process_path> ("bin.rc.path");
        v.insert<process_path> ("bin.ranlib.path");

        v.insert<path>         ("config.bin.ar", true);
        v.insert<path>         ("config.bin.ranlib", true);
      }

      // Configure.
      //
      if (first)
      {
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

        // Use the target to decide on the default binutils program names.
        //
        const string& tsys (cast<string> (r["bin.target.system"]));
        const char* ar_d (tsys == "win32-msvc" ? "lib" : "ar");

        // This can be either a pattern or a fallback search directory.
        //
        const string* pat (cast_null<string> (r["bin.pattern"]));
        bool fb (pat != nullptr && path::traits::is_separator (pat->back ()));

        // Don't save the default value to config.build so that if the user
        // changes, say, the C++ compiler (which hinted the pattern), then
        // ar will automatically change as well.
        //
        auto ap (
          config::required (
            r,
            "config.bin.ar",
            path (apply_pattern (ar_d, fb ? nullptr : pat)),
            false,
            config::save_commented));

        auto rp (
          config::required (
            r,
            "config.bin.ranlib",
            nullptr,
            false,
            config::save_commented));

        const path& ar (cast<path> (ap.first));
        const path* ranlib (cast_null<path> (rp.first));

        ar_info ari (
          guess_ar (ar, ranlib, fb ? dir_path (*pat) : dir_path ()));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (ap.second || rp.second ? 2 : 3))
        {
          diag_record dr (text);

          dr << "bin.ar " << project (r) << '@' << r.out_path () << '\n'
             << "  ar         " << ari.ar_path << '\n'
             << "  id         " << ari.ar_id << '\n'
             << "  signature  " << ari.ar_signature << '\n'
             << "  checksum   " << ari.ar_checksum;

          if (ranlib != nullptr)
          {
            dr << '\n'
               << "  ranlib     " << ari.ranlib_path << '\n'
               << "  id         " << ari.ranlib_id << '\n'
               << "  signature  " << ari.ranlib_signature << '\n'
               << "  checksum   " << ari.ranlib_checksum;
          }
        }

        r.assign<process_path> ("bin.ar.path")      = move (ari.ar_path);
        r.assign<string>       ("bin.ar.id")        = move (ari.ar_id);
        r.assign<string>       ("bin.ar.signature") = move (ari.ar_signature);
        r.assign<string>       ("bin.ar.checksum")  = move (ari.ar_checksum);

        if (ranlib != nullptr)
        {
          r.assign<process_path> ("bin.ranlib.path") = move (ari.ranlib_path);
          r.assign<string>       ("bin.ranlib.id")   = move (ari.ranlib_id);
          r.assign<string>       ("bin.ranlib.signature") =
            move (ari.ranlib_signature);
          r.assign<string>       ("bin.ranlib.checksum") =
            move (ari.ranlib_checksum);
        }
      }

      return true;
    }

    bool
    ar_init (scope& r,
             scope& b,
             const location& loc,
             unique_ptr<module_base>&,
             bool,
             bool,
             const variable_map& hints)
    {
      tracer trace ("bin::ar_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Make sure the bin core and ar.config are loaded.
      //
      if (!cast_false<bool> (b["bin.loaded"]))
        load_module (r, b, "bin", loc, false, hints);

      if (!cast_false<bool> (b["bin.ar.config.loaded"]))
        load_module (r, b, "bin.ar.config", loc, false, hints);

      return true;
    }

    bool
    ld_config_init (scope& r,
                    scope& b,
                    const location& loc,
                    unique_ptr<module_base>&,
                    bool first,
                    bool,
                    const variable_map& hints)
    {
      tracer trace ("bin::ld_config_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Make sure bin.config is loaded.
      //
      if (!cast_false<bool> (b["bin.config.loaded"]))
        load_module (r, b, "bin.config", loc, false, hints);

      // Enter configuration variables.
      //
      if (first)
      {
        auto& v (var_pool.rw (r));

        v.insert<process_path> ("bin.ld.path");
        v.insert<path>         ("config.bin.ld", true);
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

        // This can be either a pattern or a fallback search directory.
        //
        const string* pat (cast_null<string> (r["bin.pattern"]));
        bool fb (pat != nullptr && path::traits::is_separator (pat->back ()));

        auto p (
          config::required (
            r,
            "config.bin.ld",
            path (apply_pattern (ld_d, fb ? nullptr : pat)),
            false,
            config::save_commented));

        const path& ld (cast<path> (p.first));
        ld_info ldi (guess_ld (ld, fb ? dir_path (*pat) : dir_path ()));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          text << "bin.ld " << project (r) << '@' << r.out_path () << '\n'
               << "  ld         " << ldi.path << '\n'
               << "  id         " << ldi.id << '\n'
               << "  signature  " << ldi.signature << '\n'
               << "  checksum   " << ldi.checksum;
        }

        r.assign<process_path> ("bin.ld.path")      = move (ldi.path);
        r.assign<string>       ("bin.ld.id")        = move (ldi.id);
        r.assign<string>       ("bin.ld.signature") = move (ldi.signature);
        r.assign<string>       ("bin.ld.checksum")  = move (ldi.checksum);
      }

      return true;
    }

    bool
    ld_init (scope& r,
             scope& b,
             const location& loc,
             unique_ptr<module_base>&,
             bool,
             bool,
             const variable_map& hints)
    {
      tracer trace ("bin::ld_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Make sure the bin core and ld.config are loaded.
      //
      if (!cast_false<bool> (b["bin.loaded"]))
        load_module (r, b, "bin", loc, false, hints);

      if (!cast_false<bool> (b["bin.ld.config.loaded"]))
        load_module (r, b, "bin.ld.config", loc, false, hints);

      const string& lid (cast<string> (r["bin.ld.id"]));

      // Register the pdb{} target if using the VC toolchain.
      //
      using namespace install;

      if (lid == "msvc")
      {
        const target_type& pdb (b.derive_target_type<file> ("pdb").first);
        install_path (b, pdb, dir_path ("bin")); // Goes to install.bin
        install_mode (b, pdb, "644");            // But not executable.
      }

      return true;
    }

    bool
    rc_config_init (scope& r,
                    scope& b,
                    const location& loc,
                    unique_ptr<module_base>&,
                    bool first,
                    bool,
                    const variable_map& hints)
    {
      tracer trace ("bin::rc_config_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Make sure bin.config is loaded.
      //
      if (!cast_false<bool> (b["bin.config.loaded"]))
        load_module (r, b, "bin.config", loc, false, hints);

      // Enter configuration variables.
      //
      if (first)
      {
        auto& v (var_pool.rw (r));

        v.insert<process_path> ("bin.rc.path");
        v.insert<path>         ("config.bin.rc", true);
      }

      // Configure.
      //
      if (first)
      {
        // config.bin.rc
        //
        // Use the target to decide on the default rc name.
        //
        const string& tsys (cast<string> (r["bin.target.system"]));
        const char* rc_d (tsys == "win32-msvc" ? "rc" : "windres");

        // This can be either a pattern or a fallback search directory.
        //
        const string* pat (cast_null<string> (r["bin.pattern"]));
        bool fb (pat != nullptr && path::traits::is_separator (pat->back ()));

        auto p (
          config::required (
            r,
            "config.bin.rc",
            path (apply_pattern (rc_d, fb ? nullptr : pat)),
            false,
            config::save_commented));

        const path& rc (cast<path> (p.first));
        rc_info rci (guess_rc (rc, fb ? dir_path (*pat) : dir_path ()));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          text << "bin.rc " << project (r) << '@' << r.out_path () << '\n'
               << "  rc         " << rci.path << '\n'
               << "  id         " << rci.id << '\n'
               << "  signature  " << rci.signature << '\n'
               << "  checksum   " << rci.checksum;
        }

        r.assign<process_path> ("bin.rc.path")      = move (rci.path);
        r.assign<string>       ("bin.rc.id")        = move (rci.id);
        r.assign<string>       ("bin.rc.signature") = move (rci.signature);
        r.assign<string>       ("bin.rc.checksum")  = move (rci.checksum);
      }

      return true;
    }

    bool
    rc_init (scope& r,
             scope& b,
             const location& loc,
             unique_ptr<module_base>&,
             bool,
             bool,
             const variable_map& hints)
    {
      tracer trace ("bin::rc_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Make sure the bin core and rc.config are loaded.
      //
      if (!cast_false<bool> (b["bin.loaded"]))
        load_module (r, b, "bin", loc, false, hints);

      if (!cast_false<bool> (b["bin.rc.config.loaded"]))
        load_module (r, b, "bin.rc.config", loc, false, hints);

      return true;
    }
  }
}
