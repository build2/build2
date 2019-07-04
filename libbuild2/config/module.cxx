// file      : libbuild2/config/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/config/module.hxx>

using namespace std;

namespace build2
{
  namespace config
  {
    void module::
    save_variable (const variable& var, uint64_t flags)
    {
      const string& n (var.name);

      // First try to find the module with the name that is the longest
      // prefix of this variable name.
      //
      auto& sm (saved_modules);
      auto i (sm.find_sup (n));

      // If no module matched, then create one based on the variable name.
      //
      if (i == sm.end ())
      {
        // @@ For now with 'config.' prefix.
        //
        i = sm.insert (string (n, 0, n.find ('.', 7)));
      }

      // Don't insert duplicates. The config.import vars are particularly
      // susceptible to duplication.
      //
      saved_variables& sv (i->second);
      auto j (sv.find (var));

      if (j == sv.end ())
        sv.push_back (saved_variable {var, flags});
      else
        assert (j->flags == flags);
    }

    void module::
    save_module (const char* name, int prio)
    {
      saved_modules.insert (string ("config.") += name, prio);
    }

    const string module::name ("config");
    const uint64_t module::version (1);
  }
}
