// file      : build2/test/script/runner.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TEST_SCRIPT_RUNNER_HXX
#define BUILD2_TEST_SCRIPT_RUNNER_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/diagnostics.hxx> // location

#include <build2/test/script/script.hxx>

namespace build2
{
  namespace test
  {
    struct common;

    namespace script
    {
      // An exception that can be thrown by a runner to exit the scope (for
      // example, as a result of executing the exit builtin). The status
      // indicates whether the scope should be considered to have succeeded
      // or failed.
      //
      struct exit_scope
      {
        bool status;

        explicit
        exit_scope (bool s): status (s) {}
      };

      class runner
      {
      public:
        // Return false if this test/group should be skipped.
        //
        virtual bool
        test (scope&) const = 0;

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
        virtual void
        run (scope&,
             const command_expr&, command_type,
             size_t index,
             const location&) = 0;

        virtual bool
        run_if (scope&, const command_expr&, size_t, const location&) = 0;

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

        virtual void
        enter (scope&, const location&) override;

        virtual void
        run (scope&,
             const command_expr&, command_type,
             size_t,
             const location&) override;

        virtual bool
        run_if (scope&, const command_expr&, size_t, const location&) override;

        virtual void
        leave (scope&, const location&) override;

      private:
        const common& common_;
      };
    }
  }
}

#endif // BUILD2_TEST_SCRIPT_RUNNER_HXX
