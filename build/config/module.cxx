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
    config_init (scope& r,
                 scope& b,
                 const location& l,
                 std::unique_ptr<module>&,
                 bool first,
                 bool)
    {
      tracer trace ("config::init");

      if (&r != &b)
        fail (l) << "config module must be initialized in bootstrap.build";

      if (!first)
      {
        warn (l) << "multiple config module initializations";
        return true;
      }

      const dir_path& out_root (r.out_path ());
      level5 ([&]{trace << "for " << out_root;});

      // Register meta-operations.
      //
      r.meta_operations.insert (configure_id, configure);
      r.meta_operations.insert (disfigure_id, disfigure);

      // Register alias and fallback rule for the configure meta-operation.
      //
      global_scope->rules.insert<file> (
        configure_id, 0, "file", file_rule::instance);

      r.rules.insert<alias> (configure_id, 0, "alias", alias_rule::instance);
      r.rules.insert<file> (configure_id, 0, "", fallback_rule::instance);
      r.rules.insert<target> (configure_id, 0, "", fallback_rule::instance);

      // Load config.build if one exists.
      //
      path f (out_root / config_file);

      if (file_exists (f))
        source (f, r, r);

      return true;
    }
  }
}
