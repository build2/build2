// file      : libbuild2/cxx/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cxx/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>
#include <libbuild2/install/utility.hxx>

#include <libbuild2/cc/guess.hxx>
#include <libbuild2/cc/module.hxx>

#include <libbuild2/cc/target.hxx> // pc*
#include <libbuild2/cxx/target.hxx>

#ifndef BUILD2_DEFAULT_CXX
#  ifdef BUILD2_NATIVE_CXX
#    define BUILD2_DEFAULT_CXX BUILD2_NATIVE_CXX
#  else
#    define BUILD2_DEFAULT_CXX ""
#  endif
#endif

using namespace std;
using namespace butl;

namespace build2
{
  namespace cxx
  {
    using cc::compiler_id;
    using cc::compiler_type;
    using cc::compiler_class;
    using cc::compiler_info;

    class config_module: public cc::config_module
    {
    public:
      explicit
      config_module (config_data&& d): cc::config_module (move (d)) {}

      virtual void
      translate_std (const compiler_info&,
                     const target_triplet&,
                     scope&,
                     strings&,
                     const string*) const override;
    };

    using cc::module;

    void config_module::
    translate_std (const compiler_info& ci,
                   const target_triplet& tt,
                   scope& rs,
                   strings& mode,
                   const string* v) const
    {
      compiler_type ct (ci.id.type);
      compiler_class cl (ci.class_);
      uint64_t mj (ci.version.major);
      uint64_t mi (ci.version.minor);
      uint64_t p (ci.version.patch);

      // Besides various `NN` we have two special values: `latest` and
      // `experimental`. It can also be `gnu++NN`.
      //
      // The semantics of the `latest` value is the latest available standard
      // that is not necessarily complete or final but is practically usable.
      // In other words, a project that uses this value and does not rely on
      // any unstable/bleeding edge parts of the standard (or takes care to
      // deal with them, for example, using feature test macros), can be
      // reasonably expected to work. In particular, this is the value we use
      // by default in projects created by bdep-new(1) as well as to build the
      // build2 toolchain itself.
      //
      // The `experimental` value, as the name suggests, is the latest
      // available standard that is not necessarily usable in real projects.
      // By definition, `experimental` >= `latest`.
      //
      // In addition to the `experimental` value itself we have a number of
      // feature flags that can be used to enable or disable certain major
      // parts (such as modules, concepts, etc) in this mode. They are also
      // used to signal back to the project whether a particular feature is
      // available. A feature flag set by the user has a tri-state semantics:
      //
      // - false        - disabled
      // - unspecified  - enabled if practically usable
      // - true         - enabled even if practically unusable
      //
      bool latest       (v != nullptr && *v == "latest");
      bool experimental (v != nullptr && *v == "experimental");

      // This helper helps recognize both NN and [cC]++NN to avoid an endless
      // stream of user questions. It can also be used to recognize Nx in
      // addition to NN (e.g., "14" and "1y").
      //
      auto stdcmp = [v] (const char* nn, const char* nx = nullptr)
      {
        if (v != nullptr)
        {
          const char* s (v->c_str ());
          if ((s[0] == 'c' || s[0] == 'C') && s[1] == '+' && s[2] == '+')
            s += 3;

          return strcmp (s, nn) == 0 || (nx != nullptr && strcmp (s, nx) == 0);
        }

        return false;
      };

      // Feature flags.
      //
      auto& vp (rs.var_pool (true /* public */)); // All qualified.

      // Similar to config.cxx.std, config.cxx.features.* overrides
      // cxx.features.*.
      //
      struct feature
      {
        optional<bool>   value; // cxx.features.* value.
        optional<bool> c_value; // config.cxx.features.* value.
        bool            result; // Calculated result value.

        feature& operator= (bool r) {result = r; return *this;}

        build2::value&    value_; // cxx.features.* variable value.
        const char*       name_;  // Feature name.
      };

      auto get_feature = [&rs, &vp] (const char* name) -> feature
      {
        auto& var   (vp.insert<bool> (string ("cxx.features.") + name));
        auto& c_var (vp.insert<bool> (string ("config.cxx.features.") + name));

        pair<value&, bool> val (rs.vars.insert (var));
        lookup l (config::lookup_config (rs, c_var));

        optional<bool> v, c_v;
        if (l.defined ())
          v = c_v = cast_false<bool> (*l);
        else if (!val.second)
          v = cast_false<bool> (val.first);

        return feature {v, c_v, false, val.first, name};
      };

      auto set_feature = [&rs, &ci, v] (const feature& f)
      {
        if (f.c_value && *f.c_value != f.result)
        {
          fail << f.name_ << " cannot be "
               << (*f.c_value ? "enabled" : "disabled") << " for "
               << project (rs) << '@' << rs <<
          info << "C++ language standard is "
               << (v != nullptr ? v->c_str () : "compiler-default") <<
          info << "C++ compiler is " << ci.signature <<
          info << f.name_ << " state requested with config.cxx.features."
               << f.name_;
        }

        f.value_ = f.result;
      };

      feature modules (get_feature ("modules"));
      //feature concepts (get_feature ("concepts"));

      // NOTE: see also module sidebuild subproject if changing anything about
      // modules here.

      string o;

      auto prepend = [&mode, i = mode.begin ()] (string o) mutable
      {
        i = mode.insert (i, move (o)) + 1;
      };

      // Derive approximate __cplusplus value from the standard if possible.
      //
      optional<uint32_t> cplusplus;

      switch (cl)
      {
      case compiler_class::msvc:
        {
          // C++ standard-wise, with VC you got what you got up until 14.2.
          // Starting with 14.3 there is now the /std: switch which defaults
          // to c++14 but can be set to c++latest. And from 15.3 it can be
          // c++17. And from 16.11 it can be c++20 (we start with the compiler
          // version for 16.11.4 since 16.11.0 seems to be indistinguishable
          // from 16.10).
          //
          bool v16_11 (           mj >  19 || (mj == 19 && (mi > 29 || (mi == 29 && p >= 30136))));
          bool v16_0  (v16_11 || (mj == 19 && mi >= 20));
          bool v15_3  (v16_0  || (mj == 19 && mi >= 11));
          bool v14_3  (v15_3  || (mj == 19 && (mi > 0 || (mi == 0 && p >= 24215))));

          // The question is also whether we should verify that the requested
          // standard is provided by this VC version. And if so, from which
          // version should we say VC supports 11, 14, and 17? We should
          // probably be as loose as possible here since the author will
          // always be able to tighten (but not loosen) this in the buildfile
          // (i.e., detect unsupported versions).
          //
          // For now we are not going to bother doing this for C++03.
          //
          if (experimental)
          {
            if (v14_3)
              o = "/std:c++latest";

            // According to the documentation:
            //
            // "The value of __cplusplus with the /std:c++latest option
            // depends on the version of Visual Studio. It's always at least
            // one higher than the highest supported __cplusplus standard
            // value supported by your version of Visual Studio."
            //
            if (v16_11)
              cplusplus = 202002 + 1;
            else if (v16_0)
              cplusplus = 201703 + 1;
            else if (v14_3)
              cplusplus = 201402 + 1;
            else if (mj >= 19)
              cplusplus = 201402;
            else if (mj >= 16)
              cplusplus = 201103;
            else
              cplusplus = 199711;
          }
          else if (latest)
          {
            // We used to map `latest` to `c++latest` but starting from 16.1,
            // VC seem to have adopted the "move fast and break things" motto
            // for this mode. So starting from 16 we only enable it in
            // `experimental`.
            //
            // Note: no /std:c++23 yet as of MSVC 17.6.
            //
            if (v16_11)
              o = "/std:c++20";
            else if (v16_0)
              o = "/std:c++17";
            else if (v14_3)
              o = "/std:c++latest";

            if (v16_11)
              cplusplus = 202002;
            else if (v16_0)
              cplusplus = 201703;
            else if (v14_3)
              cplusplus = 201402 + 1;
            else if (mj >= 19)
              cplusplus = 201402;
            else if (mj >= 16)
              cplusplus = 201103;
            else
              cplusplus = 199711;
          }
          else if (v == nullptr)
          {
            // @@ TODO: map defaults to cplusplus for each version.
          }
          else if (!stdcmp ("98") && !stdcmp ("03"))
          {
            bool sup (false);

            if      (stdcmp ("11", "0x")) // C++11 since VS2010/10.0.
            {
              sup = mj >= 16;
              cplusplus = 201103;
            }
            else if (stdcmp ("14", "1y")) // C++14 since VS2015/14.0.
            {
              sup = mj >= 19;
              cplusplus = 201402;
            }
            else if (stdcmp ("17", "1z")) // C++17 since VS2015/14.0u2.
            {
              // Note: the VC15 compiler version is 19.10.
              //
              sup = (mj > 19 ||
                     (mj == 19 && (mi > 0 || (mi == 0 && p >= 23918))));
              cplusplus = 201703;
            }
            else if (stdcmp ("20", "2a")) // C++20 since VS2019/16.11.
            {
              sup = v16_11;
              cplusplus = 202002;
            }

            if (!sup)
              fail << "C++ " << *v << " is not supported by " << ci.signature <<
                info << "required by " << project (rs) << '@' << rs;

            if (v15_3)
            {
              if      (stdcmp ("20", "2a")) o = "/std:c++20";
              else if (stdcmp ("17", "1z")) o = "/std:c++17";
              else if (stdcmp ("14", "1y")) o = "/std:c++14";
            }
            else if (v14_3)
            {
              if      (stdcmp ("14", "1y")) o = "/std:c++14";
              else if (stdcmp ("17", "1z")) o = "/std:c++latest";
            }
          }
          else
            cplusplus = 199711;

          if (!o.empty ())
            prepend (move (o));

          // Since VC 15.7 we can get a (more) accurate __cplusplus value if
          // we ask for it with /Zc:__cplusplus:
          //
          // https://devblogs.microsoft.com/cppblog/msvc-now-correctly-reports-__cplusplus/
          //
          if (mj > 19 || (mj == 19 &&  mi >= 14))
          {
            if (!find_option_prefix ("/Zc:__cplusplus", mode))
              prepend ("/Zc:__cplusplus");
          }

          break;
        }
      case compiler_class::gcc:
        {
          if (latest || experimental)
          {
            switch (ct)
            {
            case compiler_type::gcc:
              {
                if (mj >= 14)
                {
                  o = "-std=c++26";
                  cplusplus = 202400;
                }
                else if (mj >= 11)
                {
                  o = "-std=c++23";
                  cplusplus = 202302;
                }
                else if (mj >= 8)
                {
                  o = "-std=c++2a";
                  cplusplus = 202002;
                }
                else if (mj >= 5)
                {
                  o = "-std=c++1z";
                  cplusplus = 201703;
                }
                else if (mj == 4 && mi >= 8)
                {
                  o = "-std=c++1y";
                  cplusplus = 201402;
                }
                else if (mj == 4 && mi >= 4)
                {
                  o = "-std=c++0x";
                  cplusplus = 201103;
                }
                else
                  cplusplus = 199711;

                break;
              }
            case compiler_type::clang:
              {
                if (mj >= 18)
                {
                  o = "-std=c++26";
                  cplusplus = 202400;
                }
                else if (mj >= 13)
                {
                  o = "-std=c++2b";
                  cplusplus = 202302;
                }
                else if (mj == 10 && latest && tt.system == "win32-msvc")
                {
                  // Clang 10.0.0 targeting MSVC 16.4 and 16.5 (preview) in
                  // the c++2a mode uncovers some Concepts-related bugs in
                  // MSVC STL (LLVM bug #44956). So in this case we map
                  // `latest` to c++17.
                  //
                  // While reportedly this has been fixed in the later
                  // versions of MSVC, instead of somehow passing the version
                  // of MSVC Clang is targeting, we will just assume that
                  // Clang 11 and later are used with a sufficiently new
                  // version of MSVC.
                  //
                  o = "-std=c++17";
                  cplusplus = 201703;
                }
                else if (mj >= 5)
                {
                  o = "-std=c++2a";
                  cplusplus = 202002;
                }
                else if (mj >  3 || (mj == 3 && mi >= 5))
                {
                  o = "-std=c++1z";
                  cplusplus = 201703;
                }
                else if (mj == 3 && mi >= 4)
                {
                  o = "-std=c++1y";
                  cplusplus = 201402;
                }
                else /* ??? */
                {
                  o = "-std=c++0x";
                  cplusplus = 201103;
                }

                break;
              }
            case compiler_type::icc:
              {
                if (mj >= 17)
                {
                  o = "-std=c++1z";
                  cplusplus = 201703;
                }
                else if (mj >  15 || (mj == 15 && p >= 3))
                {
                  o = "-std=c++1y";
                  cplusplus = 201402;
                }
                else /* ??? */
                {
                  o = "-std=c++0x";
                  cplusplus = 201103;
                }

                break;
              }
            default:
              assert (false);
            }
          }
          else if (v == nullptr)
          {
            // @@ TODO: map defaults to cplusplus for each version.
          }
          else
          {
            // Translate 11 to 0x, 14 to 1y, 17 to 1z, 20 to 2a, 23 to 2b, and
            // 26 to 2c for compatibility with older versions of the
            // compilers.
            //
            // @@ TMP: update C++26 __cplusplus value once known (and above).
            //
            o = "-std=";

            if      (stdcmp ("26", "2c")) {o += "c++2c"; cplusplus = 202400;}
            else if (stdcmp ("23", "2b")) {o += "c++2b"; cplusplus = 202302;}
            else if (stdcmp ("20", "2a")) {o += "c++2a"; cplusplus = 202002;}
            else if (stdcmp ("17", "1z")) {o += "c++1z"; cplusplus = 201703;}
            else if (stdcmp ("14", "1y")) {o += "c++1y"; cplusplus = 201402;}
            else if (stdcmp ("11", "0x")) {o += "c++0x"; cplusplus = 201103;}
            else if (stdcmp ("03")      ) {o += "c++03"; cplusplus = 199711;}
            else if (stdcmp ("98")      ) {o += "c++98"; cplusplus = 199711;}
            else
            {
              o += *v; // In case the user specifies `gnu++NN` or some such.

              // @@ TODO: can we still try to derive cplusplus value?
            }
          }

          if (!o.empty ())
            prepend (move (o));

          break;
        }
      }

      // Additional experimental options.
      //
      if (experimental)
      {
        switch (ct)
        {
        case compiler_type::msvc:
          {
            // Let's enable the new preprocessor in this mode. For background,
            // see MSVC issue 10537317.
            //
            if (mj > 19 || (mj == 19 && mi >= 39))
              prepend ("/Zc:preprocessor");

            // Starting with 15.5 (19.12) Visual Studio-created projects
            // default to the strict mode. However, this flag currently tends
            // to trigger too many compiler bugs. So for now we leave it to
            // the experimenters to enjoy.
            //
            if (mj > 19 || (mj == 19 && mi >= 12))
              prepend ("/permissive-");

            break;
          }
        default:
          break;
        }
      }

      // Unless disabled by the user, try to enable C++ modules.
      //
      // NOTE: see also diagnostics about modules support required (if
      //       attempting to use) in compile rule.
      //
      if (!modules.value || *modules.value)
      {
        switch (ct)
        {
        case compiler_type::msvc:
          {
            // Modules are enabled by default in /std:c++20 and
            // /std:c++latest with both defining __cpp_modules to 201907
            // (final C++20 module), at least as of 17.6 (LTS).
            //
            // @@ Should we enable modules by default? There are still some
            // serious bugs, like inability to both `import std;` and
            // `#include <string>` in the same translation unit (see Visual
            // Studio issue #10541166).
            //
            if (modules.value)
            {
              if (cplusplus && *cplusplus < 202002)
              {
                fail << "support for C++ modules requires C++20 or later" <<
                  info << "standard in use is " << *cplusplus <<
                  info << "required by " << project (rs) << '@' << rs;
              }

              if (mj < 19 || (mj == 19 && mi < 36))
              {
                fail << "support for C++ modules requires MSVC 17.6 or later" <<
                  info << "C++ compiler is " << ci.signature <<
                  info << "required by " << project (rs) << '@' << rs;
              }

              modules = true;
            }

            break;
          }
        case compiler_type::gcc:
          {
            // We use the module mapper support which is only available since
            // GCC 11. And since we are not yet capable of supporting
            // generated headers via the mapper, we require the user to
            // explicitly request modules.
            //
            // @@ Actually, now that we pre-generate headers by default, this
            // is probably no longer the reason. But GCC modules being
            // unusable due to bugs is stil a reason.
            //
            if (modules.value)
            {
              if (cplusplus && *cplusplus < 202002)
              {
                fail << "support for C++ modules requires C++20 or later" <<
                  info << "standard in use is " << *cplusplus <<
                  info << "required by " << project (rs) << '@' << rs;
              }

              if (mj < 11)
              {
                fail << "support for C++ modules requires GCC 11 or later" <<
                  info << "C++ compiler is " << ci.signature <<
                  info << "required by " << project (rs) << '@' << rs;
              }

              // Defines __cpp_modules:
              //
              // 11 -- 201810
              //
              prepend ("-fmodules-ts");
              modules = true;
            }

            break;
          }
        case compiler_type::clang:
          {
            // Things (command line options, semantics) changed quite a bit
            // around Clang 16 so we don't support anything earlier than
            // that (it's not practically usable anyway).
            //
            // Clang enables modules by default in c++20 or later but they
            // don't yet (as of Clang 18) define __cpp_modules. When they
            // do, we can consider enabling modules by default on our side.
            // For now, we only enable modules if forced with explicit
            // cxx.features.modules=true.
            //
            if (modules.value)
            {
              if (cplusplus && *cplusplus < 202002)
              {
                fail << "support for C++ modules requires C++20 or later" <<
                  info << "standard in use is " << *cplusplus <<
                  info << "required by " << project (rs) << '@' << rs;
              }

              if (mj < 16)
              {
                fail << "support for C++ modules requires Clang 16 or later" <<
                  info << "C++ compiler is " << ci.signature <<
                  info << "required by " << project (rs) << '@' << rs;
              }

              // See https://github.com/llvm/llvm-project/issues/71364
              //
              prepend ("-D__cpp_modules=201907L");
              modules = true;
            }

            break;
          }
        case compiler_type::icc:
          break; // No modules support yet.
        }
      }

      set_feature (modules);
      //set_feature (concepts);
    }

    // See cc::data::x_{hdr,inc} for background.
    //
    static const target_type* const hdr[] =
    {
      &hxx::static_type,
      &ixx::static_type,
      &txx::static_type,
      &mxx::static_type,
      nullptr
    };

    // Note that we don't include S{} here because none of the files we
    // compile can plausibly want to include .S. (Maybe in inline assembler
    // instructions?)
    //
    static const target_type* const inc[] =
    {
      &hxx::static_type,
      &h::static_type,
      &ixx::static_type,
      &txx::static_type,
      &mxx::static_type,
      &cxx::static_type,
      &c::static_type,
      &mm::static_type,
      &m::static_type,
      &cxx_inc::static_type,
      &cc::c_inc::static_type,
      nullptr
    };

    bool
    types_init (scope& rs,
                scope& bs,
                const location& loc,
                bool,
                bool,
                module_init_extra&)
    {
      tracer trace ("cxx::types_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cxx.types module must be loaded in project root";

      // Register target types and configure their "installability".
      //
      using namespace install;

      bool install_loaded (cast_false<bool> (rs["install.loaded"]));

      // Note: not registering mm{} (it is registered seperately by the
      // respective optional .types submodule).
      //
      // Note: mxx{} is in hdr. @@ But maybe it shouldn't be...
      //
      rs.insert_target_type<cxx> ();

      auto insert_hdr = [&rs, install_loaded] (const target_type& tt)
      {
        rs.insert_target_type (tt);

        // Install headers into install.include.
        //
        if (install_loaded)
          install_path (rs, tt, dir_path ("include"));
      };

      for (const target_type* const* ht (hdr); *ht != nullptr; ++ht)
        insert_hdr (**ht);

      // Also register the C header for C-derived languages.
      //
      insert_hdr (h::static_type);

      // @@ PERF: maybe factor this to cc.types?
      //
      rs.insert_target_type<cc::pc> ();
      rs.insert_target_type<cc::pca> ();
      rs.insert_target_type<cc::pcs> ();

      if (install_loaded)
        install_path<cc::pc> (rs, dir_path ("pkgconfig"));

      return true;
    }

    static const char* const hinters[] = {"c", nullptr};

    // See cc::module for details on guess_init vs config_init.
    //
    bool
    guess_init (scope& rs,
                scope& bs,
                const location& loc,
                bool,
                bool,
                module_init_extra& extra)
    {
      tracer trace ("cxx::guess_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cxx.guess module must be loaded in project root";

      // Load cc.core.vars so that we can cache all the cc.* variables.
      //
      load_module (rs, rs, "cc.core.vars", loc);

      // Enter all the variables and initialize the module data.
      //
      // All the variables we enter are qualified so go straight for the
      // public variable pool.
      //
      auto& vp (rs.var_pool (true /* public */));

      cc::config_data d {
        cc::lang::cxx,

        "cxx",
        "c++",
        "obj-c++",
        BUILD2_DEFAULT_CXX,
        ".ii",
        ".mii",

        hinters,

        vp["bin.binless"],

        // NOTE: remember to update documentation if changing anything here.
        //
        vp.insert<strings> ("config.cxx"),
        vp.insert<string>  ("config.cxx.id"),
        vp.insert<string>  ("config.cxx.version"),
        vp.insert<string>  ("config.cxx.target"),
        vp.insert<string>  ("config.cxx.std"),
        vp.insert<strings> ("config.cxx.poptions"),
        vp.insert<strings> ("config.cxx.coptions"),
        vp.insert<strings> ("config.cxx.loptions"),
        vp.insert<strings> ("config.cxx.aoptions"),
        vp.insert<strings> ("config.cxx.libs"),

        // Project's internal scope.
        //
        // A header search path (-I) exported by a library that is outside of
        // the internal scope is considered external and, if supported by the
        // compiler, the corresponding -I option is translated to an
        // appropriate "external header search path" option (-isystem for
        // GCC/Clang, /external:I for MSVC 16.10 and later or clang-cl 13 and
        // later). In particular, this suppresses compiler warnings in such
        // external headers (/external:W0 is automatically added unless a
        // custom /external:Wn is specified).
        //
        // The internal scope can be specified by the project with the
        // cxx.internal.scope variable and overridden by the user with the
        // config.cxx.internal.scope variable. Note that cxx.internal.scope
        // must be specified before loading the cxx module (cxx.config, more
        // precisely) and after which it contains the effective value (see
        // below). For example:
        //
        //   # root.build
        //
        //   cxx.internal.scope = current
        //
        //   using cxx
        //
        // Valid values for cxx.internal.scope are:
        //
        //   current  -- current root scope (where variable is assigned)
        //   base     -- target's base scope
        //   root     -- target's root scope
        //   bundle   -- target's bundle amalgamation (see scope::bundle_root())
        //   strong   -- target's strong amalgamation (see scope::strong_root())
        //   weak     -- target's weak amalgamation (see scope::weak_root())
        //   global   -- global scope (everything is internal)
        //
        // Valid values for config.cxx.internal.scope are the same except for
        // `current`.
        //
        // Note also that there are [config.]cc.internal.scope variables that
        // can be used to specify the internal scope for all the cc-based
        // modules.
        //
        // The project's effective internal scope is chosen based on the
        // following priority list:
        //
        // 1. config.cxx.internal.scope
        //
        // 2. config.cc.internal.scope
        //
        // 3. effective scope from bundle amalgamation
        //
        // 4. cxx.internal.scope
        //
        // 5. cc.internal.scope
        //
        // In particular, item #3 allows an amalgamation that bundles a
        // project to override its internal scope.
        //
        // The recommended value for a typical project is `current`, meaning
        // that only headers inside the project will be considered internal.
        // The tests subproject, if present, will inherit its value from the
        // project (which acts as a bundle amalgamation), unless it is being
        // built out of source (for example, to test an installed library).
        //
        // A project can also whitelist specific libraries using the
        // cxx.internal.libs variable. If a library target name (that is, the
        // name inside lib{}) matches any of the wildcard patterns listed in
        // this variable, then the library is considered internal regardless
        // of its location. For example (notice that the pattern is quoted):
        //
        //   # root.build
        //
        //   cxx.internal.scope = current
        //   cxx.internal.libs = foo 'bar-*'
        //
        //   using cxx
        //
        // Note that this variable should also be set before loading the
        // cxx module and there is the common cc.internal.libs equivalent.
        // However, there are no config.* versions nor the override by the
        // bundle amalgamation semantics.
        //
        // Typically you would want to whitelist libraries that are developed
        // together but reside in separate build system projects. In
        // particular, a separate *-tests project for a library should
        // whitelist the library being tested if the internal scope
        // functionality is in use. Another reason to whitelist is to catch
        // warnings in instantiations of templates that belong to a library
        // that is otherwise warning-free (see the MSVC /external:templates-
        // option for background).
        //
        // Note also that if multiple libraries are installed into the same
        // location (or otherwise share the same header search paths, for
        // example, as a family of libraries), then the whitelist may not
        // be effective.
        //
        vp.insert<string> ("config.cxx.internal.scope"),

        // Headers and header groups whose inclusion should or should not be
        // translated to the corresponding header unit imports.
        //
        // A header can be specified either as an absolute and normalized path
        // or as a <>-style include file or file pattern (for example,
        // <vector>, <boost/**.hpp>). The latter kind is automatically
        // resolved to the absolute form based on the compiler's system (as
        // opposed to project's) header search paths.
        //
        // Currently recognized header groups are:
        //
        // std-importable -- translate importable standard library headers
        // std            -- translate all standard library headers
        // all-importable -- translate all importable headers
        // all            -- translate all headers
        //
        // Note that a header may belong to multiple groups which are looked
        // up from the most to least specific, for example: <vector>,
        // std-importable, std, all-importable, all.
        //
        // A header or group can also be excluded from being translated, for
        // example:
        //
        //   std-importable <vector>@false
        //
        // The config.cxx.translate_include value is prepended (merged with
        // override) into cxx.translate_include while loading the cxx.config
        // module. The headers and header groups in cxx.translate_include are
        // resolved while loading the cxx module. For example:
        //
        //   cxx.translate_include = <map>@false  # Can be overriden.
        //   using cxx.config
        //   cxx.translate_include =+ <set>@false # Cannot be overriden.
        //   using cxx
        //
        &vp.insert<cc::translatable_headers> ("config.cxx.translate_include"),

        vp.insert<process_path_ex> ("cxx.path"),
        vp.insert<strings>         ("cxx.mode"),
        vp.insert<path>            ("cxx.config.path"),
        vp.insert<strings>         ("cxx.config.mode"),
        vp.insert<dir_paths>       ("cxx.sys_lib_dirs"),
        vp.insert<dir_paths>       ("cxx.sys_hdr_dirs"),

        vp.insert<string>   ("cxx.std"),

        vp.insert<strings>  ("cxx.poptions"),
        vp.insert<strings>  ("cxx.coptions"),
        vp.insert<strings>  ("cxx.loptions"),
        vp.insert<strings>  ("cxx.aoptions"),
        vp.insert<strings>  ("cxx.libs"),

        vp.insert<string>  ("cxx.internal.scope"),
        vp.insert<strings> ("cxx.internal.libs"),

        &vp.insert<cc::translatable_headers> ("cxx.translate_include"),

        vp["cc.poptions"],
        vp["cc.coptions"],
        vp["cc.loptions"],
        vp["cc.aoptions"],
        vp["cc.libs"],

        vp.insert<strings>      ("cxx.export.poptions"),
        vp.insert<strings>      ("cxx.export.coptions"),
        vp.insert<strings>      ("cxx.export.loptions"),
        vp.insert<vector<name>> ("cxx.export.libs"),
        vp.insert<vector<name>> ("cxx.export.impl_libs"),

        vp["cc.export.poptions"],
        vp["cc.export.coptions"],
        vp["cc.export.loptions"],
        vp["cc.export.libs"],
        vp["cc.export.impl_libs"],

        vp["cc.pkgconfig.include"],
        vp["cc.pkgconfig.lib"],

        vp.insert<string> ("cxx.stdlib"),

        vp["cc.runtime"],
        vp["cc.stdlib"],

        vp["cc.type"],
        vp["cc.system"],
        vp["cc.module_name"],
        vp["cc.importable"],
        vp["cc.reprocess"],
        vp["cc.serialize"],

        // Ability to signal that source is already (partially) preprocessed.
        // Valid values are 'none' (not preprocessed), 'includes' (no #include
        // directives in source), 'modules' (as above plus no module
        // declaration depends on preprocessor, e.g., #ifdef, etc), and 'all'
        // (the source is fully preprocessed). Note that for 'all' the source
        // can still contain comments and line continuations. Note also that
        // for some compilers (e.g., VC) there is no way to signal that the
        // source is already preprocessed.
        //
        // What about header unit imports? Well, they are in a sense
        // standardized precompiled headers so we treat them as includes.
        //
        vp.insert<string>   ("cxx.preprocessed"),

        nullptr, // cxx.features.symexport (set in init() below).

        vp.insert<string>   ("cxx.id"),
        vp.insert<string>   ("cxx.id.type"),
        vp.insert<string>   ("cxx.id.variant"),

        vp.insert<string>   ("cxx.class"),

        &vp.insert<string>   ("cxx.version"),
        &vp.insert<uint64_t> ("cxx.version.major"),
        &vp.insert<uint64_t> ("cxx.version.minor"),
        &vp.insert<uint64_t> ("cxx.version.patch"),
        &vp.insert<string>   ("cxx.version.build"),

        &vp.insert<string>   ("cxx.variant_version"),
        &vp.insert<uint64_t> ("cxx.variant_version.major"),
        &vp.insert<uint64_t> ("cxx.variant_version.minor"),
        &vp.insert<uint64_t> ("cxx.variant_version.patch"),
        &vp.insert<string>   ("cxx.variant_version.build"),

        vp.insert<string>   ("cxx.signature"),
        vp.insert<string>   ("cxx.checksum"),

        vp.insert<string>   ("cxx.pattern"),

        vp.insert<target_triplet> ("cxx.target"),

        vp.insert<string>   ("cxx.target.cpu"),
        vp.insert<string>   ("cxx.target.vendor"),
        vp.insert<string>   ("cxx.target.system"),
        vp.insert<string>   ("cxx.target.version"),
        vp.insert<string>   ("cxx.target.class")
      };

      // Alias some cc. variables as cxx.
      //
      vp.insert_alias (d.c_runtime,     "cxx.runtime");
      vp.insert_alias (d.c_module_name, "cxx.module_name");
      vp.insert_alias (d.c_importable,  "cxx.importable");

      vp.insert_alias (d.c_pkgconfig_include, "cxx.pkgconfig.include");
      vp.insert_alias (d.c_pkgconfig_lib,     "cxx.pkgconfig.lib");

      auto& m (extra.set_module (new config_module (move (d))));
      m.guess (rs, loc, extra.hints);

      return true;
    }

    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 bool,
                 bool,
                 module_init_extra& extra)
    {
      tracer trace ("cxx::config_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cxx.config module must be loaded in project root";

      // Load cxx.guess and share its module instance as ours.
      //
      extra.module = load_module (rs, rs, "cxx.guess", loc, extra.hints);
      extra.module_as<config_module> ().init (rs, loc, extra.hints);

      return true;
    }

    bool
    init (scope& rs,
          scope& bs,
          const location& loc,
          bool,
          bool,
          module_init_extra& extra)
    {
      tracer trace ("cxx::init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cxx module must be loaded in project root";

      // Load cxx.config.
      //
      auto& cm (
        load_module<config_module> (rs, rs, "cxx.config", loc, extra.hints));

      auto& vp (rs.var_pool (true /* public */)); // All qualified.

      bool modules (cast<bool> (rs["cxx.features.modules"]));

      bool symexport (false);
      if (modules)
      {
        auto& var (vp.insert<bool> ("cxx.features.symexport"));
        symexport = cast_false<bool> (rs[var]);
        cm.x_symexport = &var;
      }

      cc::data d {
        cm,

        "cxx.compile",
        "cxx.link",
        "cxx.install",

        cm.x_info->id,
        cm.x_info->class_,
        cm.x_info->version.major,
        cm.x_info->version.minor,
        cm.x_info->variant_version ? cm.x_info->variant_version->major : 0,
        cm.x_info->variant_version ? cm.x_info->variant_version->minor : 0,
        cast<process_path>   (rs[cm.x_path]),
        cast<strings>        (rs[cm.x_mode]),
        cast<target_triplet> (rs[cm.x_target]),
        cm.env_checksum,

        modules,
        symexport,

        cm.iscope,
        cm.iscope_current,

        cast_null<strings> (rs["cc.internal.libs"]),
        cast_null<strings> (rs[cm.x_internal_libs]),

        cast<dir_paths> (rs[cm.x_sys_lib_dirs]),
        cast<dir_paths> (rs[cm.x_sys_hdr_dirs]),
        cm.x_info->sys_mod_dirs ? &cm.x_info->sys_mod_dirs->first : nullptr,

        cm.sys_lib_dirs_mode,
        cm.sys_hdr_dirs_mode,
        cm.sys_mod_dirs_mode,

        cm.sys_lib_dirs_extra,
        cm.sys_hdr_dirs_extra,

        cxx::static_type,
        modules ? &mxx::static_type : nullptr,
        cxx_inc::static_type,
        hdr,
        inc
      };

      auto& m (extra.set_module (new module (move (d), rs)));
      m.init (rs, loc, extra.hints, *cm.x_info);

      return true;
    }

    bool
    objcxx_types_init (scope& rs,
                       scope& bs,
                       const location& loc,
                       bool,
                       bool,
                       module_init_extra&)
    {
      tracer trace ("cxx::objcxx_types_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cxx.objcxx.types module must be loaded in project root";

      // Register the mm{} target type.
      //
      rs.insert_target_type<mm> ();

      return true;
    }

    bool
    objcxx_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 bool,
                 bool,
                 module_init_extra&)
    {
      tracer trace ("cxx::objcxx_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cxx.objcxx module must be loaded in project root";

      module* mod (rs.find_module<module> ("cxx"));

      if (mod == nullptr)
        fail (loc) << "cxx.objcxx module must be loaded after cxx module";

      // Register the target type and "enable" it in the module.
      //
      // Note that we must register the target type regardless of whether the
      // C++ compiler is capable of compiling Objective-C++. But we enable
      // only if it is.
      //
      // Note: see similar code in the c module.
      //
      load_module (rs, rs, "cxx.objcxx.types", loc);

      // Note that while Objective-C++ is supported by MinGW GCC, it's
      // unlikely Clang supports it when targeting MSVC or Emscripten. But
      // let's keep the check simple for now.
      //
      if (mod->ctype == compiler_type::gcc ||
          mod->ctype == compiler_type::clang)
        mod->x_obj = &mm::static_type;

      return true;
    }

    bool
    predefs_init (scope& rs,
                  scope& bs,
                  const location& loc,
                  bool,
                  bool,
                  module_init_extra&)
    {
      tracer trace ("cxx::predefs_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cxx.predefs module must be loaded in project root";

      module* mod (rs.find_module<module> ("cxx"));

      if (mod == nullptr)
        fail (loc) << "cxx.predefs module must be loaded after cxx module";

      // Register the cxx.predefs rule.
      //
      // Why invent a separate module instead of just always registering it in
      // the cxx module? The reason is performance: this rule will be called
      // for every C++ header.
      //
      cc::predefs_rule& r (*mod);

      rs.insert_rule<hxx> (perform_update_id,   r.rule_name, r);
      rs.insert_rule<hxx> (perform_clean_id,    r.rule_name, r);
      rs.insert_rule<hxx> (configure_update_id, r.rule_name, r);

      return true;
    }

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"cxx.types",        nullptr, types_init},
      {"cxx.guess",        nullptr, guess_init},
      {"cxx.config",       nullptr, config_init},
      {"cxx.objcxx.types", nullptr, objcxx_types_init},
      {"cxx.objcxx",       nullptr, objcxx_init},
      {"cxx.predefs",      nullptr, predefs_init},
      {"cxx",              nullptr, init},
      {nullptr,            nullptr, nullptr}
    };

    const module_functions*
    build2_cxx_load ()
    {
      return mod_functions;
    }
  }
}
