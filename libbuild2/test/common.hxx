// file      : libbuild2/test/common.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_COMMON_HXX
#define LIBBUILD2_TEST_COMMON_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

namespace build2
{
  namespace test
  {
    enum class output_before {fail, warn, clean};
    enum class output_after {clean, keep};

    struct common_data
    {
      const variable& config_test;
      const variable& config_test_output;
      const variable& config_test_timeout;
      const variable& config_test_runner;

      const variable& var_test;
      const variable& test_options;
      const variable& test_arguments;

      const variable& test_runner_path;
      const variable& test_runner_options;

      const variable& test_stdin;
      const variable& test_stdout;
      const variable& test_roundtrip;
      const variable& test_input;

      const variable& test_target;
    };

    struct common: common_data
    {
      // The config.test.output values.
      //
      output_before before = output_before::warn;
      output_after after = output_after::clean;

      // The config.test.timeout values.
      //
      optional<duration> operation_timeout;
      optional<duration> test_timeout;

      // The test.runner.{path,options} values extracted from the
      // config.test.runner value, if any.
      //
      const process_path* runner_path = nullptr;
      const strings* runner_options = nullptr;

      // The config.test query interface.
      //
      const names* test_ = nullptr; // The config.test value if any.
      scope*       root_ = nullptr; // The root scope for target resolution.

      // Store it as the underlying representation and use the release-consume
      // ordering (see mtime_target for the reasoning).
      //
      mutable atomic<timestamp::rep> operation_deadline_ {
        timestamp_unknown_rep};

      // Return the test operation deadline, calculating it on the first call
      // as an offset from now by the operation timeout.
      //
      optional<timestamp>
      operation_deadline () const;

      // Return true if the specified alias target should pass-through to its
      // prerequisites.
      //
      bool
      pass (const target& alias_target) const;

      // Return true if the specified target should be tested.
      //
      bool
      test (const target& test_target) const;

      // Return true if the specified target should be tested with the
      // specified testscript test (or group).
      //
      bool
      test (const target& test_target, const path& id_path) const;

      explicit
      common (common_data&& d): common_data (move (d)) {}
    };

    // Helpers.
    //

    // Return the nearest of the target-enclosing root scopes test operation
    // deadlines.
    //
    optional<timestamp>
    operation_deadline (const target&);

    // Return the lesser of the target-enclosing root scopes test timeouts.
    //
    optional<duration>
    test_timeout (const target&);

    // Convert the test timeouts in the target-enclosing root scopes into
    // deadlines and return the nearest between them and the operation
    // deadlines in the enclosing root scopes.
    //
    optional<timestamp>
    test_deadline (const target&);
  }
}

#endif // LIBBUILD2_TEST_COMMON_HXX
