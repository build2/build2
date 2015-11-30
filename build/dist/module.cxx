// file      : build/dist/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/dist/module>

#include <build/scope>
#include <build/file>
#include <build/diagnostics>

#include <build/config/utility>

#include <build/dist/rule>
#include <build/dist/operation>

using namespace std;
using namespace butl;

namespace build
{
  namespace dist
  {
    static rule rule_;

    extern "C" void
    dist_init (scope& r,
               scope& b,
               const location& l,
               std::unique_ptr<module>&,
               bool first)
    {
      tracer trace ("dist::init");

      if (&r != &b)
        fail (l) << "dist module must be initialized in bootstrap.build";

      if (!first)
      {
        warn (l) << "multiple dist module initializations";
        return;
      }

      const dir_path& out_root (r.out_path ());
      level5 ([&]{trace << "for " << out_root;});

      // Register meta-operation.
      //
      r.meta_operations.insert (dist_id, dist);

      // Register our wildcard rule. Do it explicitly for the alias
      // to prevent something like insert<target>(dist_id, test_id)
      // taking precedence.
      //
      r.rules.insert<target> (dist_id, 0, "dist", rule_);
      r.rules.insert<alias> (dist_id, 0, "alias", rule_);

      // Enter module variables.
      //
      if (first)
      {
        variable_pool.find ("dist", bool_type);

        variable_pool.find ("dist.package", string_type);

        variable_pool.find ("dist.root", dir_path_type);
        variable_pool.find ("config.dist.root", dir_path_type);

        //@@ VAR type
        //
        variable_pool.find ("dist.cmd", string_type);
        variable_pool.find ("config.dist.cmd", string_type);

        variable_pool.find ("dist.archives", strings_type);
        variable_pool.find ("config.dist.archives", strings_type);
      }

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
    }
  }
}
