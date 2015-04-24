// file      : build/bin/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/bin/module>

#include <build/scope>
#include <build/variable>
#include <build/diagnostics>

#include <build/config/utility>

using namespace std;

namespace build
{
  namespace bin
  {
    void
    init (scope& root, scope& base, const location& l)
    {
      //@@ TODO: avoid multiple inits (generally, for modules).
      //
      tracer trace ("bin::init");

      //@@ Should it be this way?
      //
      if (&root != &base)
        fail (l) << "bin module must be initialized in project root scope";

      //@@ TODO: need to register target types, rules here instead of main().

      const dir_path& out_root (root.path ());
      level4 ([&]{trace << "for " << out_root;});

      // Configure.
      //
    }

    void
    init_lib (const dir_path& d)
    {
      scope* root (scopes.find (d).root_scope ());

      if (root == nullptr)
        return;

      // config.bin.lib
      //
      {
        auto v (root->vars.assign ("bin.lib"));

        if (!v)
          v = config::required (*root, "config.bin.lib", "shared").first;
      }
    }
  }
}
