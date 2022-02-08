// file      : libbuild2/version/module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_VERSION_MODULE_HXX
#define LIBBUILD2_VERSION_MODULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

namespace build2
{
  namespace version
  {
    // A map of package names sanitized for use in variable names to the
    // 'depends' values from manifest.
    //
    using package_name = project_name;

    struct dependency
    {
      package_name name;
      string constraint;
      bool buildtime;
    };

    using dependencies = map<string, dependency>;

    struct module: build2::module
    {
      using dependencies_type = version::dependencies;

      static const string name;

      // The project variable value sanitized for use in variable names.
      //
      const string project;

      butl::standard_version version;
      bool committed; // Whether this is a committed snapshot.
      bool rewritten; // Whether this is a rewritten .z snapshot.

      dependencies_type dependencies;

      module (const project_name& p,
              butl::standard_version v,
              bool c,
              bool r,
              dependencies_type d)
          : project (p.variable ()),
            version (move (v)),
            committed (c),
            rewritten (r),
            dependencies (move (d)) {}
    };
  }
}

#endif // LIBBUILD2_VERSION_MODULE_HXX
