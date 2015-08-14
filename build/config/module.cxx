// file      : build/config/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/config/module>

#include <butl/filesystem>

#include <build/scope>
#include <build/file>
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

    extern "C" void
    config_init (scope& r,
                 scope& b,
                 const location& l,
                 std::unique_ptr<module>&,
                 bool first)
    {
      tracer trace ("config::init");

      if (&r != &b)
        fail (l) << "config module must be initialized in bootstrap.build";

      if (!first)
      {
        warn (l) << "multiple config module initializations";
        return;
      }

      const dir_path& out_root (r.path ());
      level4 ([&]{trace << "for " << out_root;});

      // Register meta-operations.
      //
      r.meta_operations.insert (configure_id, configure);
      r.meta_operations.insert (disfigure_id, disfigure);

      // Load config.build if one exists.
      //
      path f (out_root / config_file);

      if (file_exists (f))
        source (f, r, r);
    }
  }
}
