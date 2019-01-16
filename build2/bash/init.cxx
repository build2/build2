// file      : build2/bash/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bash/init.hxx>

#include <build2/scope.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>
#include <build2/diagnostics.hxx>

#include <build2/install/utility.hxx>

#include <build2/bash/rule.hxx>
#include <build2/bash/target.hxx>
#include <build2/bash/utility.hxx>

using namespace std;

namespace build2
{
  namespace bash
  {
    static const in_rule in_rule_;
    static const install_rule install_rule_ (in_rule_);

    bool
    init (scope& rs,
          scope& bs,
          const location& l,
          unique_ptr<module_base>&,
          bool,
          bool,
          const variable_map&)
    {
      tracer trace ("bash::init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      // Load in.base (in.* varibales, in{} target type).
      //
      if (!cast_false<bool> (rs["in.base.loaded"]))
        load_module (rs, rs, "in.base", l);

      bool install_loaded (cast_false<bool> (rs["install.loaded"]));

      // Register target types and configure default installability.
      //
      bs.target_types.insert<bash> ();

      if (install_loaded)
      {
        using namespace install;

        // Install into bin/<project>/ by default stripping the .bash
        // extension from <project> if present.
        //
        const project_name& p (cast<project_name> (rs.vars[var_project]));

        install_path<bash> (bs, dir_path ("bin") /= project_base (p));
        install_mode<bash> (bs, "644");
      }

      // Register rules.
      //
      {
        auto& r (bs.rules);

        r.insert<exe> (perform_update_id,   "bash.in", in_rule_);
        r.insert<exe> (perform_clean_id,    "bash.in", in_rule_);
        r.insert<exe> (configure_update_id, "bash.in", in_rule_);

        r.insert<bash> (perform_update_id,   "bash.in", in_rule_);
        r.insert<bash> (perform_clean_id,    "bash.in", in_rule_);
        r.insert<bash> (configure_update_id, "bash.in", in_rule_);

        if (install_loaded)
        {
          r.insert<exe>  (perform_install_id,   "bash.install",   install_rule_);
          r.insert<exe>  (perform_uninstall_id, "bash.uninstall", install_rule_);

          r.insert<bash> (perform_install_id,   "bash.install",   install_rule_);
          r.insert<bash> (perform_uninstall_id, "bash.uninstall", install_rule_);
        }
      }

      return true;
    }
  }
}
