// file      : libbuild2/cxx/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cxx/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/cc/guess.hxx>
#include <libbuild2/cc/module.hxx>

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

      // Besides various `c++NN` we have two special values: `latest` and
      // `experimental`.
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

      // Feature flags.
      //
      auto& vp (rs.var_pool ());

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

      switch (cl)
      {
      case compiler_class::msvc:
        {
          // C++ standard-wise, with VC you got what you got up until 14.2.
          // Starting with 14.3 there is now the /std: switch which defaults
          // to c++14 but can be set to c++latest. And from 15.3 it can be
          // c++17. And from 16.?? it can be c++20.
          //
          bool v16_10 (false  /* mj > 19 || (mj == 19 && mi >= 29) */);
          bool v16_0  (v16_10 || mj > 19 || (mj == 19 && mi >= 20));
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
          }
          else if (latest)
          {
            // We used to map `latest` to `c++latest` but starting from 16.1,
            // VC seem to have adopted the "move fast and break things" motto
            // for this mode. So starting from 16 we only enable it in
            // `experimental`.
            //
            if (v16_10)
              o = "/std:c++20";
            else if (v16_0)
              o = "/std:c++17";
            else if (v14_3)
              o = "/std:c++latest";
          }
          else if (v == nullptr)
            ;
          else if (*v != "98" && *v != "03")
          {
            bool sup (false);

            if      (*v == "11") // C++11 since VS2010/10.0.
            {
              sup = mj >= 16;
            }
            else if (*v == "14") // C++14 since VS2015/14.0.
            {
              sup = mj >= 19;
            }
            else if (*v == "17") // C++17 since VS2015/14.0u2.
            {
              // Note: the VC15 compiler version is 19.10.
              //
              sup = (mj > 19 ||
                     (mj == 19 && (mi > 0 || (mi == 0 && p >= 23918))));
            }
            /*
            else if (*v == "20") // C++20 since VS2019/16.??.
            {
              sup = (mj > 19 || (mj == 19 && mi >= 29));
            }
            */

            if (!sup)
              fail << "C++" << *v << " is not supported by " << ci.signature <<
                info << "required by " << project (rs) << '@' << rs;

            if (v15_3)
            {
              if      (*v == "20") o = "/std:c++20";
              else if (*v == "17") o = "/std:c++17";
              else if (*v == "14") o = "/std:c++14";
            }
            else if (v14_3)
            {
              if      (*v == "14") o = "/std:c++14";
              else if (*v == "17") o = "/std:c++latest";
            }
          }

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
                if      (mj >= 11)           o = "-std=c++23"; // 23
                else if (mj >= 8)            o = "-std=c++2a"; // 20
                else if (mj >= 5)            o = "-std=c++1z"; // 17
                else if (mj == 4 && mi >= 8) o = "-std=c++1y"; // 14
                else if (mj == 4 && mi >= 4) o = "-std=c++0x"; // 11

                break;
              }
            case compiler_type::clang:
              {
                // Clang 10.0.0 targeting MSVC 16.4 and 16.5 (preview) in the
                // c++2a mode uncovers some Concepts-related bugs in MSVC STL
                // (LLVM bug #44956). So in this case we map `latest` to
                // c++17.
                //
                // While reportedly this has been fixed in the later versions
                // of MSVC, instead of somehow passing the version of MSVC
                // Clang is targeting, we will just assume that Clang 11
                // and later are used with a sufficiently new version of
                // MSVC.
                //
                if (mj == 10 && latest && tt.system == "win32-msvc")
                {
                  o = "-std=c++17";
                }
                //else if  (mj >= 13)                        o = "-std=c++2b";
                else if  (mj >= 5)                         o = "-std=c++2a";
                else if  (mj >  3 || (mj == 3 && mi >= 5)) o = "-std=c++1z";
                else if  (mj == 3 && mi >= 4)              o = "-std=c++1y";
                else     /* ??? */                         o = "-std=c++0x";

                break;
              }
            case compiler_type::icc:
              {
                if      (mj >= 17)                         o = "-std=c++1z";
                else if (mj >  15 || (mj == 15 && p >= 3)) o = "-std=c++1y";
                else    /* ??? */                          o = "-std=c++0x";

                break;
              }
            default:
              assert (false);
            }
          }
          else if (v == nullptr)
            ;
          else
          {
            // Translate 11 to 0x, 14 to 1y, 17 to 1z, 20 to 2a, and 23 to 2b
            // for compatibility with older versions of the compilers.
            //
            o = "-std=";

            if      (*v == "23") o += "c++2b";
            else if (*v == "20") o += "c++2a";
            else if (*v == "17") o += "c++1z";
            else if (*v == "14") o += "c++1y";
            else if (*v == "11") o += "c++0x";
            else if (*v == "03") o += "c++03";
            else if (*v == "98") o += "c++98";
            else o += *v; // In case the user specifies `gnu++NN` or some such.
          }

          if (!o.empty ())
            prepend (move (o));

          break;
        }
      }

      if (experimental)
      {
        switch (ct)
        {
        case compiler_type::msvc:
          {
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

        // Unless disabled by the user, try to enable C++ modules.
        //
        if (!modules.value || *modules.value)
        {
          switch (ct)
          {
          case compiler_type::msvc:
            {
              // While modules are supported in VC 15.0 (19.10), there is a
              // bug in the separate interface/implementation unit support
              // which makes them pretty much unusable. This has been fixed in
              // 15.3 (19.11). And 15.5 (19.12) supports the `export module
              // M;` syntax. And 16.4 (19.24) supports the global module
              // fragment. And in 16.8 all the modules-related options have
              // been changed. Seeing that the whole thing is unusable anyway,
              // we disable it for 16.8 or later for now.
              //
              if ((mj > 19 || (mj == 19 && mi >= (modules.value ? 10 : 12))) &&
                  (mj < 19 || (mj == 19 && mi < 28) || modules.value))
              {
                prepend (
                  mj > 19  || mi >= 24     ?
                  "/D__cpp_modules=201810" : // p1103 (merged modules)
                  mj == 19 || mi >= 12     ?
                  "/D__cpp_modules=201704" : // p0629r0 (export module M;)
                  "/D__cpp_modules=201703"); // n4647   (       module M;)

                prepend ("/experimental:module");
                modules = true;
              }
              break;
            }
          case compiler_type::gcc:
            {
              // We use the module mapper support which is only available
              // since GCC 11. And since we are not yet capable of supporting
              // generated headers via the mapper, we require the user to
              // explicitly request modules.
              //
              if (mj >= 11 && modules.value)
              {
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
              // At the time of this writing, support for C++20 modules in
              // Clang is incomplete. And starting with Clang 9 (Apple Clang
              // 11.0.3), they are enabled by default in the C++2a mode which
              // breaks the way we set things up for partial preprocessing;
              // see this post for details:
              //
              // http://lists.llvm.org/pipermail/cfe-dev/2019-October/063637.html
              //
              // As a result, for now, we only enable modules if forced with
              // explicit cxx.features.modules=true.
              //
              // Also see Clang modules support hack in cc::compile.
              //
              if (modules.value)
              {
                prepend ("-D__cpp_modules=201704"); // p0629r0
                mode.push_back ("-fmodules-ts"); // For the hack to work.
                modules = true;
              }

              break;
            }
          case compiler_type::icc:
            break; // No modules support yet.
          }
        }
      }

      set_feature (modules);
      //set_feature (concepts);
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
      auto& vp (rs.var_pool ());

      cc::config_data d {
        cc::lang::cxx,

        "cxx",
        "c++",
        BUILD2_DEFAULT_CXX,
        ".ii",

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

        vp.insert<string> ("cxx.internal.scope"),

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

        vp.insert<string> ("cxx.stdlib"),

        vp["cc.runtime"],
        vp["cc.stdlib"],

        vp["cc.type"],
        vp["cc.system"],
        vp["cc.module_name"],
        vp["cc.importable"],
        vp["cc.reprocess"],

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

    static const target_type* const hdr[] =
    {
      &hxx::static_type,
      &ixx::static_type,
      &txx::static_type,
      &mxx::static_type,
      nullptr
    };

    static const target_type* const inc[] =
    {
      &hxx::static_type,
      &h::static_type,
      &ixx::static_type,
      &txx::static_type,
      &mxx::static_type,
      &cxx::static_type,
      &c::static_type,
      nullptr
    };

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

      auto& vp (rs.var_pool ());

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
        "cxx.uninstall",

        cm.x_info->id.type,
        cm.x_info->id.variant,
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

        cm.internal_scope,
        cm.internal_scope_current,

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
        hdr,
        inc
      };

      auto& m (extra.set_module (new module (move (d))));
      m.init (rs, loc, extra.hints, *cm.x_info);

      return true;
    }

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"cxx.guess",  nullptr, guess_init},
      {"cxx.config", nullptr, config_init},
      {"cxx",        nullptr, init},
      {nullptr,      nullptr, nullptr}
    };

    const module_functions*
    build2_cxx_load ()
    {
      return mod_functions;
    }
  }
}
