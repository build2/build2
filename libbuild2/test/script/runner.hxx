// file      : libbuild2/test/script/runner.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_SCRIPT_RUNNER_HXX
#define LIBBUILD2_TEST_SCRIPT_RUNNER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/script/run.hxx> // exit

#include <libbuild2/test/script/script.hxx>

namespace build2
{
  namespace test
  {
    struct common;

    namespace script
    {
      using exit_scope = build2::script::exit;

      class runner
      {
      public:
        // Return false if this test/group should be skipped.
        //
        virtual bool
        test (scope&) const = 0;

        // Return the runner program path and options if the test commands
        // must be run via the runner and the pair of NULLs otherwise.
        //
        virtual pair<const process_path*, const strings*>
        test_runner () = 0;

        // Location is the scope start location (for diagnostics, etc).
        //
        virtual void
        enter (scope&, const location&) = 0;

        // Index is the 1-base index of this command line in the command list
        // (e.g., in a compound test). If it is 0 then it means there is only
        // one command (e.g., a simple test). This information can be used,
        // for example, to derive file names.
        //
        // Location is the start position of this command line in the
        // testscript. It can be used in diagnostics.
        //
        // Optionally, execute the specified function instead of the last
        // pipe command.
        //
        virtual void
        run (scope&,
             const command_expr&, command_type,
             const iteration_index*, size_t index,
             const function<command_function>&,
             const location&) = 0;

        virtual bool
        run_cond (scope&,
                  const command_expr&,
                  const iteration_index*, size_t,
                  const location&) = 0;

        // Location is the scope end location (for diagnostics, etc).
        //
        virtual void
        leave (scope&, const location&) = 0;
      };

      class default_runner: public runner
      {
      public:
        explicit
        default_runner (const common& c): common_ (c) {}

        virtual bool
        test (scope& s) const override;

        // Return the test.runner.{path,options} values, if config.test.runner
        // is specified.
        //
        virtual pair<const process_path*, const strings*>
        test_runner () override;

        virtual void
        enter (scope&, const location&) override;

        virtual void
        run (scope&,
             const command_expr&, command_type,
             const iteration_index*, size_t,
             const function<command_function>&,
             const location&) override;

        virtual bool
        run_cond (scope&,
                  const command_expr&,
                  const iteration_index*, size_t,
                  const location&) override;

        virtual void
        leave (scope&, const location&) override;

      private:
        const common& common_;
      };
    }
  }
}

#endif // LIBBUILD2_TEST_SCRIPT_RUNNER_HXX
