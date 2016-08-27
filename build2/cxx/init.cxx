// file      : build2/cxx/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/init>

#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/cc/guess>
#include <build2/cc/module>

#include <build2/cxx/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cxx
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
        // C++ standard-wise, with VC you get what you get. The question is
        // whether we should verify that the requested standard is provided by
        // this VC version. And if so, from which version should we say VC
        // supports 11, 14, and 17? We should probably be as loose as possible
        // here since the author will always be able to tighten (but not
        // loosen) this in the buildfile (i.e., detect unsupported versions).
        //
        // For now we are not going to bother doing this for C++03.
        //
        if (v != "98" && v != "03")
        {
          uint64_t cver (ci.version.major);

          // @@ Is mapping for 14 and 17 correct? Maybe Update 2 for 14?
          //
          if ((v == "11" && cver < 16) || // C++11 since VS2010/10.0.
              (v == "14" && cver < 19) || // C++14 since VS2015/14.0.
              (v == "17" && cver < 20))   // C++17 since VS20??/15.0.
          {
            fail << "C++" << v << " is not supported by " << ci.signature <<
              info << "required by " << project (rs) << '@' << rs.out_path ();
          }
        }
      }
      else
      {
        // Translate 11 to 0x, 14 to 1y, and 17 to 1z for compatibility with
        // older versions of the compilers.
        //
        r = "-std=";

        if (v == "98")
          r += "c++98";
        else if (v == "03")
          r += "c++03";
        else if (v == "11")
          r += "c++0x";
        else if (v == "14")
          r += "c++1y";
        else if (v == "17")
          r += "c++1z";
        else
          r += v; // In case the user specifies something like 'gnu++17'.
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
      tracer trace ("cxx::config_init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      // We only support root loading (which means there can only be one).
      //
      if (&rs != &bs)
        fail (loc) << "cxx.config module must be loaded in project root";

      // Load cc.core.vars so that we can cache all the cc.* variables.
      //
      if (!cast_false<bool> (rs["cc.core.vars.loaded"]))
        load_module ("cc.core.vars", rs, rs, loc);

      // Enter all the variables and initialize the module data.
      //
      auto& v (var_pool);

      cc::config_data d {
        cc::lang::cxx,

        "cxx",
        "c++",
        "g++",

        // Note: some overridable, some not.
        //
        v.insert<path>    ("config.cxx",          true),
        v.insert<strings> ("config.cxx.poptions", true),
        v.insert<strings> ("config.cxx.coptions", true),
        v.insert<strings> ("config.cxx.loptions", true),
        v.insert<strings> ("config.cxx.libs",     true),

        v.insert<process_path> ("cxx.path"),
        v.insert<dir_paths>    ("cxx.sys_lib_dirs"),

        v.insert<strings> ("cxx.poptions"),
        v.insert<strings> ("cxx.coptions"),
        v.insert<strings> ("cxx.loptions"),
        v.insert<strings> ("cxx.libs"),

        v["cc.poptions"],
        v["cc.coptions"],
        v["cc.loptions"],
        v["cc.libs"],

        v.insert<strings> ("cxx.export.poptions"),
        v.insert<strings> ("cxx.export.coptions"),
        v.insert<strings> ("cxx.export.loptions"),
        v.insert<names>   ("cxx.export.libs"),

        v["cc.export.poptions"],
        v["cc.export.coptions"],
        v["cc.export.loptions"],
        v["cc.export.libs"],

        v["cc.type"],
        v["cc.system"],

        v.insert<string>   ("cxx.std", true),

        v.insert<string>   ("cxx.id"),
        v.insert<string>   ("cxx.id.type"),
        v.insert<string>   ("cxx.id.variant"),

        v.insert<string>   ("cxx.version"),
        v.insert<uint64_t> ("cxx.version.major"),
        v.insert<uint64_t> ("cxx.version.minor"),
        v.insert<uint64_t> ("cxx.version.patch"),
        v.insert<string>   ("cxx.version.build"),

        v.insert<string>   ("cxx.signature"),
        v.insert<string>   ("cxx.checksum"),

        v.insert<string>   ("cxx.target"),
        v.insert<string>   ("cxx.target.cpu"),
        v.insert<string>   ("cxx.target.vendor"),
        v.insert<string>   ("cxx.target.system"),
        v.insert<string>   ("cxx.target.version"),
        v.insert<string>   ("cxx.target.class")
      };

      assert (mod == nullptr);
      config_module* m;
      mod.reset (m = new config_module (move (d)));
      m->init (rs, loc, hints);
      return true;
    }

    static const target_type* hdr[] =
    {
      &hxx::static_type,
      &ixx::static_type,
      &txx::static_type,
      &h::static_type,
      nullptr
    };

    static const target_type* inc[] =
    {
      &hxx::static_type,
      &ixx::static_type,
      &txx::static_type,
      &cxx::static_type,
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
      tracer trace ("cxx::init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      // We only support root loading (which means there can only be one).
      //
      if (&rs != &bs)
        fail (loc) << "cxx module must be loaded in project root";

      // Load cxx.config.
      //
      if (!cast_false<bool> (rs["cxx.config.loaded"]))
        load_module ("cxx.config", rs, rs, loc, false, hints);

      config_module& cm (*rs.modules.lookup<config_module> ("cxx.config"));

      cc::data d {
        cm,

        "cxx.compile",
        "cxx.link",
        "cxx.install",
        "cxx.uninstall",

        cast<string> (rs[cm.x_id]),
        cast<string> (rs[cm.x_target]),
        cast<string> (rs[cm.x_target_system]),
        cast<string> (rs[cm.x_target_class]),

        cm.tstd,

        cast_null<process_path> (rs["pkgconfig.path"]),
        cast<dir_paths> (rs[cm.x_sys_lib_dirs]),

        cxx::static_type,
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
