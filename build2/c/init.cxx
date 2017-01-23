// file      : build2/c/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/c/init>

#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/cc/guess>
#include <build2/cc/module>

#include <build2/c/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace c
  {
    using cc::compiler_info;

    class config_module: public cc::config_module
    {
    public:
      explicit
      config_module (config_data&& d)
          : config_data (move (d)), cc::config_module (move (d)) {}

      string
      translate_std (const compiler_info&,
                     scope&,
                     const string&) const override;
    };

    using cc::module;

    string config_module::
    translate_std (const compiler_info& ci, scope& rs, const string& v) const
    {
      string r;

      if (ci.id.type == "msvc")
      {
        // Standard-wise, with VC you get what you get. The question is
        // whether we should verify that the requested standard is provided by
        // this VC version. And if so, from which version should we say VC
        // supports 90, 99, and 11? We should probably be as loose as possible
        // here since the author will always be able to tighten (but not
        // loosen) this in the buildfile (i.e., detect unsupported versions).
        //
        // The state of affairs seem to be (from Herb Sutter's blog):
        //
        // 10.0 - most of C95 plus a few C99 features
        // 11.0 - partial support for the C++11 subset of C11
        // 12.0 - more C11 features from the C++11 subset, most of C99
        //
        // So let's say C99 is supported from 10.0 and C11 from 11.0. And C90
        // is supported by everything we care to support.
        //
        if (v != "90")
        {
          uint64_t cver (ci.version.major);

          if ((v == "99" && cver < 16) || // Since VS2010/10.0.
              (v == "11" && cver < 17))   // Since VS2012/11.0.
          {
            fail << "C" << v << " is not supported by " << ci.signature <<
              info << "required by " << project (rs) << '@' << rs.out_path ();
          }
        }
      }
      else
      {
        // 90 and 89 are the same standard. Translate 99 to 9x and 11 to 1x
        // for compatibility with older versions of the compilers.
        //
        r = "-std=";

        if (v == "90")
          r += "c90";
        else if (v == "99")
          r += "c9x";
        else if (v == "11")
          r += "c1x";
        else
          r += v; // In case the user specifies something like 'gnu11'.
      }

      return r;
    }

    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 unique_ptr<module_base>& mod,
                 bool,
                 bool,
                 const variable_map& hints)
    {
      tracer trace ("c::config_init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      // We only support root loading (which means there can only be one).
      //
      if (&rs != &bs)
        fail (loc) << "c.config module must be loaded in project root";

      // Load cc.core.vars so that we can cache all the cc.* variables.
      //
      if (!cast_false<bool> (rs["cc.core.vars.loaded"]))
        load_module ("cc.core.vars", rs, rs, loc);

      // Enter all the variables and initialize the module data.
      //
      auto& v (var_pool);

      cc::config_data d {
        cc::lang::c,

        "c",
        "c",
        "gcc",

        // Note: some overridable, some not.
        //
        v.insert<path>    ("config.c",          true),
        v.insert<strings> ("config.c.poptions", true),
        v.insert<strings> ("config.c.coptions", true),
        v.insert<strings> ("config.c.loptions", true),
        v.insert<strings> ("config.c.libs",     true),

        v.insert<process_path> ("c.path"),
        v.insert<dir_paths>    ("c.sys_lib_dirs"),
        v.insert<dir_paths>    ("c.sys_inc_dirs"),

        v.insert<strings>      ("c.poptions"),
        v.insert<strings>      ("c.coptions"),
        v.insert<strings>      ("c.loptions"),
        v.insert<strings>      ("c.libs"),

        v["cc.poptions"],
        v["cc.coptions"],
        v["cc.loptions"],
        v["cc.libs"],

        v.insert<strings>      ("c.export.poptions"),
        v.insert<strings>      ("c.export.coptions"),
        v.insert<strings>      ("c.export.loptions"),
        v.insert<vector<name>> ("c.export.libs"),

        v["cc.export.poptions"],
        v["cc.export.coptions"],
        v["cc.export.loptions"],
        v["cc.export.libs"],

        v["cc.type"],
        v["cc.system"],

        v.insert<string>   ("c.std", variable_visibility::project),

        v.insert<string>   ("c.id"),
        v.insert<string>   ("c.id.type"),
        v.insert<string>   ("c.id.variant"),

        v.insert<string>   ("c.version"),
        v.insert<uint64_t> ("c.version.major"),
        v.insert<uint64_t> ("c.version.minor"),
        v.insert<uint64_t> ("c.version.patch"),
        v.insert<string>   ("c.version.build"),

        v.insert<string>   ("c.signature"),
        v.insert<string>   ("c.checksum"),

        v.insert<target_triplet> ("c.target"),

        v.insert<string>   ("c.target.cpu"),
        v.insert<string>   ("c.target.vendor"),
        v.insert<string>   ("c.target.system"),
        v.insert<string>   ("c.target.version"),
        v.insert<string>   ("c.target.class")
      };

      assert (mod == nullptr);
      config_module* m;
      mod.reset (m = new config_module (move (d)));
      m->init (rs, loc, hints);
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
          unique_ptr<module_base>& mod,
          bool,
          bool,
          const variable_map& hints)
    {
      tracer trace ("c::init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      // We only support root loading (which means there can only be one).
      //
      if (&rs != &bs)
        fail (loc) << "c module must be loaded in project root";

      // Load c.config.
      //
      if (!cast_false<bool> (rs["c.config.loaded"]))
        load_module ("c.config", rs, rs, loc, false, hints);

      config_module& cm (*rs.modules.lookup<config_module> ("c.config"));

      cc::data d {
        cm,

        "c.compile",
        "c.link",
        "c.install",
        "c.uninstall",

        cast<string>         (rs[cm.x_id]),
        cast<target_triplet> (rs[cm.x_target]),

        cm.tstd,

        cast_null<process_path> (rs["pkgconfig.path"]),
        cast<dir_paths>         (rs[cm.x_sys_lib_dirs]),
        cast<dir_paths>         (rs[cm.x_sys_inc_dirs]),

        c::static_type,
        hdr,
        inc
      };

      assert (mod == nullptr);
      module* m;
      mod.reset (m = new module (move (d)));
      m->init (rs, loc, hints);
      return true;
    }
  }
}
