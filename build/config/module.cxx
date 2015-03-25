// file      : build/config/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/config/module>

#include <build/path>
#include <build/scope>
#include <build/diagnostics>

#include <build/config/operation>

using namespace std;

namespace build
{
  namespace config
  {
    static bool
    trigger (scope&, const path& p)
    {
      tracer trace ("config::trigger");

      level4 ([&]{trace << "intercepted sourcing of " << p;});
      return false;
    }

    void
    init (scope& root, scope& base, const location& l)
    {
      //@@ TODO: avoid multiple inits (generally, for modules).
      //

      tracer trace ("config::init");

      if (&root != &base)
        fail (l) << "config module must be initialized in project root scope";

      const path& out_root (root.path ());
      level4 ([&]{trace << "for " << out_root << '/';});

      // Register meta-operations.
      //
      if (root.meta_operations.insert (configure) != configure_id ||
          root.meta_operations.insert (disfigure) != disfigure_id)
        fail (l) << "config module must be initialized before other modules";

      // Register the build/config.build sourcing trigger.
      //
      root.triggers[out_root / path ("build/config.build")] = &trigger;
    }
  }
}
