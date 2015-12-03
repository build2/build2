// file      : build/config/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/config/module>

#include <butl/filesystem>

#include <build/file>
#include <build/rule>
#include <build/scope>
#include <build/diagnostics>

#include <build/config/operation>

using namespace std;
using namespace butl;

namespace build
{
  namespace config
  {
    //@@ Same as in operation.cxx
    //
    static const path config_file ("build/config.build");

    extern "C" bool
    config_init (scope& root,
                 scope& base,
                 const location& l,
                 std::unique_ptr<module>&,
                 bool first,
                 bool)
    {
      tracer trace ("config::init");

      if (&root != &base)
        fail (l) << "config module must be initialized in bootstrap.build";

      if (!first)
      {
        warn (l) << "multiple config module initializations";
        return true;
      }

      const dir_path& out_root (root.out_path ());
      level5 ([&]{trace << "for " << out_root;});

      // Register meta-operations.
      //
      root.meta_operations.insert (configure_id, configure);
      root.meta_operations.insert (disfigure_id, disfigure);

      // Register alias and fallback rule for the configure meta-operation.
      //
      {
        // We need this rule for out-of-any-project dependencies (e.g.,
        // libraries imported from /usr/lib).
        //
        global_scope->rules.insert<file> (
          configure_id, 0, "config.file", file_rule::instance);

        auto& r (root.rules);

        r.insert<target> (configure_id, 0, "config", fallback_rule::instance);
        r.insert<file> (configure_id, 0, "config.file", fallback_rule::instance);
        r.insert<alias> (configure_id, 0, "config.alias", alias_rule::instance);
      }

      // Load config.build if one exists.
      //
      path f (out_root / config_file);

      if (file_exists (f))
        source (f, root, root);

      return true;
    }
  }
}
