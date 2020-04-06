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

    // Lookup a config.* variable value and, if the value is defined, mark it
    // as saved.
    //
    // The second version in addition sets the new_value argument to true if
    // the value is "new" (but not to false; so it can be used to accumulate
    // the result from multiple calls). A value is considered new if it was
    // set to the default value (inherited or not, including overrides). We
    // also treat command line overrides (inherited or not) as new. For this
    // version new means either the default value was inherited or it was
    // overridden. This flag is usually used to test that the new value is
    // valid, print the configuration report, etc.
    //
    // Unlike the rest of the lookup_config() versions, this one leaves the
    // unspecified value as undefined rather than setting it to a default
    // value. This can be useful when we don't have a default value or in case
    // we want the mentioning of the variable to be omitted from persistent
    // storage (e.g., a config file) if the default value is used.
    //
    // Note also that we can first do the lookup without the default value and
    // then, if there is no value, call the version with the default value and
    // end up with the same result as if we called the default value version
    // straight away. This is useful when computing the default value is
    // expensive. It is also ok to call both versions multiple times provided
    // the flags are the same.
    //
    // @@ Should we pass flags and interpret save_null_omitted to treat null
    //    as undefined? Sounds logical.
    //
    lookup
    lookup_config (scope& rs, const variable&);

    lookup
    lookup_config (bool& new_value, scope& rs, const variable&);

    // Note that the variable is expected to have already been entered.
    //
    inline lookup
    lookup_config (scope& rs, const string& var)
    {
      return lookup_config (rs, rs.ctx.var_pool[var]);
    }

    inline lookup
    lookup_config (bool& new_value, scope& rs, const string& var)
    {
      return lookup_config (new_value, rs, rs.ctx.var_pool[var]);
    }

    // Lookup a config.* variable value and, if the value is undefined, set it
    // to the default. Always mark it as saved.
    //
    // If the default value is nullptr, then the unspecified value is set to
    // NULL which can be used to distinguish between the "not yet configured",
    // "configured as unspecified", and "configures as empty" cases which can
    // have different semantics if the value is merged into a non-config.*
    // variable. This default value is traditionally used for "optional"
    // values such as command line options.
    //
    // The value is returned as lookup (even though it is always defined
    // though potentially as NULL) in order to pass along its location (could
    // be used to detect inheritance, etc).
    //
    // The second version in addition sets the new_value argument as described
    // above. Note, however, that if the save_default_commented flag is
    // specified, then the default value is never considered "new" since for
    // such variables absence of a value means it is the default value. This
    // flag is normally used for dynamically adjusting (e.g., hinted) default
    // values.
    //
    // If override is true and the variable doesn't come from this root scope
    // or from the command line (i.e., it is inherited from the amalgamation),
    // then its value is "overridden" to the default value on this root scope.
    //
    // @@ Should save_null_omitted be interpreted to treat null as undefined?
    //    Sounds logical.
    //
    template <typename T>
    lookup
    lookup_config (scope& rs,
                   const variable&,
                   T&& default_value,
                   uint64_t save_flags = 0,
                   bool override = false);

    template <typename T>
    lookup
    lookup_config (bool& new_value,
                   scope& rs,
                   const variable&,
                   T&& default_value,
                   uint64_t save_flags = 0,
                   bool override = false);

    inline lookup
    lookup_config (scope& rs,
                   const variable& var,
                   const char* default_value,
                   uint64_t save_flags = 0,
                   bool override = false)
    {
      return lookup_config (
        rs, var, string (default_value), save_flags, override);
    }

    inline lookup
    lookup_config (bool& new_value,
                   scope& rs,
                   const variable& var,
                   const char* default_value,
                   uint64_t save_flags = 0,
                   bool override = false)
    {
      return lookup_config (
        new_value, rs, var, string (default_value), save_flags, override);
    }

    // Note that the variable is expected to have already been entered.
    //
    template <typename T>
    inline lookup
    lookup_config (scope& rs,
                   const string& var,
                   T&& default_value,
                   uint64_t save_flags = 0,
                   bool override = false)
    {
      return lookup_config (rs,
                            rs.ctx.var_pool[var],
                            std::forward<T> (default_value), // VC14
                            save_flags,
                            override);
    }

    template <typename T>
    inline lookup
    lookup_config (bool& new_value,
                   scope& rs,
                   const string& var,
                   T&& default_value,
                   uint64_t save_flags = 0,
                   bool override = false)
    {
      return lookup_config (new_value,
                            rs,
                            rs.ctx.var_pool[var],
                            std::forward<T> (default_value), // VC14
                            save_flags,
                            override);
    }

    inline lookup
    lookup_config (scope& rs,
                   const string& var,
                   const char* default_value,
                   uint64_t save_flags = 0,
                   bool override = false)
    {
      return lookup_config (
        rs, var, string (default_value), save_flags, override);
    }

    inline lookup
    lookup_config (bool& new_value,
                   scope& rs,
                   const string& var,
                   const char* default_value,
                   uint64_t save_flags = 0,
                   bool override = false)
    {
      return lookup_config (
        new_value, rs, var, string (default_value), save_flags, override);
    }

    // Check whether there are any variables specified from the config.<name>
    // namespace. The idea is that we can check if there are any, say,
    // config.install.* values. If there are none, then we can assume this
    // functionality is not (yet) used and omit writing a whole bunch of NULL
    // config.install.* values to the config.build file.  We call this
    // omitted/delayed configuration.
    //
    // Note that this function detects and ignores special config.* variables
    // (such as config.*.configured) which may be used by a module to remember
    // that it is unconfigured (e.g., in order to avoid re-running the tests,
    // etc; see below).
    //
    LIBBUILD2_SYMEXPORT bool
    specified_config (scope& rs, const string& var);

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

#include <libbuild2/config/utility.ixx>
#include <libbuild2/config/utility.txx>

#endif // LIBBUILD2_CONFIG_UTILITY_HXX
