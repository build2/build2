// file      : build/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/module>

#include <utility> // make_pair()

#include <build/scope>
#include <build/variable>
#include <build/diagnostics>

using namespace std;

namespace build
{
  available_module_map builtin_modules;

  void
  boot_module (const string& name, scope& rs, const location& loc)
  {
    // First see if this modules has already been loaded for this project.
    //
    loaded_module_map& lm (rs.modules);
    auto i (lm.find (name));

    if (i != lm.end ())
    {
      module_state& s (i->second);

      // The only valid situation here is if the module has already been
      // bootstrapped.
      //
      assert (s.boot);
      return;
    }

    // Otherwise search for this module.
    //
    auto j (builtin_modules.find (name));

    if (j == builtin_modules.end ())
      fail (loc) << "unknown module " << name;

    const module_functions& mf (j->second);

    if (mf.boot == nullptr)
      fail (loc) << "module " << name << " shouldn't be loaded in bootstrap";

    i = lm.emplace (name, module_state {true, mf.init, nullptr, loc}).first;
    mf.boot (rs, loc, i->second.module);
  }

  bool
  load_module (bool opt,
               const string& name,
               scope& rs,
               scope& bs,
               const location& loc)
  {
    // First see if this modules has already been loaded for this project.
    //
    loaded_module_map& lm (rs.modules);
    auto i (lm.find (name));
    bool f (i == lm.end ());

    if (f)
    {
      // Otherwise search for this module.
      //
      auto j (builtin_modules.find (name));

      if (j == builtin_modules.end ())
      {
        if (!opt)
          fail (loc) << "unknown module " << name;
      }
      else
      {
        const module_functions& mf (j->second);

        if (mf.boot != nullptr)
          fail (loc) << "module " << name << " should be loaded in bootstrap";

        i = lm.emplace (
          name, module_state {false, mf.init, nullptr, loc}).first;
      }
    }
    else
    {
      module_state& s (i->second);

      if (s.boot)
      {
        s.boot = false;
        f = true; // This is a first call to init.
      }
    }

    bool l (i != lm.end ());
    bool c (l && i->second.init (rs, bs, loc, i->second.module, f, opt));

    const variable& lv (var_pool.find (name + ".loaded",
                                       variable_visibility::project,
                                       bool_type));
    const variable& cv (var_pool.find (name + ".configured",
                                       variable_visibility::project,
                                       bool_type));
    bs.assign (lv) = l;
    bs.assign (cv) = c;

    return l && c;
  }
}
