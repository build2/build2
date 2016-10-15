// file      : build2/test/script/script.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/script>

#include <build2/target>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      script::
      script (target& tt, target& st)
          : test_target (tt), script_target (st)
      {
        // Unless we have the test variable set on the test or script target,
        // set it at the script level to the test target's path.
        //
        {
          // Note: use the same variable type as in buildfile.
          //
          const variable& var (var_pool.insert<path> ("test"));

          if (!find (var))
          {
            value& v (assign (var));

            // If this is a path-based target, then we use the path. If this
            // is a directory (alias) target, then we use the directory path.
            // Otherwise, we leave it NULL expecting the testscript to set it
            // to something appropriate, if used.
            //
            if (auto* p = tt.is_a<path_target> ())
              v = p->path ();
            else if (tt.is_a<dir> ())
              v = path (tt.dir.string ()); // Strip trailing slash.
          }
        }
      }

      lookup script::
      find (const variable& var) const
      {
        if (const value* v = vars.find (var))
          return lookup (v, &vars);

        // Switch to the corresponding buildfile variable. Note that we don't
        // want to insert a new variable into the pool (we might be running
        // concurrently). Plus, if there is no such variable, then we cannot
        // possibly find any value.
        //
        const variable* pvar (build2::var_pool.find (var.name));

        if (pvar == nullptr)
          return lookup ();

        {
          const variable& var (*pvar);

          // First check the target we are testing.
          //
          {
            // Note that we skip applying the override if we did not find any
            // value. In this case, presumably the override also affects the
            // script target and we will pick it up there. A bit fuzzy.
            //
            auto p (test_target.find_original (var, true));

            if (p.first)
            {
              if (var.override != nullptr)
                p = test_target.base_scope ().find_override (
                  var, move (p), true);

              return p.first;
            }
          }

          // Then the script target followed by the scopes it is in. Note that
          // while unlikely it is possible the test and script targets will be
          // in different scopes which brings the question of which scopes we
          // should search.
          //
          return script_target[var];
        }
      }

      value& script::
      append (const variable& var)
      {
        lookup l (find (var));

        if (l.defined () && l.belongs (*this)) // Existing var in this scope.
          return const_cast<value&> (*l);

        value& r (assign (var)); // NULL.

        //@@ I guess this is where we convert untyped value to strings?
        //
        if (l.defined ())
          r = *l; // Copy value (and type) from the outer scope.

        return r;
      }
    }
  }
}
