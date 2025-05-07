// file      : libbuild2/version/module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_VERSION_MODULE_HXX
#define LIBBUILD2_VERSION_MODULE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>

#include <libbuild2/version/export.hxx>

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

    struct LIBBUILD2_VERSION_SYMEXPORT module: build2::module
    {
      using dependencies_type = version::dependencies;

      static const string name;

      // Note that if using amalgamation manifest, everything (version,
      // dependencies, etc) except for build2_version_constraint will be
      // empty. We could, however, change that if there is a use-case (for now
      // this feels like a waste, especially the version snapshot extraction
      // from git).

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
