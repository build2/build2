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

    void
    init (scope& root, scope& base, const location& l)
    {
      //@@ TODO: avoid multiple inits (generally, for modules).
      //

      tracer trace ("config::init");

      if (&root != &base)
        fail (l) << "config module must be initialized in project root scope";

      const dir_path& out_root (root.path ());
      level4 ([&]{trace << "for " << out_root;});

      // Register meta-operations.
      //
      if (root.meta_operations.insert (configure) != configure_id ||
          root.meta_operations.insert (disfigure) != disfigure_id)
        fail (l) << "config module must be initialized before other modules";

      // Load config.build if one exists.
      //
      path f (out_root / config_file);

      if (file_exists (f))
        source (f, root, root);
    }
  }
}
