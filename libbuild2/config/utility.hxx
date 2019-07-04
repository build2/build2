// file      : libbuild2/config/utility.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CONFIG_UTILITY_HXX
#define LIBBUILD2_CONFIG_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  class scope;

  namespace config
  {
    // Set, if necessary, a required config.* variable.
    //
    // If override is true and the variable doesn't come from this root scope
    // or from the command line (i.e., it is inherited from the amalgamtion),
    // then its value is "overridden" to the default value on this root scope.
    // See save_variable() for more information on save_flags.
    //
    // Return the reference to the value as well as the indication of whether
    // the value is "new", that is, it was set to the default value (inherited
    // or not, including overrides). We also treat command line overrides
    // (inherited or not) as new. This flag is usually used to test that the
    // new value is valid, print report, etc. We return the value as lookup
    // (always defined) to pass alone its location (could be used to detect
    // inheritance, etc).
    //
    // Note also that if save_flags has save_commented, then a default value
    // is never considered "new" since for such variables absence of a value
    // means the default value.
    //
    template <typename T>
    pair<lookup, bool>
    required (scope& root,
              const variable&,
              const T& default_value,
              bool override = false,
              uint64_t save_flags = 0);

    // Note that the variable is expected to have already been registered.
    //
    template <typename T>
    inline pair<lookup, bool>
    required (scope& root,
              const string& name,
              const T& default_value,
              bool override = false,
              uint64_t save_flags = 0)
    {
      return required (
        root, var_pool[name], default_value, override, save_flags);
    }

    inline pair<lookup, bool>
    required (scope& root,
              const string& name,
              const char* default_value,
              bool override = false,
              uint64_t save_flags = 0)
    {
      return required (
        root, name, string (default_value), override, save_flags);
    }

    // As above, but leave the unspecified value as undefined rather than
    // setting it to the default value.
    //
    // This can be useful when we don't have a default value but may figure
    // out some fallback. See config.bin.target for an example.
    //
    LIBBUILD2_SYMEXPORT pair<lookup, bool>
    omitted (scope& root, const variable&);

    // Note that the variable is expected to have already been registered.
    //
    inline pair<lookup, bool>
    omitted (scope& root, const string& name)
    {
      return omitted (root, var_pool[name]);
    }

    // Set, if necessary, an optional config.* variable. In particular, an
    // unspecified variable is set to NULL which is used to distinguish
    // between the "configured as unspecified" and "not yet configured" cases.
    //
    // Return the value (as always defined lookup), which can be NULL.
    //
    // @@ Rename since clashes with the optional class template.
    //
    LIBBUILD2_SYMEXPORT lookup
    optional (scope& root, const variable&);

    // Note that the variable is expected to have already been registered.
    //
    inline lookup
    optional (scope& root, const string& name)
    {
      return optional (root, var_pool[name]);
    }

    // Check whether there are any variables specified from the config
    // namespace. The idea is that we can check if there are any, say,
    // config.install.* values. If there are none, then we can assume
    // this functionality is not (yet) used and omit writing a whole
    // bunch of NULL config.install.* values to the config.build file.
    // We call it omitted/delayed configuration.
    //
    // Note that this function detects and ignores the special
    // config.*.configured variable which may be used by a module to
    // "remember" that it is unconfigured (e.g., in order to avoid re-
    // running the tests, etc).
    //
    LIBBUILD2_SYMEXPORT bool
    specified (scope& root, const string& name);

    // Check if there is a false config.*.configured value. This mechanism can
    // be used to "remember" that the module is left unconfigured in order to
    // avoid re-running the tests, etc.
    //
    LIBBUILD2_SYMEXPORT bool
    unconfigured (scope& root, const string& name);

    // Set the config.*.configured value. Note that you only need to set it to
    // false. It will be automatically ignored if there are any other config.*
    // values for this module. Return true if this sets a new value.
    //
    LIBBUILD2_SYMEXPORT bool
    unconfigured (scope& root, const string& name, bool);

    // Enter the variable so that it is saved during configuration. See
    // config::module for details.
    //
    const uint64_t save_commented = 0x01; // Save default value as commented.

    LIBBUILD2_SYMEXPORT void
    save_variable (scope& root, const variable&, uint64_t flags = 0);

    // Establish module order/priority. See config::module for details.
    //
    LIBBUILD2_SYMEXPORT void
    save_module (scope& root, const char* name, int prio = 0);

    // Create a project in the specified directory.
    //
    LIBBUILD2_SYMEXPORT void
    create_project (const dir_path& d,
                    const build2::optional<dir_path>& amalgamation,
                    const strings& boot_modules,    // Bootstrap modules.
                    const string&  root_pre,        // Extra root.build text.
                    const strings& root_modules,    // Root modules.
                    const string&  root_post,       // Extra root.build text.
                    bool config,                    // Load config module.
                    bool buildfile,                 // Create root buildfile.
                    const char* who,                // Who is creating it.
                    uint16_t verbosity = 1);        // Diagnostic verbosity.

    inline path
    config_file (const scope& root)
    {
      return (root.out_path () /
              root.root_extra->build_dir /
              "config." + root.root_extra->build_ext);
    }
  }
}

#include <libbuild2/config/utility.txx>

#endif // LIBBUILD2_CONFIG_UTILITY_HXX
