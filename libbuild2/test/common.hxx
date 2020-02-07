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

      const variable& var_test;
      const variable& test_options;
      const variable& test_arguments;

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

      // The config.test query interface.
      //
      const names* test_ = nullptr; // The config.test value if any.
      scope*       root_ = nullptr; // The root scope for target resolution.

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
  }
}

#endif // LIBBUILD2_TEST_COMMON_HXX
