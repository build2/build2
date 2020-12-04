// file      : libbuild2/config/functions.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <sstream>

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/config/operation.hxx>

using namespace std;

namespace build2
{
  namespace config
  {
    void
    functions (function_map& m)
    {
      function_family f (m, "config");

      // Return the configuration file contents as a string, similar to the
      // config.config.save variable functionality.
      //
      // Note that this function can only be used during configure unless the
      // config module creation was requested for other meta-operations with
      // config.config.module=true in bootstrap.build.
      //
      // Note that this function is not pure.
      //
      f.insert (".save", false) += [] (const scope* s)
      {
        if (s == nullptr)
          fail << "config.save() called out of scope" << endf;

        s = s->root_scope ();

        if (s == nullptr)
          fail << "config.save() called out of project" << endf;

        ostringstream os;

        // Empty project set should is ok as long as inherit is false.
        //
        project_set ps;
        save_config (*s,
                     os, path_name ("config.save()"),
                     false /* inherit */,
                     ps);

        return os.str ();
      };
    }
  }
}
