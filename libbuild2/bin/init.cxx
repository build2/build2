// file      : libbuild2/bin/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bin/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/test/module.hxx>

#include <libbuild2/install/rule.hxx>
#include <libbuild2/install/utility.hxx>

#include <libbuild2/bin/rule.hxx>
#include <libbuild2/bin/def-rule.hxx>
#include <libbuild2/bin/guess.hxx>
#include <libbuild2/bin/target.hxx>
#include <libbuild2/bin/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace bin
  {
    static const obj_rule obj_;
    static const libul_rule libul_;
    static const lib_rule lib_;
    static const def_rule def_;

    // Default config.bin.*.lib values.
    //
    static const strings exe_lib {"shared", "static"};
    static const strings liba_lib {"static", "shared"};
    static const strings libs_lib {"shared", "static"};

    bool
    vars_init (scope& rs,
               scope& bs,
               const location& loc,
               bool,
               bool,
               module_init_extra&)
    {
      tracer trace ("bin::vars_init");
      l5 ([&]{trace << "for " << rs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "bin.vars module must be loaded in project root";

      // Enter variables.
      //
      // All the variables we enter are qualified so go straight for the
      // public variable pool.
      //
      auto& vp (rs.var_pool (true /* public */));

      // Target is a string and not target_triplet because it can be
      // specified by the user.
      //
      vp.insert<string>    ("config.bin.target");
      vp.insert<string>    ("config.bin.pattern");

      // Library types to build.
      //
      vp.insert<string>    ("config.bin.lib");

      // Library types to use (in priority order).
      //
      vp.insert<strings>   ("config.bin.exe.lib");
      vp.insert<strings>   ("config.bin.liba.lib");
      vp.insert<strings>   ("config.bin.libs.lib");

      // The rpath[_link].auto flag controls automatic rpath behavior, for
      // example, addition of rpaths for prerequisite libraries (see the cc
      // module for an example). Default is true.
      //
      // Note also that a rule may need to make rpath relative if
      // install.relocatable is true.
      //
      vp.insert<dir_paths> ("config.bin.rpath");
      vp.insert<bool>      ("config.bin.rpath.auto");

      vp.insert<dir_paths> ("config.bin.rpath_link");
      vp.insert<bool>      ("config.bin.rpath_link.auto");

      vp.insert<string>    ("config.bin.prefix");
      vp.insert<string>    ("config.bin.suffix");
      vp.insert<string>    ("config.bin.lib.prefix");
      vp.insert<string>    ("config.bin.lib.suffix");
      vp.insert<string>    ("config.bin.exe.prefix");
      vp.insert<string>    ("config.bin.exe.suffix");

      vp.insert<string>    ("bin.lib");

      vp.insert<strings>   ("bin.exe.lib");
      vp.insert<strings>   ("bin.liba.lib");
      vp.insert<strings>   ("bin.libs.lib");

      vp.insert<dir_paths> ("bin.rpath");
      vp.insert<bool>      ("bin.rpath.auto");

      vp.insert<dir_paths> ("bin.rpath_link");
      vp.insert<bool>      ("bin.rpath_link.auto");

      // Link whole archive. Note: with target visibility.
      //
      // The lookup semantics is as follows: we first look for a prerequisite-
      // specific value, then for a target-specific value in the prerequisite
      // library, and then for target type/pattern-specific value starting
      // from the scope of the target being linked. In that final lookup we do
      // not look in the target being linked itself since that is used to
      // indicate how this target should be used as a prerequisite of other
      // targets. For example:
      //
      // exe{test}: liba{foo}
      // liba{foo}: libua{foo1 foo2}
      // liba{foo}: bin.whole = false # Affects test but not foo1 and foo2.
      //
      // If unspecified, defaults to false for liba{} and to true for libu*{}.
      //
      vp.insert<bool> ("bin.whole", variable_visibility::target);

      // Mark library as binless.
      //
      // For example, the user can mark a C++ library consisting of only
      // module interfaces as binless so it becomes a modules equivalent to
      // header-only library (which we will call a module interface-only
      // library).
      //
      vp.insert<bool> ("bin.binless", variable_visibility::target);

      // Executable and library name prefixes and suffixes.
      //
      vp.insert<string> ("bin.exe.prefix");
      vp.insert<string> ("bin.exe.suffix");
      vp.insert<string> ("bin.lib.prefix");
      vp.insert<string> ("bin.lib.suffix");

      // The optional custom clean patterns should be just the pattern stem,
      // without the library prefix/name or extension. For example, `-[A-Z]`
      // instead of `libfoo-[A-Z].so`. Note that the custom version pattern is
      // only used for platform-independent versions (for platforms-specific
      // versions we can always derive the pattern automatically).
      //
      vp.insert<string> ("bin.lib.load_suffix");
      vp.insert<string> ("bin.lib.load_suffix_pattern");

      vp.insert<map<optional<string>, string>> ("bin.lib.version");
      vp.insert<string>                        ("bin.lib.version_pattern");

      return true;
    }

    bool
    types_init (scope& rs,
                scope& bs,
                const location& loc,
                bool,
                bool,
                module_init_extra&)
    {
      tracer trace ("bin::types_init");
      l5 ([&]{trace << "for " << rs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "bin.types module must be loaded in project root";

      // Register target types.
      //
      // Note that certain platform-specific and toolchain-specific types are
      // registered in bin and bin.ld.
      //
      // Note also that it would make sense to configure their default
      // "installability" here but that requires the knowledge of the platform
      // in some cases. So we do it all in bin for now. One way to support
      // both use-cases would be to detect if we are loaded after bin.guess
      // and then decide whether to do it here or delay to bin.
      //
      // NOTE: remember to update the documentation if changing anything here!
      //
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

      // Register the def{} target type. Note that we do it here since it is
      // input and can be specified unconditionally (i.e., not only when
      // building for Windows).
      //
      rs.insert_target_type<def> ();

      return true;
    }

    void
    functions (function_map&); // functions.cxx

    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 bool,
                 bool,
                 module_init_extra& extra)
    {
      tracer trace ("bin::config_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "bin.config module must be loaded in project root";

      context& ctx (rs.ctx);

      // Register the bin function family if this is the first instance of the
      // bin modules.
      //
      if (!function_family::defined (ctx.functions, "bin"))
        functions (ctx.functions);

      // Load bin.vars.
      //
      load_module (rs, rs, "bin.vars", loc);

      // Configuration.
      //
      using config::lookup_config;

      // Adjust module priority (binutils).
      //
      config::save_module (rs, "bin", 350);

      bool new_cfg (false); // Any new configuration values?

      // config.bin.target
      //
      const target_triplet* tgt (nullptr);
      {
        // Note: go straight for the public variable pool.
        //
        const variable& var (ctx.var_pool["config.bin.target"]);

        // We first see if the value was specified via the configuration
        // mechanism.
        //
        lookup l (lookup_config (new_cfg, rs, var));

        // Then see if there is a config hint (e.g., from the cc module).
        //
        bool hint (false);
        if (!l)
        {
          // Note: new_cfg is false for a hinted value.
          //
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
          s = run<string> (ctx,
                           3,
                           *config_sub, s.c_str (),
                           [] (string& l, bool) {return move (l);});
          l5 ([&]{trace << "config.sub target: '" << s << "'";});
        }

        try
        {
          target_triplet t (s);

          l5 ([&]{trace << "canonical target: '" << t.string () << "'; "
                        << "class: " << t.class_;});

          assert (!hint || s == t.representation ());

          // Also enter as bin.target.{cpu,vendor,system,version,class}
          // for convenience of access.
          //
          rs.assign<string> ("bin.target.cpu")     = t.cpu;
          rs.assign<string> ("bin.target.vendor")  = t.vendor;
          rs.assign<string> ("bin.target.system")  = t.system;
          rs.assign<string> ("bin.target.version") = t.version;
          rs.assign<string> ("bin.target.class")   = t.class_;

          tgt = &rs.assign<target_triplet> ("bin.target", move (t));
        }
        catch (const invalid_argument& e)
        {
          // This is where we suggest that the user specifies --config-sub
          // to help us out.
          //
          fail << "unable to parse binutils target '" << s << "': " << e <<
            info << "consider using the --config-sub option";
        }
      }

      // config.bin.pattern
      //
      const string* pat (nullptr);
      {
        // Note: go straight for the public variable pool.
        //
        const variable& var (ctx.var_pool["config.bin.pattern"]);

        // We first see if the value was specified via the configuration
        // mechanism.
        //
        lookup l (lookup_config (new_cfg, rs, var));

        // Then see if there is a config hint (e.g., from the C++ module).
        //
        if (!l)
        {
          // Note: new_cfg is false for a hinted value.
          //
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

          pat = &rs.assign<string> ("bin.pattern", s);
        }
      }

      // The idea here is as follows: if we already have one of
      // the bin.* variables set, then we assume this is static
      // project configuration and don't bother setting the
      // corresponding config.bin.* variable.
      //
      //@@ Need to validate the values. Would be more efficient
      //   to do it once on assignment than every time on query.
      //

      // config.bin.lib
      //
      // By default it's both unless the target doesn't support one of the
      // variants.
      //
      {
        value& v (rs.assign ("bin.lib"));
        if (!v)
          v = *lookup_config (rs,
                              "config.bin.lib",
                              tgt->system == "emscripten" ? "static" :
                              "both");
      }

      // config.bin.exe.lib
      //
      {
        value& v (rs.assign ("bin.exe.lib"));
        if (!v)
          v = *lookup_config (rs, "config.bin.exe.lib", exe_lib);
      }

      // config.bin.liba.lib
      //
      {
        value& v (rs.assign ("bin.liba.lib"));
        if (!v)
          v = *lookup_config (rs, "config.bin.liba.lib", liba_lib);
      }

      // config.bin.libs.lib
      //
      {
        value& v (rs.assign ("bin.libs.lib"));
        if (!v)
          v = *lookup_config (rs, "config.bin.libs.lib", libs_lib);
      }

      // config.bin.rpath[_link]
      //
      // These ones are optional and we merge them into bin.rpath[_link], if
      // any.
      //
      rs.assign ("bin.rpath") += cast_null<dir_paths> (
        lookup_config (rs, "config.bin.rpath", nullptr));

      rs.assign ("bin.rpath_link") += cast_null<dir_paths> (
        lookup_config (rs, "config.bin.rpath_link", nullptr));

      // config.bin.rpath[_link].auto
      //
      {
        lookup l;

        rs.assign ("bin.rpath.auto") =
          (l = lookup_config (rs, "config.bin.rpath.auto"))
          ? cast<bool> (l)
          : true;

        rs.assign ("bin.rpath_link.auto") =
          (l = lookup_config (rs, "config.bin.rpath_link.auto"))
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
        lookup p (lookup_config (rs, "config.bin.prefix"));
        lookup s (lookup_config (rs, "config.bin.suffix"));

        auto set = [&rs] (const char* bv, const char* cv, lookup l)
        {
          if (lookup o = lookup_config (rs, cv))
            l = o;

          if (l)
            rs.assign (bv) = *l;
        };

        set ("bin.lib.prefix", "config.bin.lib.prefix", p);
        set ("bin.lib.suffix", "config.bin.lib.suffix", s);

        set ("bin.exe.prefix", "config.bin.exe.prefix", p);
        set ("bin.exe.suffix", "config.bin.exe.suffix", s);
      }

      // If this is a configuration with new values, then print the report
      // at verbosity level 2 and up (-v).
      //
      if (verb >= (new_cfg ? 2 : 3))
      {
        diag_record dr (text);

        dr << "bin " << project (rs) << '@' << rs << '\n'
           << "  target     " << *tgt;

        if (pat != nullptr)
          dr << '\n'
             << "  pattern    " << *pat;
      }

      return true;
    }

    extern const char wasm_ext[] = "wasm"; // VC14 rejects constexpr.

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

      // Load bin.{config,types}.
      //
      load_module (rs, rs, "bin.config", loc, extra.hints);
      load_module (rs, rs, "bin.types", loc);

      // Cache some config values we will be needing below.
      //
      const target_triplet& tgt (cast<target_triplet> (rs["bin.target"]));

      // Configure target type default "installability". Also register
      // additional platform-specific types.
      //
      bool install_loaded (cast_false<bool> (rs["install.loaded"]));
      {
        using namespace install;

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
          install_path<libs> (
            bs, dir_path (tgt.class_ == "windows" ? "bin" : "lib"));

        // Create additional target types for certain targets.
        //
        if (tgt.class_ == "windows")
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

        if (tgt.cpu == "wasm32" || tgt.cpu == "wasm64")
        {
          // @@ TODO: shouldn't this be wrapped in if(first) somehow?

          const target_type& wasm (
            rs.derive_target_type(
              target_type {
                "wasm",
                &file::static_type,
                nullptr, /* factory */
                &target_extension_fix<wasm_ext>,
                nullptr, /* default_extension */
                &target_pattern_fix<wasm_ext>,
                &target_print_0_ext_verb, // Fixed extension, no use printing.
                &target_search, // Note: don't look for an existing file.
                target_type::flag::none}));

          if (install_loaded)
          {
            // Note that we keep the executable bit on the .wasm file, see
            // Emscripten issue 12707 for background.
            //
            install_path (bs, wasm, dir_path ("bin"));
          }
        }
      }

      // Register rules.
      //
      {
        auto& r (bs.rules);

        r.insert<obj> (perform_update_id, "bin.obj", obj_);
        r.insert<obj> (perform_clean_id,  "bin.obj", obj_);

        r.insert<bmi> (perform_update_id, "bin.bmi", obj_);
        r.insert<bmi> (perform_clean_id,  "bin.bmi", obj_);

        r.insert<hbmi> (perform_update_id, "bin.hbmi", obj_);
        r.insert<hbmi> (perform_clean_id,  "bin.hbmi", obj_);

        r.insert<libul> (perform_update_id, "bin.libul", libul_);
        r.insert<libul> (perform_clean_id,  "bin.libul", libul_);

        // Similar to alias.
        //
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
          // Note that without custom dist rules in setups along the follwing
          // lines the source file will be unreachable by dist:
          //
          // lib{foo}: obj{foo}
          // obja{foo}: cxx{foo}
          // objs{foo}: cxx{foo}
          //
          r.insert<obj> (dist_id, 0, "bin.obj", obj_);
          r.insert<bmi> (dist_id, 0, "bin.bmi", obj_);
          r.insert<hbmi> (dist_id, 0, "bin.hbmi", obj_);
          r.insert<libul> (dist_id, 0, "bin.libul", libul_);

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
        // All the variables we enter are qualified so go straight for the
        // public variable pool.
        //
        auto& vp (rs.var_pool (true /* public */));

        vp.insert<path> ("config.bin.ar");
        vp.insert<path> ("config.bin.ranlib");
      }

      // Configuration.
      //
      if (first)
      {
        using config::lookup_config;

        bool new_cfg (false); // Any new configuration values?

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
        const path& ar (
          cast<path> (
            lookup_config (new_cfg,
                           rs,
                           "config.bin.ar",
                           path (apply_pattern (ar_d, pat.pattern)),
                           config::save_default_commented)));

        const path* ranlib (
          cast_null<path> (
            lookup_config (new_cfg,
                           rs,
                           "config.bin.ranlib",
                           nullptr,
                           config::save_default_commented)));

        const ar_info& ari (guess_ar (rs.ctx, ar, ranlib, pat.paths));

        // If this is a configuration with new values, then print the report
        // at verbosity level 2 and up (-v).
        //
        if (verb >= (new_cfg ? 2 : 3))
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

        rs.assign<process_path_ex> ("bin.ar.path")   = process_path_ex (
          ari.ar_path,
          "ar",
          ari.ar_checksum,
          hash_environment (ari.ar_environment));
        rs.assign<string>       ("bin.ar.id")        = ari.ar_id;
        rs.assign<string>       ("bin.ar.signature") = ari.ar_signature;
        rs.assign<string>       ("bin.ar.checksum")  = ari.ar_checksum;

        {
          const semantic_version& v (ari.ar_version);

          rs.assign<string>   ("bin.ar.version")       = v.string ();
          rs.assign<uint64_t> ("bin.ar.version.major") = v.major;
          rs.assign<uint64_t> ("bin.ar.version.minor") = v.minor;
          rs.assign<uint64_t> ("bin.ar.version.patch") = v.patch;
          rs.assign<string>   ("bin.ar.version.build") = v.build;
        }

        config::save_environment (rs, ari.ar_environment);

        if (ranlib != nullptr)
        {
          rs.assign<process_path_ex> ("bin.ranlib.path")   = process_path_ex (
            ari.ranlib_path,
            "ranlib",
            ari.ranlib_checksum,
            hash_environment (ari.ranlib_environment));
          rs.assign<string>       ("bin.ranlib.id")        = ari.ranlib_id;
          rs.assign<string>       ("bin.ranlib.signature") = ari.ranlib_signature;
          rs.assign<string>       ("bin.ranlib.checksum")  = ari.ranlib_checksum;

          config::save_environment (rs, ari.ranlib_environment);
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
        // All the variables we enter are qualified so go straight for the
        // public variable pool.
        //
        auto& vp (rs.var_pool (true /* public */));

        vp.insert<path> ("config.bin.ld");
      }

      // Configuration.
      //
      if (first)
      {
        using config::lookup_config;

        bool new_cfg (false); // Any new configuration values?

        // config.bin.ld
        //
        // Use the target to decide on the default ld name.
        //
        const string& tsys (cast<string> (rs["bin.target.system"]));
        const char* ld_d (tsys == "win32-msvc" ? "link" : "ld");

        // This can be either a pattern or search path(s).
        //
        pattern_paths pat (lookup_pattern (rs));

        const path& ld (
          cast<path> (
            lookup_config (new_cfg,
                           rs,
                           "config.bin.ld",
                           path (apply_pattern (ld_d, pat.pattern)),
                           config::save_default_commented)));

        const ld_info& ldi (guess_ld (rs.ctx, ld, pat.paths));

        // If this is a configuration with new values, then print the report
        // at verbosity level 2 and up (-v).
        //
        if (verb >= (new_cfg ? 2 : 3))
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

        rs.assign<process_path_ex> ("bin.ld.path")   = process_path_ex (
          ldi.path,
          "ld",
          ldi.checksum,
          hash_environment (ldi.environment));
        rs.assign<string>       ("bin.ld.id")        = ldi.id;
        rs.assign<string>       ("bin.ld.signature") = ldi.signature;
        rs.assign<string>       ("bin.ld.checksum")  = ldi.checksum;

        if (ldi.version)
        {
          const semantic_version& v (*ldi.version);

          rs.assign<string>   ("bin.ld.version")       = v.string ();
          rs.assign<uint64_t> ("bin.ld.version.major") = v.major;
          rs.assign<uint64_t> ("bin.ld.version.minor") = v.minor;
          rs.assign<uint64_t> ("bin.ld.version.patch") = v.patch;
          rs.assign<string>   ("bin.ld.version.build") = v.build;
        }

        config::save_environment (rs, ldi.environment);
      }

      return true;
    }

    extern const char pdb_ext[] = "pdb"; // VC14 rejects constexpr.

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
        // @@ TODO: shouldn't this be wrapped in if(first) somehow?

        const target_type& pdb (
          rs.derive_target_type(
            target_type {
              "pdb",
              &file::static_type,
              nullptr, /* factory */
              &target_extension_fix<pdb_ext>,
              nullptr, /* default_extension */
              &target_pattern_fix<pdb_ext>,
              &target_print_0_ext_verb, // Fixed extension, no use printing.
              &target_search, // Note: don't look for an existing file.
              target_type::flag::none}));

        if (cast_false<bool> (rs["install.loaded"]))
        {
          install_path (bs, pdb, dir_path ("bin")); // Goes to install.bin
          install_mode (bs, pdb, "644");            // But not executable.
        }
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
        // All the variables we enter are qualified so go straight for the
        // public variable pool.
        //
        auto& vp (rs.var_pool (true /* public */));

        vp.insert<path> ("config.bin.rc");
      }

      // Configuration.
      //
      if (first)
      {
        using config::lookup_config;

        bool new_cfg (false); // Any new configuration values?

        // config.bin.rc
        //
        // Use the target to decide on the default rc name.
        //
        const string& tsys (cast<string> (rs["bin.target.system"]));
        const char* rc_d (tsys == "win32-msvc" ? "rc" : "windres");

        // This can be either a pattern or search path(s).
        //
        pattern_paths pat (lookup_pattern (rs));

        const path& rc (
          cast<path> (
            lookup_config (new_cfg,
                           rs,
                           "config.bin.rc",
                           path (apply_pattern (rc_d, pat.pattern)),
                           config::save_default_commented)));

        const rc_info& rci (guess_rc (rs.ctx, rc, pat.paths));

        // If this is a configuration with new values, then print the report
        // at verbosity level 2 and up (-v).
        //
        if (verb >= (new_cfg ? 2 : 3))
        {
          text << "bin.rc " << project (rs) << '@' << rs << '\n'
               << "  rc         " << rci.path << '\n'
               << "  id         " << rci.id << '\n'
               << "  signature  " << rci.signature << '\n'
               << "  checksum   " << rci.checksum;
        }

        rs.assign<process_path_ex> ("bin.rc.path")   = process_path_ex (
          rci.path,
          "rc",
          rci.checksum,
          hash_environment (rci.environment));
        rs.assign<string>       ("bin.rc.id")        = rci.id;
        rs.assign<string>       ("bin.rc.signature") = rci.signature;
        rs.assign<string>       ("bin.rc.checksum")  = rci.checksum;

        config::save_environment (rs, rci.environment);
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

    bool
    nm_config_init (scope& rs,
                    scope& bs,
                    const location& loc,
                    bool first,
                    bool,
                    module_init_extra& extra)
    {
      tracer trace ("bin::nm_config_init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure bin.config is loaded.
      //
      load_module (rs, bs, "bin.config", loc, extra.hints);

      // Enter configuration variables.
      //
      if (first)
      {
        // All the variables we enter are qualified so go straight for the
        // public variable pool.
        //
        auto& vp (rs.var_pool (true /* public */));

        vp.insert<path> ("config.bin.nm");
      }

      // Configuration.
      //
      if (first)
      {
        using config::lookup_config;

        bool new_cfg (false); // Any new configuration values?

        // config.bin.nm
        //
        // Use the target to decide on the default nm name. Note that in case
        // of win32-msvc this is insufficient and we fallback to the linker
        // type (if available) to decide between dumpbin and llvm-nm (with
        // fallback to dumpbin).
        //
        // Finally note that the dumpbin.exe functionality is available via
        // link.exe /DUMP.
        //
        const string& tsys (cast<string> (rs["bin.target.system"]));
        const char* nm_d (tsys == "win32-msvc"
                          ? (cast_empty<string> (rs["bin.ld.id"]) == "msvc-lld"
                             ? "llvm-nm"
                             : "dumpbin")
                          : "nm");

        // This can be either a pattern or search path(s).
        //
        pattern_paths pat (lookup_pattern (rs));

        const path& nm (
          cast<path> (
            lookup_config (new_cfg,
                           rs,
                           "config.bin.nm",
                           path (apply_pattern (nm_d, pat.pattern)),
                           config::save_default_commented)));

        const nm_info& nmi (guess_nm (rs.ctx, nm, pat.paths));

        // If this is a configuration with new values, then print the report
        // at verbosity level 2 and up (-v).
        //
        if (verb >= (new_cfg ? 2 : 3))
        {
          text << "bin.nm " << project (rs) << '@' << rs << '\n'
               << "  nm         " << nmi.path << '\n'
               << "  id         " << nmi.id << '\n'
               << "  signature  " << nmi.signature << '\n'
               << "  checksum   " << nmi.checksum;
        }

        rs.assign<process_path_ex> ("bin.nm.path")   = process_path_ex (
          nmi.path,
          "nm",
          nmi.checksum,
          hash_environment (nmi.environment));
        rs.assign<string>       ("bin.nm.id")        = nmi.id;
        rs.assign<string>       ("bin.nm.signature") = nmi.signature;
        rs.assign<string>       ("bin.nm.checksum")  = nmi.checksum;

        config::save_environment (rs, nmi.environment);
      }

      return true;
    }

    bool
    nm_init (scope& rs,
             scope& bs,
             const location& loc,
             bool,
             bool,
             module_init_extra& extra)
    {
      tracer trace ("bin::nm_init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure the bin core and nm.config are loaded.
      //
      load_module (rs, bs, "bin",           loc, extra.hints);
      load_module (rs, bs, "bin.nm.config", loc, extra.hints);

      return true;
    }

    bool
    def_init (scope& rs,
              scope& bs,
              const location& loc,
              bool,
              bool,
              module_init_extra& extra)
    {
      tracer trace ("bin::def_init");
      l5 ([&]{trace << "for " << bs;});

      // Make sure the bin core is loaded (def{} target type). We also load
      // nm.config unless we are using MSVC link.exe and can access dumpbin
      // via its /DUMP option.
      //
      const string* lid (cast_null<string> (rs["bin.ld.id"]));

      load_module (rs, bs, "bin", loc, extra.hints);

      if (lid == nullptr || *lid != "msvc")
        load_module (rs, bs, "bin.nm.config", loc, extra.hints);

      // Register the def{} rule.
      //
      bs.insert_rule<def> (perform_update_id,   "bin.def", def_);
      bs.insert_rule<def> (perform_clean_id,    "bin.def", def_);
      bs.insert_rule<def> (configure_update_id, "bin.def", def_);

      return true;
    }

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"bin.vars",      nullptr, vars_init},
      {"bin.types",     nullptr, types_init},
      {"bin.config",    nullptr, config_init},
      {"bin.ar.config", nullptr, ar_config_init},
      {"bin.ar",        nullptr, ar_init},
      {"bin.ld.config", nullptr, ld_config_init},
      {"bin.ld",        nullptr, ld_init},
      {"bin.rc.config", nullptr, rc_config_init},
      {"bin.rc",        nullptr, rc_init},
      {"bin.nm.config", nullptr, nm_config_init},
      {"bin.nm",        nullptr, nm_init},
      {"bin.def",       nullptr, def_init},
      {"bin",           nullptr, init},
      {nullptr,         nullptr, nullptr}
    };

    const module_functions*
    build2_bin_load ()
    {
      return mod_functions;
    }
  }
}
