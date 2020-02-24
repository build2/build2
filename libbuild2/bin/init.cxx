// file      : build2/bin/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bin/init.hxx>

#include <map>

#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/test/module.hxx>

#include <libbuild2/install/rule.hxx>
#include <libbuild2/install/utility.hxx>

#include <libbuild2/bin/rule.hxx>
#include <libbuild2/bin/guess.hxx>
#include <libbuild2/bin/target.hxx>
#include <libbuild2/bin/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace bin
  {
    static const fail_rule fail_;
    static const lib_rule lib_;

    // Default config.bin.*.lib values.
    //
    static const strings exe_lib {"shared", "static"};
    static const strings liba_lib {"static", "shared"};
    static const strings libs_lib {"shared", "static"};

    bool
    vars_init (scope& rs,
               scope&,
               const location&,
               bool first,
               bool,
               module_init_extra&)
    {
      tracer trace ("bin::vars_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      // Enter variables. Note: some overridable, some not.
      //
      // Target is a string and not target_triplet because it can be
      // specified by the user.
      //
      auto& vp (rs.var_pool ());

      const auto vis_tgt (variable_visibility::target);
      const auto vis_prj (variable_visibility::project);

      vp.insert<string>    ("config.bin.target",   true);
      vp.insert<string>    ("config.bin.pattern",  true);

      // Library types to build.
      //
      vp.insert<string>    ("config.bin.lib",      true);

      // Library types to use (in priority order).
      //
      vp.insert<strings>   ("config.bin.exe.lib",  true);
      vp.insert<strings>   ("config.bin.liba.lib", true);
      vp.insert<strings>   ("config.bin.libs.lib", true);

      // The rpath[_link].auto flag controls automatic rpath behavior, for
      // example, addition of rpaths for prerequisite libraries (see the cc
      // module for an example). Default is true.
      //
      vp.insert<dir_paths> ("config.bin.rpath",      true);
      vp.insert<bool>      ("config.bin.rpath.auto", true);

      vp.insert<dir_paths> ("config.bin.rpath_link",      true);
      vp.insert<bool>      ("config.bin.rpath_link.auto", true);

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
      vp.insert<bool>      ("bin.rpath.auto");

      vp.insert<dir_paths> ("bin.rpath_link");
      vp.insert<bool>      ("bin.rpath_link.auto");

      // Link whole archive. Note: non-overridable with target visibility.
      //
      // The lookup semantics is as follows: we first look for a prerequisite-
      // specific value, then for a target-specific value in the library being
      // linked, and then for target type/pattern-specific value starting from
      // the scope of the target being linked-to. In that final lookup we do
      // not look in the target being linked-to itself since that is used to
      // indicate how this target should be linked to other targets. For
      // example:
      //
      // exe{test}: liba{foo}
      // liba{foo}: libua{foo1 foo2}
      // liba{foo}: bin.whole = false # Affects test but not foo1 and foo2.
      //
      // If unspecified, defaults to false for liba{} and to true for libu*{}.
      //
      vp.insert<bool>      ("bin.whole", vis_tgt);

      vp.insert<string>    ("bin.exe.prefix");
      vp.insert<string>    ("bin.exe.suffix");
      vp.insert<string>    ("bin.lib.prefix");
      vp.insert<string>    ("bin.lib.suffix");

      // The optional custom clean patterns should be just the pattern stem,
      // without the library prefix/name or extension. For example, `-[A-Z]`
      // instead of `libfoo-[A-Z].so`. Note that the custom version pattern is
      // only used for platform-independent versions (for platforms-specific
      // versions we can always derive the pattern automatically).
      //
      vp.insert<string> ("bin.lib.load_suffix", vis_prj);
      vp.insert<string> ("bin.lib.load_suffix_pattern", vis_prj);

      vp.insert<map<string, string>> ("bin.lib.version", vis_prj);
      vp.insert<string>              ("bin.lib.version_pattern", vis_prj);

      return true;
    }

    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 bool first,
                 bool,
                 module_init_extra& extra)
    {
      tracer trace ("bin::config_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (&rs != &bs)
        fail (loc) << "bin.config module must be loaded in project root";

      // Load bin.vars.
      //
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
        value& v (rs.assign ("bin.lib"));
        if (!v)
          v = *required (rs, "config.bin.lib", "both").first;
      }

      // config.bin.exe.lib
      //
      {
        value& v (rs.assign ("bin.exe.lib"));
        if (!v)
          v = *required (rs, "config.bin.exe.lib", exe_lib).first;
      }

      // config.bin.liba.lib
      //
      {
        value& v (rs.assign ("bin.liba.lib"));
        if (!v)
          v = *required (rs, "config.bin.liba.lib", liba_lib).first;
      }

      // config.bin.libs.lib
      //
      {
        value& v (rs.assign ("bin.libs.lib"));
        if (!v)
          v = *required (rs, "config.bin.libs.lib", libs_lib).first;
      }

      // config.bin.rpath[_link]
      //
      // These ones are optional and we merge them into bin.rpath[_link], if
      // any.
      //
      rs.assign ("bin.rpath") += cast_null<dir_paths> (
        optional (rs, "config.bin.rpath"));

      rs.assign ("bin.rpath_link") += cast_null<dir_paths> (
        optional (rs, "config.bin.rpath_link"));

      // config.bin.rpath[_link].auto
      //
      {
        lookup l;

        rs.assign ("bin.rpath.auto") =
          (l = omitted (rs, "config.bin.rpath.auto").first)
          ? cast<bool> (l)
          : true;

        rs.assign ("bin.rpath_link.auto") =
          (l = omitted (rs, "config.bin.rpath_link.auto").first)
          ? cast<bool> (l)
          : true;
      }

      // config.bin.{lib,exe}.{prefix,suffix}
      //
      // These ones are not used very often so we will omit them from the
      // config.build if not specified. We also override any existing value
      // that might have been specified before loading the module.
      //
      {
        lookup p (omitted (rs, "config.bin.prefix").first);
        lookup s (omitted (rs, "config.bin.suffix").first);

        auto set = [&rs] (const char* bv, const char* cv, lookup l)
        {
          if (lookup o = omitted (rs, cv).first)
            l = o;

          if (l)
            rs.assign (bv) = *l;
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
          const variable& var (rs.ctx.var_pool["config.bin.target"]);

          // We first see if the value was specified via the configuration
          // mechanism.
          //
          auto p (omitted (rs, var));
          lookup l (p.first);

          // Then see if there is a config hint (e.g., from the cc module).
          //
          bool hint (false);
          if (!l)
          {
            if (auto hl = extra.hints[var])
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
          if (!hint && config_sub)
          {
            s = run<string> (3,
                             *config_sub,
                             s.c_str (),
                             [] (string& l, bool) {return move (l);});
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
          const variable& var (rs.ctx.var_pool["config.bin.pattern"]);

          // We first see if the value was specified via the configuration
          // mechanism.
          //
          auto p (omitted (rs, var));
          lookup l (p.first);

          // Then see if there is a config hint (e.g., from the C++ module).
          //
          if (!l)
          {
            if (auto hl = extra.hints[var])
              l = hl;
          }

          // For ease of use enter it as bin.pattern (since it can come from
          // different places).
          //
          if (l)
          {
            const string& s (cast<string> (l));

            if (s.empty () ||
                (!path::traits_type::is_separator (s.back ()) &&
                 s.find ('*') == string::npos))
            {
              fail << "missing '*' or trailing '"
                   << char (path::traits_type::directory_separator)
                   << "' in binutils pattern '" << s << "'";
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

          dr << "bin " << project (rs) << '@' << rs << '\n'
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
          bool first,
          bool,
          module_init_extra& extra)
    {
      tracer trace ("bin::init");
      l5 ([&]{trace << "for " << bs;});

      // Load bin.config.
      //
      load_module (rs, rs, "bin.config", loc, extra.hints);

      // Cache some config values we will be needing below.
      //
      const string& tclass (cast<string> (rs["bin.target.class"]));

      // Register target types and configure their default "installability".
      //
      bool install_loaded (cast_false<bool> (rs["install.loaded"]));
      {
        using namespace install;

        if (first)
        {
          rs.insert_target_type<obj>  ();
          rs.insert_target_type<obje> ();
          rs.insert_target_type<obja> ();
          rs.insert_target_type<objs> ();

          rs.insert_target_type<bmi>  ();
          rs.insert_target_type<bmie> ();
          rs.insert_target_type<bmia> ();
          rs.insert_target_type<bmis> ();

          rs.insert_target_type<hbmi>  ();
          rs.insert_target_type<hbmie> ();
          rs.insert_target_type<hbmia> ();
          rs.insert_target_type<hbmis> ();

          rs.insert_target_type<libul> ();
          rs.insert_target_type<libue> ();
          rs.insert_target_type<libua> ();
          rs.insert_target_type<libus> ();

          rs.insert_target_type<lib>  ();
          rs.insert_target_type<liba> ();
          rs.insert_target_type<libs> ();

          // Register the def{} target type. Note that we do it here since it
          // is input and can be specified unconditionally (i.e., not only
          // when building for Windows).
          //
          rs.insert_target_type<def> ();
        }

        // Note: libu*{} members are not installable.
        //
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
          if (first)
            rs.insert_target_type<libi> ();

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

        r.insert<obj> (perform_update_id, "bin.obj", fail_);
        r.insert<obj> (perform_clean_id,  "bin.obj", fail_);

        r.insert<bmi> (perform_update_id, "bin.bmi", fail_);
        r.insert<bmi> (perform_clean_id,  "bin.bmi", fail_);

        r.insert<hbmi> (perform_update_id, "bin.hbmi", fail_);
        r.insert<hbmi> (perform_clean_id,  "bin.hbmi", fail_);

        r.insert<libul> (perform_update_id, "bin.libul", fail_);
        r.insert<libul> (perform_clean_id,  "bin.libul", fail_);

        // Similar to alias.
        //

        //@@ outer
        r.insert<lib> (perform_id,   0, "bin.lib", lib_);
        r.insert<lib> (configure_id, 0, "bin.lib", lib_);

        // Treat as a see through group for install, test, and dist.
        //
        if (install_loaded)
        {
          auto& gr (install::group_rule::instance);

          r.insert<lib> (perform_install_id,   "bin.lib", gr);
          r.insert<lib> (perform_uninstall_id, "bin.lib", gr);
        }

        if (const test::module* m = rs.find_module<test::module> ("test"))
        {
          r.insert<lib> (perform_test_id, "bin.lib", m->group_rule ());
        }

        if (rs.find_module ("dist"))
        {
          r.insert<lib> (dist_id, 0, "bin.lib", lib_);
        }
      }

      return true;
    }

    bool
    ar_config_init (scope& rs,
                    scope& bs,
                    const location& loc,
                    bool first,
                    bool,
                    module_init_extra& extra)
    {
      tracer trace ("bin::ar_config_init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure bin.config is loaded.
      //
      load_module (rs, bs, "bin.config", loc, extra.hints);

      // Enter configuration variables.
      //
      if (first)
      {
        auto& vp (rs.var_pool ());

        vp.insert<process_path> ("bin.ar.path");
        vp.insert<process_path> ("bin.ranlib.path");

        vp.insert<path>         ("config.bin.ar", true);
        vp.insert<path>         ("config.bin.ranlib", true);
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
        const string& tsys (cast<string> (rs["bin.target.system"]));
        const char* ar_d (tsys == "win32-msvc" ? "lib" : "ar");

        // This can be either a pattern or search path(s).
        //
        pattern_paths pat (lookup_pattern (rs));

        // Don't save the default value to config.build so that if the user
        // changes, say, the C++ compiler (which hinted the pattern), then
        // ar will automatically change as well.
        //
        auto ap (
          config::required (
            rs,
            "config.bin.ar",
            path (apply_pattern (ar_d, pat.pattern)),
            false,
            config::save_commented));

        auto rp (
          config::required (
            rs,
            "config.bin.ranlib",
            nullptr,
            false,
            config::save_commented));

        const path& ar (cast<path> (ap.first));
        const path* ranlib (cast_null<path> (rp.first));

        ar_info ari (guess_ar (ar, ranlib, pat.paths));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (ap.second || rp.second ? 2 : 3))
        {
          diag_record dr (text);

          {
            dr << "bin.ar " << project (rs) << '@' << rs << '\n'
               << "  ar         " << ari.ar_path << '\n'
               << "  id         " << ari.ar_id << '\n'
               << "  version    " << ari.ar_version.string () << '\n'
               << "  major      " << ari.ar_version.major << '\n'
               << "  minor      " << ari.ar_version.minor << '\n'
               << "  patch      " << ari.ar_version.patch << '\n';
          }

          if (!ari.ar_version.build.empty ())
          {
            dr << "  build      " << ari.ar_version.build << '\n';
          }

          {
            dr << "  signature  " << ari.ar_signature << '\n'
               << "  checksum   " << ari.ar_checksum;
          }

          if (ranlib != nullptr)
          {
            dr << '\n'
               << "  ranlib     " << ari.ranlib_path << '\n'
               << "  id         " << ari.ranlib_id << '\n'
               << "  signature  " << ari.ranlib_signature << '\n'
               << "  checksum   " << ari.ranlib_checksum;
          }
        }

        rs.assign<process_path> ("bin.ar.path")      = move (ari.ar_path);
        rs.assign<string>       ("bin.ar.id")        = move (ari.ar_id);
        rs.assign<string>       ("bin.ar.signature") = move (ari.ar_signature);
        rs.assign<string>       ("bin.ar.checksum")  = move (ari.ar_checksum);

        {
          semantic_version& v (ari.ar_version);

          rs.assign<string>   ("bin.ar.version")       = v.string ();
          rs.assign<uint64_t> ("bin.ar.version.major") = v.major;
          rs.assign<uint64_t> ("bin.ar.version.minor") = v.minor;
          rs.assign<uint64_t> ("bin.ar.version.patch") = v.patch;
          rs.assign<string>   ("bin.ar.version.build") = move (v.build);
        }

        if (ranlib != nullptr)
        {
          rs.assign<process_path> ("bin.ranlib.path") = move (ari.ranlib_path);
          rs.assign<string>       ("bin.ranlib.id")   = move (ari.ranlib_id);
          rs.assign<string>       ("bin.ranlib.signature") =
            move (ari.ranlib_signature);
          rs.assign<string>       ("bin.ranlib.checksum") =
            move (ari.ranlib_checksum);
        }
      }

      return true;
    }

    bool
    ar_init (scope& rs,
             scope& bs,
             const location& loc,
             bool,
             bool,
             module_init_extra& extra)
    {
      tracer trace ("bin::ar_init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure the bin core and ar.config are loaded.
      //
      load_module (rs, bs, "bin",           loc, extra.hints);
      load_module (rs, bs, "bin.ar.config", loc, extra.hints);

      return true;
    }

    bool
    ld_config_init (scope& rs,
                    scope& bs,
                    const location& loc,
                    bool first,
                    bool,
                    module_init_extra& extra)
    {
      tracer trace ("bin::ld_config_init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure bin.config is loaded.
      //
      load_module (rs, rs, "bin.config", loc, extra.hints);

      // Enter configuration variables.
      //
      if (first)
      {
        auto& vp (rs.var_pool ());

        vp.insert<process_path> ("bin.ld.path");
        vp.insert<path>         ("config.bin.ld", true);
      }

      // Configure.
      //
      if (first)
      {
        // config.bin.ld
        //
        // Use the target to decide on the default ld name.
        //
        const string& tsys (cast<string> (rs["bin.target.system"]));
        const char* ld_d (tsys == "win32-msvc" ? "link" : "ld");

        // This can be either a pattern or search path(s).
        //
        pattern_paths pat (lookup_pattern (rs));

        auto p (
          config::required (
            rs,
            "config.bin.ld",
            path (apply_pattern (ld_d, pat.pattern)),
            false,
            config::save_commented));

        const path& ld (cast<path> (p.first));
        ld_info ldi (guess_ld (ld, pat.paths));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          diag_record dr (text);

          {
            dr << "bin.ld " << project (rs) << '@' << rs << '\n'
               << "  ld         " << ldi.path << '\n'
               << "  id         " << ldi.id << '\n';
          }

          if (ldi.version)
          {
            dr << "  version    " << ldi.version->string () << '\n'
               << "  major      " << ldi.version->major << '\n'
               << "  minor      " << ldi.version->minor << '\n'
               << "  patch      " << ldi.version->patch << '\n';
          }

          if (ldi.version && !ldi.version->build.empty ())
          {
            dr << "  build      " << ldi.version->build << '\n';
          }

          dr << "  signature  " << ldi.signature << '\n'
             << "  checksum   " << ldi.checksum;
        }

        rs.assign<process_path> ("bin.ld.path")      = move (ldi.path);
        rs.assign<string>       ("bin.ld.id")        = move (ldi.id);
        rs.assign<string>       ("bin.ld.signature") = move (ldi.signature);
        rs.assign<string>       ("bin.ld.checksum")  = move (ldi.checksum);

        if (ldi.version)
        {
          semantic_version& v (*ldi.version);

          rs.assign<string>   ("bin.ld.version")       = v.string ();
          rs.assign<uint64_t> ("bin.ld.version.major") = v.major;
          rs.assign<uint64_t> ("bin.ld.version.minor") = v.minor;
          rs.assign<uint64_t> ("bin.ld.version.patch") = v.patch;
          rs.assign<string>   ("bin.ld.version.build") = move (v.build);
        }
      }

      return true;
    }

    bool
    ld_init (scope& rs,
             scope& bs,
             const location& loc,
             bool,
             bool,
             module_init_extra& extra)
    {
      tracer trace ("bin::ld_init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure the bin core and ld.config are loaded.
      //
      load_module (rs, bs, "bin",           loc, extra.hints);
      load_module (rs, bs, "bin.ld.config", loc, extra.hints);

      const string& lid (cast<string> (rs["bin.ld.id"]));

      // Register the pdb{} target if using the VC toolchain.
      //
      using namespace install;

      if (lid == "msvc")
      {
        const target_type& pdb (bs.derive_target_type<file> ("pdb").first);
        install_path (bs, pdb, dir_path ("bin")); // Goes to install.bin
        install_mode (bs, pdb, "644");            // But not executable.
      }

      return true;
    }

    bool
    rc_config_init (scope& rs,
                    scope& bs,
                    const location& loc,
                    bool first,
                    bool,
                    module_init_extra& extra)
    {
      tracer trace ("bin::rc_config_init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure bin.config is loaded.
      //
      load_module (rs, bs, "bin.config", loc, extra.hints);

      // Enter configuration variables.
      //
      if (first)
      {
        auto& vp (rs.var_pool ());

        vp.insert<process_path> ("bin.rc.path");
        vp.insert<path>         ("config.bin.rc", true);
      }

      // Configure.
      //
      if (first)
      {
        // config.bin.rc
        //
        // Use the target to decide on the default rc name.
        //
        const string& tsys (cast<string> (rs["bin.target.system"]));
        const char* rc_d (tsys == "win32-msvc" ? "rc" : "windres");

        // This can be either a pattern or search path(s).
        //
        pattern_paths pat (lookup_pattern (rs));

        auto p (
          config::required (
            rs,
            "config.bin.rc",
            path (apply_pattern (rc_d, pat.pattern)),
            false,
            config::save_commented));

        const path& rc (cast<path> (p.first));
        rc_info rci (guess_rc (rc, pat.paths));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          text << "bin.rc " << project (rs) << '@' << rs << '\n'
               << "  rc         " << rci.path << '\n'
               << "  id         " << rci.id << '\n'
               << "  signature  " << rci.signature << '\n'
               << "  checksum   " << rci.checksum;
        }

        rs.assign<process_path> ("bin.rc.path")      = move (rci.path);
        rs.assign<string>       ("bin.rc.id")        = move (rci.id);
        rs.assign<string>       ("bin.rc.signature") = move (rci.signature);
        rs.assign<string>       ("bin.rc.checksum")  = move (rci.checksum);
      }

      return true;
    }

    bool
    rc_init (scope& rs,
             scope& bs,
             const location& loc,
             bool,
             bool,
             module_init_extra& extra)
    {
      tracer trace ("bin::rc_init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure the bin core and rc.config are loaded.
      //
      load_module (rs, bs, "bin",           loc, extra.hints);
      load_module (rs, bs, "bin.rc.config", loc, extra.hints);

      return true;
    }

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"bin.vars",      nullptr, vars_init},
      {"bin.config",    nullptr, config_init},
      {"bin",           nullptr, init},
      {"bin.ar.config", nullptr, ar_config_init},
      {"bin.ar",        nullptr, ar_init},
      {"bin.ld.config", nullptr, ld_config_init},
      {"bin.ld",        nullptr, ld_init},
      {"bin.rc.config", nullptr, rc_config_init},
      {"bin.rc",        nullptr, rc_init},
      {nullptr,         nullptr, nullptr}
    };

    const module_functions*
    build2_bin_load ()
    {
      return mod_functions;
    }
  }
}
