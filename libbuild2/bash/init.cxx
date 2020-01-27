// file      : libbuild2/bash/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bash/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/install/utility.hxx>

#include <libbuild2/bash/rule.hxx>
#include <libbuild2/bash/target.hxx>
#include <libbuild2/bash/utility.hxx>

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
          bool first,
          bool,
          const variable_map&)
    {
      tracer trace ("bash::init");
      l5 ([&]{trace << "for " << bs;});

      // Load in.base (in.* variables, in{} target type).
      //
      load_module (rs, rs, "in.base", l);

      bool install_loaded (cast_false<bool> (rs["install.loaded"]));

      // Register target types and configure default installability.
      //
      if (first)
        rs.insert_target_type<bash> ();

      if (install_loaded)
      {
        using namespace install;

        // Install into bin/<project>/ by default stripping the .bash
        // extension from <project> if present.
        //
        const project_name& p (project (rs));

        if (!p.empty ())
        {
          install_path<bash> (bs, dir_path ("bin") /= project_base (p));
          install_mode<bash> (bs, "644");
        }
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

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"bash",  nullptr, init},
      {nullptr, nullptr, nullptr}
    };

    const module_functions*
    build2_bash_load ()
    {
      return mod_functions;
    }
  }
}
