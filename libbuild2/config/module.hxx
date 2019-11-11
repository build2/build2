// file      : libbuild2/config/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CONFIG_MODULE_HXX
#define LIBBUILD2_CONFIG_MODULE_HXX

#include <map>

#include <libbutl/prefix-map.mxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>
#include <libbuild2/variable.hxx>

namespace build2
{
  namespace config
  {
    // An ordered list of build system modules each with an ordered list of
    // list of config.* variables and their "save flags" (see save_variable())
    // that are used (as opposed to just being specified) in this
    // configuration. Populated by the config utility functions (required(),
    // optional()) and saved in the order populated.
    //
    struct saved_variable
    {
      reference_wrapper<const variable> var;
      uint64_t flags;
    };

    struct saved_variables: vector<saved_variable>
    {
      // Normally each module only have a handful of config variables and we
      // only do this during configuration so for now we do linear search
      // instead of adding a map.
      //
      const_iterator
      find (const variable& var) const
      {
        return std::find_if (
          begin (),
          end (),
          [&var] (const saved_variable& v) {return var == v.var;});
      }
    };

    struct saved_modules: butl::prefix_map<string, saved_variables, '.'>
    {
      // Priority order with INT32_MIN being the highest. Modules with the
      // same priority are saved in the order inserted.
      //
      // Generally, the idea is that we want higher-level modules at the top
      // of the file since that's the configuration that we usualy want to
      // change. So we have the following priority bands/defaults:
      //
      // 101-200/150 - code generators (e.g., yacc, bison)
      // 201-300/250 - compilers (e.g., C, C++),
      // 301-400/350 - binutils (ar, ld)
      //
      std::multimap<std::int32_t, const_iterator> order;

      pair<iterator, bool>
      insert (string name, int prio = 0)
      {
        auto p (emplace (move (name), saved_variables ()));

        if (p.second)
          order.emplace (prio, p.first);

        return p;
      }
    };

    struct module: module_base
    {
      config::saved_modules saved_modules;

      // Return true if variable/module were newly inserted.
      //
      bool
      save_variable (const variable&, uint64_t flags = 0);

      bool
      save_module (const char* name, int prio = 0);

      static const string name;
      static const uint64_t version;
    };
  }
}

#endif // LIBBUILD2_CONFIG_MODULE_HXX
