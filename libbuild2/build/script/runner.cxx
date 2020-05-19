// file      : libbuild2/build/script/runner.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/runner.hxx>

#include <libbuild2/script/run.hxx>

namespace build2
{
  namespace build
  {
    namespace script
    {
      void default_runner::
      enter (environment&, const location&)
      {
        // Noop.
      }

      void default_runner::
      leave (environment& env, const location& ll)
      {
        clean (env, ll);
      }

      void default_runner::
      run (environment& env,
           const command_expr& expr,
           size_t li,
           const location& ll)
      {
        if (verb >= 3)
          text << ":  " << expr;

        // Run the expression if we are not in the dry run mode or if it
        // executes the set builtin and print the expression otherwise, unless
        // it is already printed or the verbosity level is lower than 2.
        //
        // @@ Should we also run expressions that execute the exit builtin in
        //    the dry run mode?
        //
        if (!env.context.dry_run ||
            find_if (expr.begin (), expr.end (),
                     [] (const expr_term& et)
                     {
                       return et.pipe.back ().program.string () == "set";
                     }) != expr.end ())
          build2::script::run (env, expr, li, ll);
        else if (verb == 2)
          text << expr;
      }

      bool default_runner::
      run_if (environment& env,
              const command_expr& expr,
              size_t li, const location& ll)
      {
        if (verb >= 3)
          text << ": ?" << expr;

        return build2::script::run_if (env, expr, li, ll);
      }
    }
  }
}
