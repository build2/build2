// file      : libbuild2/config/functions.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <sstream>

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/config/module.hxx>
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

      // $config.origin()
      //
      // Return the origin of the value of the specified configuration
      // variable. Possible result values and their semantics are as follows:
      //
      // undefined
      //    The variable is undefined.
      //
      // default
      //    The variable has the default value from the config directive (or
      //    as specified by a module).
      //
      // buildfile
      //    The variable has the value from a buildfile, normally config.build
      //    but could also be from file(s) specified with config.config.load.
      //
      // override
      //    The variable has the command line override value. Note that if
      //    the override happens to be append/prepend, then the value could
      //    incorporate the original value.
      //
      // Note that the variable must be specified as a name and not as an
      // expansion (i.e., without $).
      //
      // Note that this function is not pure.
      //
      f.insert (".origin", false) += [] (const scope* s, names name)
      {
        if (s == nullptr)
          fail << "config.origin() called out of scope" << endf;

        // Only look in the root scope since that's the only config.*
        // variables we generally consider.
        //
        s = s->root_scope ();

        if (s == nullptr)
          fail << "config.origin() called out of project" << endf;

        switch (origin (*s, convert<string> (move (name))).first)
        {
        case variable_origin::undefined: return "undefined";
        case variable_origin::default_:  return "default";
        case variable_origin::buildfile: return "buildfile";
        case variable_origin::override_: return "override";
        }

        return ""; // Should not reach.
      };

      // $config.save()
      //
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

        // See save_config() for details.
        //
        assert (s->ctx.phase == run_phase::load);
        const module* mod (s->find_module<module> (module::name));

        if (mod == nullptr)
          fail << "config.save() called without config module";

        ostringstream os;

        // Empty project set should is ok as long as inherit is false.
        //
        project_set ps;
        save_config (*s,
                     os, path_name ("config.save()"),
                     false /* inherit */,
                     *mod,
                     ps);

        return os.str ();
      };
    }
  }
}
