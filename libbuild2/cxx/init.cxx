// file      : libbuild2/cxx/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cxx/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/diagnostics.hxx>

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

      virtual strings
      translate_std (const compiler_info&,
                     scope&,
                     const string*) const override;
    };

    using cc::module;

    strings config_module::
    translate_std (const compiler_info& ci, scope& rs, const string* v) const
    {
      strings r;

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
      // deal with them, for example, using feature test macros), can
      // reasonably expect to work. In particular, this is the value we use by
      // default in projects created by bdep-new(1) as well as to build the
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
      auto enter = [&rs] (const char* v) -> const variable&
      {
        return rs.var_pool ().insert<bool> (v, variable_visibility::project);
      };

      //bool concepts (false); auto& v_c (enter ("cxx.features.concepts"));
      bool modules (false); auto& v_m (enter ("cxx.features.modules"));

      // NOTE: see also module sidebuild subproject if changing anything about
      // modules here.

      string o;

      switch (cl)
      {
      case compiler_class::msvc:
        {
          // C++ standard-wise, with VC you got what you got up until 14.2.
          // Starting with 14.3 there is now the /std: switch which defaults
          // to c++14 but can be set to c++latest. And from 15.3 it can be
          // c++17.
          //
          bool v16_0 (         mj > 19 || (mj == 19 && mi >= 20));
          bool v15_3 (v16_0 || (mj == 19 && mi >= 11));
          bool v14_3 (v15_3 || (mj == 19 && (mi > 0 || (mi == 0 && p >= 24215))));

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
            if (v16_0)
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

            if (!sup)
              fail << "C++" << *v << " is not supported by " << ci.signature <<
                info << "required by " << project (rs) << '@' << rs;

            if (v15_3)
            {
              if      (*v == "14") o = "/std:c++14";
              else if (*v == "17") o = "/std:c++17";
            }
            else if (v14_3)
            {
              if      (*v == "14") o = "/std:c++14";
              else if (*v == "17") o = "/std:c++latest";
            }
          }

          if (!o.empty ())
            r.push_back (move (o));

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
                if      (mj >= 8)            o = "-std=c++2a"; // 20
                else if (mj >= 5)            o = "-std=c++1z"; // 17
                else if (mj == 4 && mi >= 8) o = "-std=c++1y"; // 14
                else if (mj == 4 && mi >= 4) o = "-std=c++0x"; // 11

                break;
              }
            case compiler_type::clang:
              {
                if       (mj >= 5)                         o = "-std=c++2a";
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
            r.push_back (move (o));

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
              r.push_back ("/permissive-");

            break;
          }
        default:
          break;
        }

        // Unless disabled by the user, try to enable C++ modules.
        //
        lookup l;
        if (!(l = rs[v_m]) || cast<bool> (l))
        {
          switch (ct)
          {
          case compiler_type::msvc:
            {
              // While modules are supported in VC15.0 (19.10), there is a bug
              // in separate interface/implementation unit support which makes
              // them pretty much unusable. This has been fixed in VC15.3
              // (19.11). And VC15.5 (19.12) supports the 'export module M;'
              // syntax.
              //
              if (mj > 19 || (mj == 19 && mi >= (l ? 10 : 12)))
              {
                r.push_back (
                  mj > 19 || mi > 11
                  ? "/D__cpp_modules=201704"   // p0629r0 (export module M;)
                  : "/D__cpp_modules=201703"); // n4647   (       module M;)

                r.push_back ("/experimental:module");
                modules = true;
              }
              break;
            }
          case compiler_type::gcc:
            {
              // We now use extended/experimental module mapper support which
              // is currently only available in our c++-modules-ex branch.
              // But let's allow forcing it to plain c++-modules in case
              // things got merged or the user feels adventurous.
              //
              if (mj >= 10 &&
                  ci.version.build.find (l
                                         ? "c++-modules"
                                         : "c++-modules-ex") != string::npos)
              {
                // Currently defines __cpp_modules=201810 which is said to
                // correspond to p1103 (merged modules).
                //
                r.push_back ("-fmodules-ts");
                modules = true;
              }
              break;
            }
          case compiler_type::clang:
            {
              // Enable starting with Clang 6.0.0.
              //
              // Note that we are using Apple to vanilla Clang version re-map
              // from above so may need to update things there as well.
              //
              // Also see Clang modules support hack in cc::compile.
              //
              // @@ Clang 9 enables modules by default in C++2a. We should
              //    probably reflect this in the modules value.
              //
              if (mj >= 6)
              {
                r.push_back ("-D__cpp_modules=201704"); // p0629r0
                r.push_back ("-fmodules-ts");
                modules = true;
              }
              break;
            }
          case compiler_type::icc:
            break; // No modules support yet.
          }
        }
      }

      rs.assign (v_m) = modules;
      //rs.assign (v_c) = concepts;

      return r;
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
      if (&rs != &bs)
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

        // Note: some overridable, some not.
        //
        // NOTE: remember to update documentation if changing anything here.
        //
        vp.insert<strings> ("config.cxx",          true),
        vp.insert<string>  ("config.cxx.id",       true),
        vp.insert<string>  ("config.cxx.version",  true),
        vp.insert<string>  ("config.cxx.target",   true),
        vp.insert<string>  ("config.cxx.std",      true),
        vp.insert<strings> ("config.cxx.poptions", true),
        vp.insert<strings> ("config.cxx.coptions", true),
        vp.insert<strings> ("config.cxx.loptions", true),
        vp.insert<strings> ("config.cxx.aoptions", true),
        vp.insert<strings> ("config.cxx.libs",     true),

        // List of translatable headers. Inclusions of such headers are
        // translated to the corresponding header unit imports.
        //
        // A header can be specified either as an absolute and normalized path
        // or as a <>-style include name. The latter kind is automatically
        // translated to the absolute form based on the compiler's system (as
        // opposed to -I) header search paths. Note also that all entries must
        // be specified before loading the cxx module.
        //
        &vp.insert<strings> ("config.cxx.translatable_headers", true),

        vp.insert<process_path> ("cxx.path"),
        vp.insert<strings>      ("cxx.mode"),
        vp.insert<dir_paths>    ("cxx.sys_lib_dirs"),
        vp.insert<dir_paths>    ("cxx.sys_inc_dirs"),

        vp.insert<string>   ("cxx.std", variable_visibility::project),

        vp.insert<strings>  ("cxx.poptions"),
        vp.insert<strings>  ("cxx.coptions"),
        vp.insert<strings>  ("cxx.loptions"),
        vp.insert<strings>  ("cxx.aoptions"),
        vp.insert<strings>  ("cxx.libs"),

        &vp.insert<strings> ("cxx.translatable_headers"),

        vp["cc.poptions"],
        vp["cc.coptions"],
        vp["cc.loptions"],
        vp["cc.aoptions"],
        vp["cc.libs"],

        vp.insert<strings>      ("cxx.export.poptions"),
        vp.insert<strings>      ("cxx.export.coptions"),
        vp.insert<strings>      ("cxx.export.loptions"),
        vp.insert<vector<name>> ("cxx.export.libs"),

        vp["cc.export.poptions"],
        vp["cc.export.coptions"],
        vp["cc.export.loptions"],
        vp["cc.export.libs"],

        vp.insert<string> ("cxx.stdlib"),

        vp["cc.runtime"],
        vp["cc.stdlib"],

        vp["cc.type"],
        vp["cc.system"],
        vp["cc.module_name"],
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
      if (&rs != &bs)
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
      if (&rs != &bs)
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
        auto& var (vp.insert<bool> ("cxx.features.symexport",
                                    variable_visibility::project));
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
        cast<process_path>   (rs[cm.x_path]),
        cast<strings>        (rs[cm.x_mode]),
        cast<target_triplet> (rs[cm.x_target]),

        cm.tstd,

        modules,
        symexport,

        cast<dir_paths> (rs[cm.x_sys_lib_dirs]),
        cast<dir_paths> (rs[cm.x_sys_inc_dirs]),
        cm.x_info->sys_mod_dirs ? &cm.x_info->sys_mod_dirs->first : nullptr,

        cm.sys_lib_dirs_mode,
        cm.sys_inc_dirs_mode,
        cm.sys_mod_dirs_mode,

        cm.sys_lib_dirs_extra,
        cm.sys_inc_dirs_extra,

        cxx::static_type,
        modules ? &mxx::static_type : nullptr,
        hdr,
        inc
      };

      auto& m (extra.set_module (new module (move (d))));
      m.init (rs, loc, extra.hints);

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
