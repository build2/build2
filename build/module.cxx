// file      : build/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/module>

#include <utility> // make_pair()

#include <build/scope>
#include <build/diagnostics>

using namespace std;

namespace build
{
  available_module_map builtin_modules;

  void
  load_module (const string& name, scope& root, scope& base, const location& l)
  {
    // First see if this modules has already been loaded for this
    // project.
    //
    loaded_module_map& lm (root.modules);
    auto i (lm.find (name));
    bool f (i == lm.end ());

    if (f)
    {
      // Otherwise search for this module.
      //
      auto j (builtin_modules.find (name));

      if (j == builtin_modules.end ())
        fail (l) << "unknown module " << name;

      i = lm.emplace (name, make_pair (j->second, nullptr)).first;
    }

    i->second.first (root, base, l, i->second.second, f);
  }
}
