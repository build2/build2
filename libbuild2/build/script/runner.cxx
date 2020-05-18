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
        if (verb >= 2)
          text << expr;

        build2::script::run (env, expr, li, ll);
      }

      bool default_runner::
      run_if (environment& env,
              const command_expr& expr,
              size_t li, const location& ll)
      {
        if (verb >= 2)
          text << expr;

        return build2::script::run_if (env, expr, li, ll);
      }
    }
  }
}
