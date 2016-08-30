// file      : build2/dist/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/dist/init>

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

    void
    boot (scope& r, const location&, unique_ptr<module_base>&)
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

        // Note: some overridable, some not.
        //
        v.insert<abs_dir_path> ("config.dist.root",     true);
        v.insert<strings>      ("config.dist.archives", true);
        v.insert<path>         ("config.dist.cmd",      true);

        v.insert<dir_path>     ("dist.root");
        v.insert<path>         ("dist.cmd");
        v.insert<strings>      ("dist.archives");

        v.insert<bool> ("dist", variable_visibility::target); // Flag.

        // Project's package name.
        //
        v.insert<string> ("dist.package", variable_visibility::project);
      }
    }

    bool
    init (scope& r,
          scope&,
          const location& l,
          unique_ptr<module_base>&,
          bool first,
          bool,
          const variable_map& config_hints)
    {
      tracer trace ("dist::init");

      if (!first)
      {
        warn (l) << "multiple dist module initializations";
        return true;
      }

      const dir_path& out_root (r.out_path ());
      l5 ([&]{trace << "for " << out_root;});

      assert (config_hints.empty ()); // We don't known any hints.

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

      // Adjust module priority so that the config.dist.* values are saved at
      // the end of config.build.
      //
      if (s)
        config::save_module (r, "dist", INT32_MAX);

      // dist.root
      //
      {
        value& v (r.assign ("dist.root"));

        if (s)
        {
          if (const value& cv = config::optional (r, "config.dist.root"))
            v = cast<dir_path> (cv); // Strip abs_dir_path.
        }
      }

      // dist.cmd
      //
      {
        value& v (r.assign ("dist.cmd"));

        if (s)
        {
          if (const value& cv = config::required (r,
                                                  "config.dist.cmd",
                                                  path ("install")).first)
            v = cv;
        }
      }

      // dist.archives
      //
      {
        value& v (r.assign ("dist.archives"));

        if (s)
        {
          if (const value& cv = config::optional (r, "config.dist.archives"))
            v = cv;
        }
      }

      return true;
    }
  }
}
