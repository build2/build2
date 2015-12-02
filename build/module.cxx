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
        i = lm.emplace (name, make_pair (j->second, nullptr)).first;
    }

    bool l (i != lm.end ());
    bool c (l && i->second.first (rs, bs, loc, i->second.second, f, opt));

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
