// file      : libbuild2/config/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CONFIG_UTILITY_HXX
#define LIBBUILD2_CONFIG_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Note that the utility functions in this file are part of the build system
  // core rather than the config module. They define the basic configuration
  // semantics that should be applicable to both transient configurations as
  // well as to other implementations of configuration persistence.
  //
  // The only persistence-specific aspects of this functionality are marking
  // of the variables as to be persisted (saved, potentially with flags),
  // establishing the module saving order (priority), and configuration
  // creation (the create meta-operation implementation) These are accessed
  // through the config module entry points (which are NULL for transient
  // configurations). Note also that the exact interpretation of the save
  // flags and module order depends on the config module implementation (which
  // may ignore them as not applicable). An implementation may also define
  // custom save flags (for example, accessible through the config.save
  // attribute). Such flags should start from 0x100000000.
  //
  LIBBUILD2_SYMEXPORT extern void
  (*config_save_variable) (scope&, const variable&, uint64_t);

  LIBBUILD2_SYMEXPORT extern void
  (*config_save_module) (scope&, const char*, int);

  LIBBUILD2_SYMEXPORT extern const string&
  (*config_preprocess_create) (context&,
                               values&,
                               vector_view<opspec>&,
                               bool,
                               const location&);

  namespace config
  {
    // Mark the variable to be saved during configuration.
    //
    const uint64_t save_default_commented = 0x01; // Based on value::extra.
    const uint64_t save_null_omitted      = 0x02; // Treat NULL as undefined.

    inline void
    save_variable (scope& rs, const variable& var, uint64_t flags = 0)
    {
      if (config_save_variable != nullptr)
        config_save_variable (rs, var, flags);
    }

    // Establish module save order/priority with INT32_MIN being the highest.
    // Modules with the same priority are saved in the order inserted.
    //
    // Generally, for user-editable persisten configuration, we want higher-
    // level modules at the top of the file since that's the configuration
    // that the user usually wants to change. As a result, we define the
    // following priority bands/defaults:
    //
    // 101-200/150 - code generators (e.g., yacc, bison)
    // 201-300/250 - compilers (e.g., C, C++),
    // 301-400/350 - binutils (ar, ld)
    //
    inline void
    save_module (scope& rs, const char* module, int prio = 0)
    {
      if (config_save_module != nullptr)
        config_save_module (rs, module, prio);
    }

    // Set, if necessary, a required config.* variable.
    //
    // If override is true and the variable doesn't come from this root scope
    // or from the command line (i.e., it is inherited from the amalgamation),
    // then its value is "overridden" to the default value on this root scope.
    // See save_variable() for more information on save_flags.
    //
    // Return the reference to the value as well as the indication of whether
    // the value is "new", that is, it was set to the default value (inherited
    // or not, including overrides). We also treat command line overrides
    // (inherited or not) as new. This flag is usually used to test that the
    // new value is valid, print report, etc. We return the value as lookup
    // (always defined) to pass along its location (could be used to detect
    // inheritance, etc).
    //
    // Note also that if save_flags has save_default_commented, then a default
    // value is never considered "new" since for such variables absence of a
    // value means the default value.
    //
    // @@ Should save_null_omitted be interpreted to treat null as undefined?
    //    Sounds logical.
    //
    template <typename T>
    pair<lookup, bool>
    required (scope& rs,
              const variable&,
              T&& default_value,
              bool override = false,
              uint64_t save_flags = 0);

    // Note that the variable is expected to have already been entered.
    //
    template <typename T>
    inline pair<lookup, bool>
    required (scope& rs,
              const string& var,
              T&& default_value,
              bool override = false,
              uint64_t save_flags = 0)
    {
      return required (rs,
                       rs.ctx.var_pool[var],
                       std::forward<T> (default_value), // VC14
                       override,
                       save_flags);
    }

    inline pair<lookup, bool>
    required (scope& rs,
              const string& var,
              const char* default_value,
              bool override = false,
              uint64_t save_flags = 0)
    {
      return required (rs, var, string (default_value), override, save_flags);
    }

    // As above, but leave the unspecified value as undefined rather than
    // setting it to the default value.
    //
    // This can be useful when we don't have a default value but may figure
    // out some fallback. See config.bin.target for an example.
    //
    LIBBUILD2_SYMEXPORT pair<lookup, bool>
    omitted (scope& rs, const variable&);

    // Note that the variable is expected to have already been entered.
    //
    inline pair<lookup, bool>
    omitted (scope& rs, const string& var)
    {
      return omitted (rs, rs.ctx.var_pool[var]);
    }

    // Set, if necessary, an optional config.* variable. In particular, an
    // unspecified variable is set to NULL which is used to distinguish
    // between the "configured as unspecified" and "not yet configured" cases.
    //
    // Return the value (as always defined lookup), which can be NULL.
    //
    // @@ Rename since clashes with the optional class template.
    //
    // @@ Does it make sense to return the new indicator here as well,
    //    for consistency/generality.
    //
    LIBBUILD2_SYMEXPORT lookup
    optional (scope& rs, const variable&);

    // Note that the variable is expected to have already been registered.
    //
    inline lookup
    optional (scope& rs, const string& var)
    {
      return optional (rs, rs.ctx.var_pool[var]);
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
    specified (scope& rs, const string& var);

    // Check if there is a false config.*.configured value. This mechanism can
    // be used to "remember" that the module is left unconfigured in order to
    // avoid re-running the tests, etc.
    //
    LIBBUILD2_SYMEXPORT bool
    unconfigured (scope& rs, const string& var);

    // Set the config.*.configured value. Note that you only need to set it to
    // false. It will be automatically ignored if there are any other config.*
    // values for this module. Return true if this sets a new value.
    //
    LIBBUILD2_SYMEXPORT bool
    unconfigured (scope& rs, const string& var, bool value);
  }
}

#include <libbuild2/config/utility.txx>

#endif // LIBBUILD2_CONFIG_UTILITY_HXX
