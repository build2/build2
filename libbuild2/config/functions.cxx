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
      // Return the origin of the specified configuration variable value.
      // Possible result values and their semantics are as follows:
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

        string n (convert<string> (move (name)));

        // Make sure this is a config.* variable. This could matter since we
        // reply on the semantics of value::extra. We could also detect
        // special variables like config.booted, some config.config.*, etc.,
        // (see config_save() for details) but that seems harmless.
        //
        if (n.compare (0, 7, "config.") != 0)
          fail << "non-config.* variable passed to config.origin()" << endf;

        const variable* var (s->ctx.var_pool.find (n));

        if (var == nullptr)
          return "undefined";

        pair<lookup, size_t> org (s->lookup_original (*var));
        pair<lookup, size_t> ovr (var->overrides == nullptr
                                  ? org
                                  : s->lookup_override (*var, org));

        if (!ovr.first.defined ())
          return "undefined";

        if (org.first != ovr.first)
          return "override";

        return org.first->extra ? "default" : "buildfile";
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
        module* mod (s->rw ().find_module<module> (module::name));

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
