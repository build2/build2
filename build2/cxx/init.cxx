// file      : build2/cxx/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/init>

#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/cc/module>

#include <build2/cxx/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cxx
  {
    using cc::config_module;

    class module: public cc::module
    {
    public:
      explicit
      module (data&& d): common (move (d)), cc::module (move (d)) {}

      bool
      translate_std (string&, scope&, const value&) const override;
    };

    bool module::
    translate_std (string& s, scope& r, const value& val) const
    {
      const string& v (cast<string> (val));

      if (cid == "msvc")
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
          uint64_t cver (cast<uint64_t> (r[x_version_major]));

          // @@ Is mapping for 14 and 17 correct? Maybe Update 2 for 14?
          //
          if ((v == "11" && cver < 16) || // C++11 since VS2010/10.0.
              (v == "14" && cver < 19) || // C++14 since VS2015/14.0.
              (v == "17" && cver < 20))   // C++17 since VS20??/15.0.
          {
            fail << "C++" << v << " is not supported by "
                 << cast<string> (r[x_signature]) <<
              info << "required by " << project (r) << '@' << r.out_path ();
          }
        }

        return false;
      }
      else
      {
        // Translate 11 to 0x, 14 to 1y, and 17 to 1z for compatibility with
        // older versions of the compilers.
        //
        s = "-std=";

        if (v == "98")
          s += "c++98";
        else if (v == "03")
          s += "c++03";
        else if (v == "11")
          s += "c++0x";
        else if (v == "14")
          s += "c++1y";
        else if (v == "17")
          s += "c++1z";
        else
          s += v; // In case the user specifies something like 'gnu++17'.

        return true;
      }
    }

    bool
    config_init (scope& r,
                 scope& b,
                 const location& loc,
                 unique_ptr<module_base>& m,
                 bool first,
                 bool,
                 const variable_map& hints)
    {
      tracer trace ("cxx::config_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      if (first)
      {
        // Load cc.vars so that we can cache all the cc.* variables.
        //
        if (!cast_false<bool> (b["cc.vars.loaded"]))
          load_module ("cc.vars", r, b, loc);

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
          v.insert<path>     ("config.cxx",          true),
          v.insert<strings>  ("config.cxx.poptions", true),
          v.insert<strings>  ("config.cxx.coptions", true),
          v.insert<strings>  ("config.cxx.loptions", true),
          v.insert<strings>  ("config.cxx.libs",     true),

          v.insert<strings>  ("cxx.poptions"),
          v.insert<strings>  ("cxx.coptions"),
          v.insert<strings>  ("cxx.loptions"),
          v.insert<strings>  ("cxx.libs"),

          v["cc.poptions"],
          v["cc.coptions"],
          v["cc.loptions"],
          v["cc.libs"],

          v.insert<strings>  ("cxx.export.poptions"),
          v.insert<strings>  ("cxx.export.coptions"),
          v.insert<strings>  ("cxx.export.loptions"),
          v.insert<strings>  ("cxx.export.libs"),

          v["cc.export.poptions"],
          v["cc.export.coptions"],
          v["cc.export.loptions"],
          v["cc.export.libs"],

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

        assert (m == nullptr);
        m.reset (new config_module (move (d)));
      }

      static_cast<config_module&> (*m).init (r, b, loc, first, hints);
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
    init (scope& r,
          scope& b,
          const location& loc,
          unique_ptr<module_base>& m,
          bool first,
          bool,
          const variable_map& hints)
    {
      tracer trace ("cxx::init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Load cxx.config.
      //
      if (!cast_false<bool> (b["cxx.config.loaded"]))
        load_module ("cxx.config", r, b, loc, false, hints);

      if (first)
      {
        config_module& cm (*r.modules.lookup<config_module> ("cxx.config"));

        cc::data d {
          cm,

          "cxx.compile",
          "cxx.link",
          "cxx.install",

          cast<string> (r[cm.x_id]),
          cast<string> (r[cm.x_target]),
          cast<string> (r[cm.x_target_system]),
          cast<string> (r[cm.x_target_class]),

          cxx::static_type,
          hdr,
          inc
        };

        assert (m == nullptr);
        m.reset (new module (move (d)));
      }

      static_cast<module&> (*m).init (r, b, loc, first, hints);
      return true;
    }
  }
}
