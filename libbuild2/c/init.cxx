// file      : libbuild2/c/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/c/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/install/utility.hxx>

#include <libbuild2/cc/guess.hxx>
#include <libbuild2/cc/module.hxx>

#include <libbuild2/cc/target.hxx> // pc*
#include <libbuild2/c/target.hxx>

#ifndef BUILD2_DEFAULT_C
#  ifdef BUILD2_NATIVE_C
#    define BUILD2_DEFAULT_C BUILD2_NATIVE_C
#  else
#    define BUILD2_DEFAULT_C ""
#  endif
#endif

using namespace std;
using namespace butl;

namespace build2
{
  namespace c
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
                   const target_triplet&,
                   scope& rs,
                   strings& mode,
                   const string* v) const
    {
      // The standard is `NN` but can also be `gnuNN`.

      // This helper helps recognize both NN and [cC]NN to avoid an endless
      // stream of user questions. It can also be used to recognize Nx in
      // addition to NN (e.g., "23" and "2x").
      //
      auto stdcmp = [v] (const char* nn, const char* nx = nullptr)
      {
        if (v != nullptr)
        {
          const char* s (v->c_str ());
          if (s[0] == 'c' || s[0] == 'C')
            s += 1;

          return strcmp (s, nn) == 0 || (nx != nullptr && strcmp (s, nx) == 0);
        }

        return false;
      };

      switch (ci.class_)
      {
      case compiler_class::msvc:
        {
          // Standard-wise, with VC you get what you get. The question is
          // whether we should verify that the requested standard is provided
          // by this VC version. And if so, from which version should we say
          // VC supports 90, 99, and 11? We should probably be as loose as
          // possible here since the author will always be able to tighten
          // (but not loosen) this in the buildfile (i.e., detect unsupported
          // versions).
          //
          // The state of affairs seem to be (from Herb Sutter's blog):
          //
          // 10.0 - most of C95 plus a few C99 features
          // 11.0 - partial support for the C++11 subset of C11
          // 12.0 - more C11 features from the C++11 subset, most of C99
          //
          // So let's say C99 is supported from 10.0 and C11 from 11.0. And
          // C90 is supported by everything we care to support.
          //
          // C17/18 is a bug-fix version of C11 so here we assume it is the
          // same as C11.
          //
          // And it's still early days for C2X. Specifically, there is not
          // much about C2X in MSVC in the official places and the following
          // page shows that it's pretty much unimplement at the time of the
          // MSVC 17.6 release:
          //
          // https://en.cppreference.com/w/c/compiler_support/23
          //
          // From version 16.8 VC now supports /std:c11 and /std:c17 options
          // which enable C11/17 conformance. However, as of version 16.10,
          // neither SDK nor CRT can be compiled in these modes (see the /std
          // option documentation for details/updates). There is also now
          // /std:clatest which can be used to enable C23 typeof as of MSVC
          // 17.9. So let's map C23 to that.
          //
          if (v == nullptr)
            ;
          else if (!stdcmp ("90"))
          {
            uint64_t mj (ci.version.major);
            uint64_t mi (ci.version.minor);

            if      (stdcmp ("99") && mj >= 16) // Since VS2010/10.0.
              ;
            else if ((stdcmp ("11") ||
                      stdcmp ("17") ||
                      stdcmp ("18")) && mj >= 18) // Since VS????/11.0.
              ;
            else if (stdcmp ("23", "2x") &&
                     (mj > 19 || (mj == 19 && mi >= 39))) // Since 17.9.
            {
              mode.insert (mode.begin (), "/std:clatest");
            }
            else
              fail << "C " << *v << " is not supported by " << ci.signature <<
                info << "required by " << project (rs) << '@' << rs;
          }
          break;
        }
      case compiler_class::gcc:
        {
          // 90 and 89 are the same standard. Translate 99 to 9x and 11 to 1x
          // for compatibility with older versions of the compilers.
          //
          if (v == nullptr)
            ;
          else
          {
            string o ("-std=");

            if      (stdcmp ("23", "2x")) o += "c2x"; // GCC 9, Clang 9 (8?).
            else if (stdcmp ("17") ||
                     stdcmp ("18"))       o += "c17"; // GCC 8, Clang 6.
            else if (stdcmp ("11"))       o += "c1x";
            else if (stdcmp ("99"))       o += "c9x";
            else if (stdcmp ("90"))       o += "c90";
            else o += *v; // In case the user specifies `gnuNN` or some such.

            mode.insert (mode.begin (), move (o));
          }
          break;
        }
      }
    }

    // See cc::data::x_{hdr,inc} for background.
    //
    static const target_type* const hdr[] =
    {
      &h::static_type,
      nullptr
    };

    // Note that we include S{} here because .S files can include each other.
    // (And maybe from inline assembler instructions?)
    //
    static const target_type* const inc[] =
    {
      &h::static_type,
      &c::static_type,
      &m::static_type,
      &S::static_type,
      &c_inc::static_type,
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
      tracer trace ("c::types_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "c.types module must be loaded in project root";

      // Register target types and configure their "installability".
      //
      using namespace install;

      bool install_loaded (cast_false<bool> (rs["install.loaded"]));

      // Note: not registering m{} or S{} (they are registered seperately
      // by the respective optional .types submodules).
      //
      rs.insert_target_type<c> ();

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

      // @@ PERF: maybe factor this to cc.types?
      //
      rs.insert_target_type<cc::pc> ();
      rs.insert_target_type<cc::pca> ();
      rs.insert_target_type<cc::pcs> ();

      if (install_loaded)
        install_path<cc::pc> (rs, dir_path ("pkgconfig"));

      return true;
    }

    static const char* const hinters[] = {"cxx", nullptr};

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
      tracer trace ("c::guess_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "c.guess module must be loaded in project root";

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
        cc::lang::c,

        "c",
        "c",
        "obj-c",
        BUILD2_DEFAULT_C,
        ".i",
        ".mi",

        hinters,

        vp["bin.binless"],

        // NOTE: remember to update documentation if changing anything here.
        //
        vp.insert<strings> ("config.c"),
        vp.insert<string>  ("config.c.id"),
        vp.insert<string>  ("config.c.version"),
        vp.insert<string>  ("config.c.target"),
        vp.insert<string>  ("config.c.std"),
        vp.insert<strings> ("config.c.poptions"),
        vp.insert<strings> ("config.c.coptions"),
        vp.insert<strings> ("config.c.loptions"),
        vp.insert<strings> ("config.c.aoptions"),
        vp.insert<strings> ("config.c.libs"),

        // See config.cxx.internal.scope for details.
        //
        vp.insert<string> ("config.c.internal.scope"),

        nullptr /* config.c.translate_include */,

        vp.insert<process_path_ex> ("c.path"),
        vp.insert<strings>         ("c.mode"),
        vp.insert<path>            ("c.config.path"),
        vp.insert<strings>         ("c.config.mode"),
        vp.insert<dir_paths>       ("c.sys_lib_dirs"),
        vp.insert<dir_paths>       ("c.sys_hdr_dirs"),

        vp.insert<string>       ("c.std"),

        vp.insert<strings>      ("c.poptions"),
        vp.insert<strings>      ("c.coptions"),
        vp.insert<strings>      ("c.loptions"),
        vp.insert<strings>      ("c.aoptions"),
        vp.insert<strings>      ("c.libs"),

        vp.insert<string>  ("c.internal.scope"),
        vp.insert<strings> ("c.internal.libs"),

        nullptr /* c.translate_include */,

        vp["cc.poptions"],
        vp["cc.coptions"],
        vp["cc.loptions"],
        vp["cc.aoptions"],
        vp["cc.libs"],

        vp.insert<strings>      ("c.export.poptions"),
        vp.insert<strings>      ("c.export.coptions"),
        vp.insert<strings>      ("c.export.loptions"),
        vp.insert<vector<name>> ("c.export.libs"),
        vp.insert<vector<name>> ("c.export.impl_libs"),

        vp["cc.export.poptions"],
        vp["cc.export.coptions"],
        vp["cc.export.loptions"],
        vp["cc.export.libs"],
        vp["cc.export.impl_libs"],

        vp["cc.pkgconfig.include"],
        vp["cc.pkgconfig.lib"],

        vp.insert_alias (vp["cc.stdlib"], "c.stdlib"), // Same as cc.stdlib.

        vp["cc.runtime"],
        vp["cc.stdlib"],

        vp["cc.type"],
        vp["cc.system"],
        vp["cc.module_name"],
        vp["cc.importable"],
        vp["cc.reprocess"],
        vp["cc.serialize"],

        vp.insert<string>   ("c.preprocessed"), // See cxx.preprocessed.
        nullptr,                                // No __symexport (no modules).

        vp.insert<string>   ("c.id"),
        vp.insert<string>   ("c.id.type"),
        vp.insert<string>   ("c.id.variant"),

        vp.insert<string>   ("c.class"),

        &vp.insert<string>   ("c.version"),
        &vp.insert<uint64_t> ("c.version.major"),
        &vp.insert<uint64_t> ("c.version.minor"),
        &vp.insert<uint64_t> ("c.version.patch"),
        &vp.insert<string>   ("c.version.build"),

        &vp.insert<string>   ("c.variant_version"),
        &vp.insert<uint64_t> ("c.variant_version.major"),
        &vp.insert<uint64_t> ("c.variant_version.minor"),
        &vp.insert<uint64_t> ("c.variant_version.patch"),
        &vp.insert<string>   ("c.variant_version.build"),

        vp.insert<string>   ("c.signature"),
        vp.insert<string>   ("c.checksum"),

        vp.insert<string>   ("c.pattern"),

        vp.insert<target_triplet> ("c.target"),

        vp.insert<string>   ("c.target.cpu"),
        vp.insert<string>   ("c.target.vendor"),
        vp.insert<string>   ("c.target.system"),
        vp.insert<string>   ("c.target.version"),
        vp.insert<string>   ("c.target.class")
      };

      // Alias some cc. variables as c.
      //
      vp.insert_alias (d.c_runtime,    "c.runtime");
      vp.insert_alias (d.c_importable, "c.importable");

      vp.insert_alias (d.c_pkgconfig_include, "c.pkgconfig.include");
      vp.insert_alias (d.c_pkgconfig_lib,     "c.pkgconfig.lib");

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
      tracer trace ("c::config_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "c.config module must be loaded in project root";

      // Load c.guess and share its module instance as ours.
      //
      extra.module = load_module (rs, rs, "c.guess", loc, extra.hints);
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
      tracer trace ("c::init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "c module must be loaded in project root";

      // Load c.config.
      //
      auto& cm (
        load_module<config_module> (rs, rs, "c.config", loc, extra.hints));

      cc::data d {
        cm,

        "c.compile",
        "c.link",
        "c.install",

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

        false, // No C modules yet.
        false, // No __symexport support since no modules.

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

        c::static_type,
        nullptr,        // No C modules yet.
        c_inc::static_type,
        hdr,
        inc
      };

      auto& m (extra.set_module (new module (move (d), rs)));
      m.init (rs, loc, extra.hints, *cm.x_info);

      return true;
    }

    bool
    objc_types_init (scope& rs,
                     scope& bs,
                     const location& loc,
                     bool,
                     bool,
                     module_init_extra&)
    {
      tracer trace ("c::objc_types_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "c.objc.types module must be loaded in project root";

      // Register the m{} target type.
      //
      rs.insert_target_type<m> ();

      return true;
    }

    bool
    objc_init (scope& rs,
               scope& bs,
               const location& loc,
               bool,
               bool,
               module_init_extra&)
    {
      tracer trace ("c::objc_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "c.objc module must be loaded in project root";

      module* mod (rs.find_module<module> ("c"));

      if (mod == nullptr)
        fail (loc) << "c.objc module must be loaded after c module";

      // Register the target type and "enable" it in the module.
      //
      // Note that we must register the target type regardless of whether the
      // C compiler is capable of compiling Objective-C. But we enable only
      // if it is.
      //
      // Note: see similar code in the cxx module.
      //
      load_module (rs, rs, "c.objc.types", loc);

      // Note that while Objective-C is supported by MinGW GCC, it's unlikely
      // Clang supports it when targeting MSVC or Emscripten. But let's keep
      // the check simple for now.
      //
      if (mod->ctype == compiler_type::gcc ||
          mod->ctype == compiler_type::clang)
        mod->x_obj = &m::static_type;

      return true;
    }

    bool
    as_cpp_types_init (scope& rs,
                       scope& bs,
                       const location& loc,
                       bool,
                       bool,
                       module_init_extra&)
    {
      tracer trace ("c::as_cpp_types_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "c.as-cpp.types module must be loaded in project root";

      // Register the S{} target type.
      //
      rs.insert_target_type<S> ();

      return true;
    }

    bool
    as_cpp_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 bool,
                 bool,
                 module_init_extra&)
    {
      tracer trace ("c::as_cpp_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "c.as-cpp module must be loaded in project root";

      module* mod (rs.find_module<module> ("c"));

      if (mod == nullptr)
        fail (loc) << "c.as-cpp module must be loaded after c module";

      // Register the target type and "enable" it in the module.
      //
      // Note that we must register the target type regardless of whether the
      // C compiler is capable of compiling Assember with C preprocessor. But
      // we enable only if it is.
      //
      load_module (rs, rs, "c.as-cpp.types", loc);

      if (mod->ctype == compiler_type::gcc ||
          mod->ctype == compiler_type::clang)
        mod->x_asp = &S::static_type;

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
      tracer trace ("c::predefs_init");
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "c.predefs module must be loaded in project root";

      module* mod (rs.find_module<module> ("c"));

      if (mod == nullptr)
        fail (loc) << "c.predefs module must be loaded after c module";

      // Register the c.predefs rule.
      //
      // Why invent a separate module instead of just always registering it in
      // the c module? The reason is performance: this rule will be called for
      // every C header.
      //
      cc::predefs_rule& r (*mod);

      rs.insert_rule<h> (perform_update_id,   r.rule_name, r);
      rs.insert_rule<h> (perform_clean_id,    r.rule_name, r);
      rs.insert_rule<h> (configure_update_id, r.rule_name, r);

      return true;
    }

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"c.types",        nullptr, types_init},
      {"c.guess",        nullptr, guess_init},
      {"c.config",       nullptr, config_init},
      {"c.objc.types",   nullptr, objc_types_init},
      {"c.objc",         nullptr, objc_init},
      {"c.as-cpp.types", nullptr, as_cpp_types_init},
      {"c.as-cpp",       nullptr, as_cpp_init},
      {"c.predefs",      nullptr, predefs_init},
      {"c",              nullptr, init},
      {nullptr,          nullptr, nullptr}
    };

    const module_functions*
    build2_c_load ()
    {
      return mod_functions;
    }
  }
}
