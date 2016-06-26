// file      : build2/config/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/config/module>

#include <butl/filesystem>

#include <build2/file>
#include <build2/rule>
#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/config/operation>

using namespace std;
using namespace butl;

namespace build2
{
  namespace config
  {
    const string module::name ("config");

    //@@ Same as in operation.cxx
    //
    static const path config_file ("build/config.build");

    extern "C" void
    config_boot (scope& root, const location&, unique_ptr<module_base>&)
    {
      tracer trace ("config::boot");

      const dir_path& out_root (root.out_path ());
      l5 ([&]{trace << "for " << out_root;});

      // Register meta-operations.
      //
      root.meta_operations.insert (configure_id, configure);
      root.meta_operations.insert (disfigure_id, disfigure);

      // Load config.build if one exists.
      //
      // Note that we have to do this during bootstrap since the order in
      // which the modules will be initialized is unspecified. So it is
      // possible that some module which needs the configuration will get
      // called first.
      //
      path f (out_root / config_file);

      if (file_exists (f))
        source (f, root, root);
    }

    extern "C" bool
    config_init (scope& root,
                 scope&,
                 const location& l,
                 unique_ptr<module_base>& mod,
                 bool first,
                 bool,
                 const variable_map& config_hints)
    {
      tracer trace ("config::init");

      if (!first)
      {
        warn (l) << "multiple config module initializations";
        return true;
      }

      l5 ([&]{trace << "for " << root.out_path ();});

      assert (config_hints.empty ()); // We don't known any hints.

      // Only create the module if we are configuring.
      //
      if (current_mif->id == configure_id)
        mod.reset (new module);

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

      return true;
    }
  }
}
