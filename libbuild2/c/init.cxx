// file      : libbuild2/c/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/c/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/cc/guess.hxx>
#include <libbuild2/cc/module.hxx>

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
    using cc::compiler_class;
    using cc::compiler_info;

    class config_module: public cc::config_module
    {
    public:
      explicit
      config_module (config_data&& d): cc::config_module (move (d)) {}

      virtual strings
      translate_std (const compiler_info&,
                     const target_triplet&,
                     scope&,
                     const string*) const override;
    };

    using cc::module;

    strings config_module::
    translate_std (const compiler_info& ci,
                   const target_triplet&,
                   scope& rs,
                   const string* v) const
    {
      strings r;

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
          // And it's still early days for C2X.
          //
          if (v == nullptr)
            ;
          else if (*v != "90")
          {
            uint64_t cver (ci.version.major);

            if ((*v == "99"   && cver < 16) ||  // Since VS2010/10.0.
                ((*v == "11" ||
                  *v == "17" ||
                  *v == "18") && cver < 18) ||
                (*v == "2x"               ))
            {
              fail << "C" << *v << " is not supported by " << ci.signature <<
                info << "required by " << project (rs) << '@' << rs;
            }
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

            if      (*v == "2x") o += "c2x"; // GCC 9, Clang 9 (8?).
            else if (*v == "17" ||
                     *v == "18") o += "c17"; // GCC 8, Clang 6.
            else if (*v == "11") o += "c1x";
            else if (*v == "99") o += "c9x";
            else if (*v == "90") o += "c90";
            else o += *v; // In case the user specifies `gnuNN` or some such.

            r.push_back (move (o));
          }
          break;
        }
      }

      return r;
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
      auto& vp (rs.var_pool ());

      cc::config_data d {
        cc::lang::c,

        "c",
        "c",
        BUILD2_DEFAULT_C,
        ".i",

        hinters,

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
        nullptr          /* config.c.translatable_headers */,

        vp.insert<process_path> ("c.path"),
        vp.insert<strings>      ("c.mode"),
        vp.insert<dir_paths>    ("c.sys_lib_dirs"),
        vp.insert<dir_paths>    ("c.sys_inc_dirs"),

        vp.insert<string>       ("c.std"),

        vp.insert<strings>      ("c.poptions"),
        vp.insert<strings>      ("c.coptions"),
        vp.insert<strings>      ("c.loptions"),
        vp.insert<strings>      ("c.aoptions"),
        vp.insert<strings>      ("c.libs"),

        nullptr                /* c.translatable_headers */,

        vp["cc.poptions"],
        vp["cc.coptions"],
        vp["cc.loptions"],
        vp["cc.aoptions"],
        vp["cc.libs"],

        vp.insert<strings>      ("c.export.poptions"),
        vp.insert<strings>      ("c.export.coptions"),
        vp.insert<strings>      ("c.export.loptions"),
        vp.insert<vector<name>> ("c.export.libs"),

        vp["cc.export.poptions"],
        vp["cc.export.coptions"],
        vp["cc.export.loptions"],
        vp["cc.export.libs"],

        vp.insert_alias (vp["cc.stdlib"], "c.stdlib"), // Same as cc.stdlib.

        vp["cc.runtime"],
        vp["cc.stdlib"],

        vp["cc.type"],
        vp["cc.system"],
        vp["cc.module_name"],
        vp["cc.reprocess"],

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
      vp.insert_alias (d.c_runtime, "c.runtime");

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

    static const target_type* const hdr[] =
    {
      &h::static_type,
      nullptr
    };

    static const target_type* const inc[] =
    {
      &h::static_type,
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
        "c.uninstall",

        cm.x_info->id.type,
        cm.x_info->id.variant,
        cm.x_info->class_,
        cm.x_info->version.major,
        cm.x_info->version.minor,
        cast<process_path>   (rs[cm.x_path]),
        cast<strings>        (rs[cm.x_mode]),
        cast<target_triplet> (rs[cm.x_target]),

        cm.tstd,

        false, // No C modules yet.
        false, // No __symexport support since no modules.

        cast<dir_paths> (rs[cm.x_sys_lib_dirs]),
        cast<dir_paths> (rs[cm.x_sys_inc_dirs]),
        cm.x_info->sys_mod_dirs ? &cm.x_info->sys_mod_dirs->first : nullptr,

        cm.sys_lib_dirs_mode,
        cm.sys_inc_dirs_mode,
        cm.sys_mod_dirs_mode,

        cm.sys_lib_dirs_extra,
        cm.sys_inc_dirs_extra,

        c::static_type,
        nullptr,        // No C modules yet.
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

      {"c.guess",  nullptr, guess_init},
      {"c.config", nullptr, config_init},
      {"c",        nullptr, init},
      {nullptr,    nullptr, nullptr}
    };

    const module_functions*
    build2_c_load ()
    {
      return mod_functions;
    }
  }
}
