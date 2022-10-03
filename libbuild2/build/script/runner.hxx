// file      : libbuild2/build/script/runner.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BUILD_SCRIPT_RUNNER_HXX
#define LIBBUILD2_BUILD_SCRIPT_RUNNER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/build/script/script.hxx>

namespace build2
{
  namespace build
  {
    struct common;

    namespace script
    {
      class runner
      {
      public:
        // Location is the script start location (for diagnostics, etc).
        //
        virtual void
        enter (environment&, const location&) = 0;

        // Index is the 1-base index of this command line in the command list.
        // If it is 0 then it means there is only one command. This
        // information can be used, for example, to derive file names.
        //
        // Location is the start position of this command line in the script.
        // It can be used in diagnostics.
        //
        // Optionally, execute the specified function instead of the last
        // pipe command.
        //
        virtual void
        run (environment&,
             const command_expr&,
             const iteration_index*, size_t index,
             const function<command_function>&,
             const location&) = 0;

        virtual bool
        run_cond (environment&,
                  const command_expr&,
                  const iteration_index*, size_t,
                  const location&) = 0;

        // Location is the script end location (for diagnostics, etc).
        //
        virtual void
        leave (environment&, const location&) = 0;
      };

      // Run command expressions.
      //
      // In dry-run mode don't run the expressions unless they are flow
      // control construct conditions or execute the set or exit builtins, but
      // print them at verbosity level 2 and up.
      //
      class default_runner: public runner
      {
      public:
        virtual void
        enter (environment&, const location&) override;

        virtual void
        run (environment&,
             const command_expr&,
             const iteration_index*, size_t,
             const function<command_function>&,
             const location&) override;

        virtual bool
        run_cond (environment&,
                  const command_expr&,
                  const iteration_index*, size_t,
                  const location&) override;

        virtual void
        leave (environment&, const location&) override;
      };
    }
  }
}

#endif // LIBBUILD2_BUILD_SCRIPT_RUNNER_HXX
