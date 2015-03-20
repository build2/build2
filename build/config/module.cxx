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
    load (scope& root, scope& base, const location& l)
    {
      tracer trace ("config::load");

      //@@ TODO: avoid multiple loads (generally, for modules).
      //
      level4 ([&]{trace << "for " << root.path () << '/';});

      if (&root != &base)
        fail (l) << "config module must be loaded in project root scope";

      // Register meta-operations.
      //
      if (root.meta_operations.insert (configure) != configure_id ||
          root.meta_operations.insert (disfigure) != disfigure_id)
        fail (l) << "config module must be loaded before other modules";

      // Register the build/config.build loading trigger.
      //
      root.triggers[path ("build/config.build")] = &trigger;
    }
  }
}
