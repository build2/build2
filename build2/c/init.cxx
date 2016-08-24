// file      : build2/c/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/c/init>

#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/cc/module>

#include <build2/c/target>

using namespace std;
using namespace butl;

namespace build2
{
  namespace c
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
          uint64_t cver (cast<uint64_t> (r[x_version_major]));

          if ((v == "99" && cver < 16) || // Since VS2010/10.0.
              (v == "11" && cver < 17))   // Since VS2012/11.0.
          {
            fail << "C" << v << " is not supported by "
                 << cast<string> (r[x_signature]) <<
              info << "required by " << project (r) << '@' << r.out_path ();
          }
        }

        return false;
      }
      else
      {
        // 90 and 89 are the same standard. Translate 99 to 9x and 11 to 1x
        // for compatibility with older versions of the compilers.
        //
        s = "-std=";

        if (v == "90")
          s += "c90";
        else if (v == "99")
          s += "c9x";
        else if (v == "11")
          s += "c1x";
        else
          s += v; // In case the user specifies something like 'gnu11'.

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
      tracer trace ("c::config_init");
      l5 ([&]{trace << "for " << b.out_path ();});

      if (first)
      {
        // Load cc.core.vars so that we can cache all the cc.* variables.
        //
        if (!cast_false<bool> (b["cc.core.vars.loaded"]))
          load_module ("cc.core.vars", r, b, loc);

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
          v.insert<strings>      ("c.poptions"),
          v.insert<strings>      ("c.coptions"),
          v.insert<strings>      ("c.loptions"),
          v.insert<strings>      ("c.libs"),

          v["cc.poptions"],
          v["cc.coptions"],
          v["cc.loptions"],
          v["cc.libs"],

          v.insert<strings>  ("c.export.poptions"),
          v.insert<strings>  ("c.export.coptions"),
          v.insert<strings>  ("c.export.loptions"),
          v.insert<names>    ("c.export.libs"),

          v["cc.export.poptions"],
          v["cc.export.coptions"],
          v["cc.export.loptions"],
          v["cc.export.libs"],

          v["cc.type"],

          v.insert<string>   ("c.std", true),

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

          v.insert<string>   ("c.target"),
          v.insert<string>   ("c.target.cpu"),
          v.insert<string>   ("c.target.vendor"),
          v.insert<string>   ("c.target.system"),
          v.insert<string>   ("c.target.version"),
          v.insert<string>   ("c.target.class")
        };

        assert (m == nullptr);
        m.reset (new config_module (move (d)));
      }

      static_cast<config_module&> (*m).init (r, b, loc, first, hints);
      return true;
    }

    static const target_type* hdr[] =
    {
      &h::static_type,
      nullptr
    };

    static const target_type* inc[] =
    {
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
      tracer trace ("c::init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Load c.config.
      //
      if (!cast_false<bool> (b["c.config.loaded"]))
        load_module ("c.config", r, b, loc, false, hints);

      if (first)
      {
        config_module& cm (*r.modules.lookup<config_module> ("c.config"));

        cc::data d {
          cm,

          "c.compile",
          "c.link",
          "c.install",
          "c.uninstall",

          cast<string> (r[cm.x_id]),
          cast<string> (r[cm.x_target]),
          cast<string> (r[cm.x_target_system]),
          cast<string> (r[cm.x_target_class]),

          c::static_type,
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
