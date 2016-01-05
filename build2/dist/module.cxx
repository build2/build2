// file      : build2/dist/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/dist/module>

#include <build2/scope>
#include <build2/file>
#include <build2/diagnostics>

#include <build2/config/utility>

#include <build2/dist/rule>
#include <build2/dist/operation>

using namespace std;
using namespace butl;

namespace build2
{
  namespace dist
  {
    static rule rule_;

    extern "C" void
    dist_boot (scope& r, const location&, unique_ptr<module>&)
    {
      tracer trace ("dist::boot");

      level5 ([&]{trace << "for " << r.out_path ();});

      // Register meta-operation.
      //
      r.meta_operations.insert (dist_id, dist);
    }

    extern "C" bool
    dist_init (scope& r,
               scope&,
               const location& l,
               unique_ptr<module>&,
               bool first,
               bool)
    {
      tracer trace ("dist::init");

      if (!first)
      {
        warn (l) << "multiple dist module initializations";
        return true;
      }

      const dir_path& out_root (r.out_path ());
      level5 ([&]{trace << "for " << out_root;});

      // Enter module variables.
      //
      if (first)
      {
        auto& v (var_pool);

        v.find ("dist", bool_type);

        v.find ("dist.package", string_type);

        v.find ("dist.root", dir_path_type);
        v.find ("config.dist.root", dir_path_type);

        //@@ VAR type
        //
        v.find ("dist.cmd", string_type);
        v.find ("config.dist.cmd", string_type);

        v.find ("dist.archives", strings_type);
        v.find ("config.dist.archives", strings_type);
      }

      // Register our wildcard rule. Do it explicitly for the alias
      // to prevent something like insert<target>(dist_id, test_id)
      // taking precedence.
      //
      r.rules.insert<target> (dist_id, 0, "dist", rule_);
      r.rules.insert<alias> (dist_id, 0, "dist.alias", rule_);

      // Configuration.
      //
      // Note that we don't use any defaults for root -- the location
      // must be explicitly specified or we will complain if and when
      // we try to dist.
      //
      using namespace config;

      bool s (specified (r, "config.dist"));

      // dist.root
      //
      {
        value& v (r.assign ("dist.root"));

        if (s)
        {
          const value& cv (optional_absolute (r, "config.dist.root"));

          if (cv && !cv.empty ())
            v = cv;
        }
      }

      // dist.cmd
      //
      {
        value& v (r.assign ("dist.cmd"));

        if (s)
        {
          const value& cv (required (r, "config.dist.cmd", "install").first);

          if (cv && !cv.empty ())
            v = cv;
        }
        else
          v = "install";
      }

      // dist.archives
      //
      {
        value& v (r.assign ("dist.archives"));

        if (s)
        {
          const value& cv (optional (r, "config.dist.archives"));

          if (cv && !cv.empty ())
            v = cv;
        }
      }

      return true;
    }
  }
}
