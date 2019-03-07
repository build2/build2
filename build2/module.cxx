// file      : build2/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/module.hxx>

#include <build2/scope.hxx>
#include <build2/variable.hxx>
#include <build2/diagnostics.hxx>

using namespace std;

namespace build2
{
  available_module_map builtin_modules;

  void
  boot_module (scope& rs, const string& name, const location& loc)
  {
    // First see if this modules has already been loaded for this project.
    //
    loaded_module_map& lm (rs.root_extra->modules);
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

    i = lm.emplace (name,
                    module_state {true, false, mf.init, nullptr, loc}).first;
    i->second.first = mf.boot (rs, loc, i->second.module);

    rs.assign (var_pool.rw (rs).insert (name + ".booted")) = true;
  }

  bool
  load_module (scope& rs,
               scope& bs,
               const string& name,
               const location& loc,
               bool opt,
               const variable_map& hints)
  {
    // First see if this modules has already been loaded for this project.
    //
    loaded_module_map& lm (rs.root_extra->modules);
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
          name,
          module_state {false, false, mf.init, nullptr, loc}).first;
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

    // Note: pattern-typed in context.cxx:reset() as project-visibility
    // variables of type bool.
    //
    auto& vp (var_pool.rw (rs));
    value& lv (bs.assign (vp.insert (name + ".loaded")));
    value& cv (bs.assign (vp.insert (name + ".configured")));

    bool l; // Loaded.
    bool c; // Configured.

    // Suppress duplicate init() calls for the same module in the same scope.
    //
    if (!lv.null)
    {
      assert (!cv.null);

      l = cast<bool> (lv);
      c = cast<bool> (cv);

      if (!opt)
      {
        if (!l)
          fail (loc) << "unknown module " << name;

        // We don't have original diagnostics. We could call init() again so
        // that it can issue it. But that means optional modules must be
        // prepared to be called again if configuring failed. Let's keep it
        // simple for now.
        //
        if (!c)
          fail (loc) << "module " << name << " failed to configure";
      }
    }
    else
    {
      l = i != lm.end ();
      c = l && i->second.init (rs, bs, loc, i->second.module, f, opt, hints);

      lv = l;
      cv = c;
    }

    return l && c;
  }
}
