// file      : build2/version/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_VERSION_MODULE_HXX
#define BUILD2_VERSION_MODULE_HXX

#include <map>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/module.hxx>

namespace build2
{
  namespace version
  {
    // The 'depends' values from manifest.
    //
    using dependency_constraints = std::map<string, string>;

    struct module: module_base
    {
      static const string name;

      const string& project; // The project variable value.

      butl::standard_version version;
      bool committed; // Whether this is a committed snapshot.
      bool rewritten; // Whether this is a rewritten .z snapshot.

      dependency_constraints dependencies;

      bool dist_uncommitted = false;

      module (const string& p,
              butl::standard_version v,
              bool c,
              bool r,
              dependency_constraints d)
          : project (p),
            version (move (v)),
            committed (c),
            rewritten (r),
            dependencies (move (d)) {}
    };
  }
}

#endif // BUILD2_VERSION_MODULE_HXX
