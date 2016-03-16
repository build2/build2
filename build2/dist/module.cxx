// file      : build2/dist/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
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

      l5 ([&]{trace << "for " << r.out_path ();});

      // Register meta-operation.
      //
      r.meta_operations.insert (dist_id, dist);

      // Enter module variables. Do it during boot in case they get assigned
      // in bootstrap.build (which is customary for, e.g., dist.package).
      //
      {
        auto& v (var_pool);

        v.find<bool> ("dist");

        v.find<string> ("dist.package");

        v.find<dir_path> ("dist.root");
        v.find<dir_path> ("config.dist.root");

        //@@ VAR type
        //
        v.find<string> ("dist.cmd");
        v.find<string> ("config.dist.cmd");

        v.find<strings> ("dist.archives");
        v.find<strings> ("config.dist.archives");
      }
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
      l5 ([&]{trace << "for " << out_root;});

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
      bool s (config::specified (r, "config.dist"));

      // dist.root
      //
      {
        value& v (r.assign ("dist.root"));

        if (s)
        {
          const value& cv (config::optional_absolute (r, "config.dist.root"));

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
          const value& cv (
            config::required (r, "config.dist.cmd", "install").first);

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
          const value& cv (config::optional (r, "config.dist.archives"));

          if (cv && !cv.empty ())
            v = cv;
        }
      }

      return true;
    }
  }
}
