// file      : libbuild2/config/utility.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CONFIG_UTILITY_HXX
#define LIBBUILD2_CONFIG_UTILITY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/config/types.hxx>

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
  // establishing the module saving order (priority), configuration creation
  // (the create meta-operation implementation), as well as configure and
  // disfigure hooks (for example, for second-level configuration). These are
  // accessed through the config module entry points (which are NULL for
  // transient configurations). Note also that the exact interpretation of the
  // save flags and module order depends on the config module implementation
  // (which may ignore them as not applicable). An implementation may also
  // define custom save flags (for example, accessible through the config.save
  // attribute). Such flags should start from 0x100000000.
  //
  LIBBUILD2_SYMEXPORT extern void
  (*config_save_variable) (scope&, const variable&, optional<uint64_t>);

  LIBBUILD2_SYMEXPORT extern void
  (*config_save_environment) (scope&, const char*);

  LIBBUILD2_SYMEXPORT extern void
  (*config_save_module) (scope&, const char*, int);

  LIBBUILD2_SYMEXPORT extern const string&
  (*config_preprocess_create) (context&,
                               values&,
                               vector_view<opspec>&,
                               bool,
                               const location&);

  LIBBUILD2_SYMEXPORT extern bool
  (*config_configure_post) (scope&, bool (*)(action, const scope&));

  LIBBUILD2_SYMEXPORT extern bool
  (*config_disfigure_pre) (scope&, bool (*)(action, const scope&));

  namespace config
  {
    // Mark a variable to be saved during configuration.
    //
    // Note: the save_*_omitted flags work best when undefined or (one of) the
    // omitted value(s) is the default (see a note in lookup_config()
    // documentation for details).
    //
    // The below lookup_*() functions mark the default value by setting
    // value::extra to 1. Note that it's exactly 1 and not "not 0" since other
    // values could have other meaning (see, for example, package skeleton
    // in bpkg).
    //
    const uint64_t save_default_commented = 0x01; // Based on value::extra.
    const uint64_t save_null_omitted      = 0x02; // Treat NULL as undefined.
    const uint64_t save_empty_omitted     = 0x04; // Treat empty as undefined.
    const uint64_t save_false_omitted     = 0x08; // Treat false as undefined.
    const uint64_t save_base              = 0x10; // Custom save with base.

    inline void
    save_variable (scope& rs, const variable& var, uint64_t flags = 0)
    {
      if (config_save_variable != nullptr)
        config_save_variable (rs, var, flags);
    }

    // Mark a variable as "unsaved" (always transient).
    //
    // Such variables are not very common and are usually used to control the
    // process of configuration itself.
    //
    inline void
    unsave_variable (scope& rs, const variable& var)
    {
      if (config_save_variable != nullptr)
        config_save_variable (rs, var, nullopt);
    }

    // Mark an environment variable to be saved during hermetic configuration.
    //
    // Some notes/suggestions on saving environment variables for tools (e.g.,
    // compilers, etc):
    //
    // 1. We want to save variables that affect the result (e.g., build
    //    output) rather than byproducts (e.g., diagnostics).
    //
    // 2. Environment variables are often poorly documented (and not always in
    //    the ENVIRONMENT section; sometimes they are mentioned together with
    //    the corresponding option). A sensible approach in this case is to
    //    save documented (and perhaps well-known undocumented) variables --
    //    the user can always save additional variables if necessary. The way
    //    to discover undocumented environment variables is to grep the source
    //    code.
    //
    // 3. Sometime environment variables only affect certain modes of a tool.
    //    If such modes are not used, then there is no need to save the
    //    corresponding variables.
    //
    // 4. Finally, there could be environment variables that are incompatible
    //    with what we are doing (e.g., they change the mode of operation or
    //    some such; see GCC's DEPENDENCIES_OUTPUT for example). The two ways
    //    to deal with this is either clear them for each invocation or, if
    //    that's too burdensome and there is no good reason to have the build
    //    system invoked with such variables, detect their presence and fail.
    //    Note that unsetting them for the entire build system process is not
    //    an option since that would be racy.
    //
    // See also build2::hash_environment().
    //
    inline void
    save_environment (scope& rs, const string& var)
    {
      if (config_save_environment != nullptr)
        config_save_environment (rs, var.c_str ());
    }

    inline void
    save_environment (scope& rs, const char* var)
    {
      if (config_save_environment != nullptr)
        config_save_environment (rs, var);
    }

    inline void
    save_environment (scope& rs, initializer_list<const char*> vars)
    {
      if (config_save_environment != nullptr)
      {
        for (const char* var: vars)
          config_save_environment (rs, var);
      }
    }

    inline void
    save_environment (scope& rs, const cstrings& vars)
    {
      if (config_save_environment != nullptr)
      {
        for (const char* var: vars)
          config_save_environment (rs, var);
      }
    }

    inline void
    save_environment (scope& rs, const strings& vars)
    {
      if (config_save_environment != nullptr)
      {
        for (const string& var: vars)
          config_save_environment (rs, var.c_str ());
      }
    }

    // A NULL-terminated list of variables (may itself be NULL).
    //
    inline void
    save_environment (scope& rs, const char* const* vars)
    {
      if (vars != nullptr && config_save_environment != nullptr)
      {
        for (; *vars != nullptr; ++vars)
          config_save_environment (rs, *vars);
      }
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

    // Post-configure and pre-disfigure hooks. Normally used to save/remove
    // persistent state. Return true if anything has been done (used for
    // diagnostics).
    //
    // The registration functions return true if the hook has been registered.
    //
    // Note that the hooks are called for the top-level project and all its
    // subprojects (if registered in the subproject root scope), from outer to
    // inner for configure and from inner to outer for disfigure. It's the
    // responsibility of the hook implementation to handle any aggregation.
    //
    using configure_post_hook = bool (action, const scope&);
    using disfigure_pre_hook  = bool (action, const scope&);

    inline bool
    configure_post (scope& rs, configure_post_hook* h)
    {
      return config_configure_post != nullptr && config_configure_post (rs, h);
    }

    inline bool
    disfigure_pre (scope& rs, disfigure_pre_hook* h)
    {
      return config_disfigure_pre != nullptr && config_disfigure_pre (rs, h);
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
    // value (in this case it also doesn't mark the variable for saving with
    // the specified flags). This can be useful when we don't have a default
    // value or in case we want the mentioning of the variable to be omitted
    // from persistent storage (e.g., a config file) if the default value is
    // used.
    //
    // Note also that we can first do the lookup without the default value and
    // then, if there is no value, call the version with the default value and
    // end up with the same result as if we called the default value version
    // straight away. This is useful when computing the default value is
    // expensive. It is also ok to call both versions multiple times provided
    // the flags are the same.
    //
    lookup
    lookup_config (scope& rs,
                   const variable&,
                   uint64_t save_flags = 0);

    lookup
    lookup_config (bool& new_value,
                   scope& rs,
                   const variable&,
                   uint64_t save_flags = 0);

    // Note that the variable is expected to have already been entered.
    //
    inline lookup
    lookup_config (scope& rs,
                   const string& var,
                   uint64_t save_flags = 0)
    {
      // Note: go straight for the public variable pool.
      //
      return lookup_config (rs, rs.ctx.var_pool[var], save_flags);
    }

    inline lookup
    lookup_config (bool& new_value,
                   scope& rs,
                   const string& var,
                   uint64_t save_flags = 0)
    {
      // Note: go straight for the public variable pool.
      //
      return lookup_config (new_value, rs, rs.ctx.var_pool[var], save_flags);
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
    // Note that while it may seem logical, these functions do not
    // "reinterpret" defined values according to the save_*_omitted flags (for
    // example, by returning the default value if the defined value is NULL
    // and the save_null_omitted flag is specified). This is because such a
    // reinterpretation may cause a diversion between the returned value and
    // the re-queried config.* variable value if the defined value came from
    // an override. To put another way, the save_*_omitted flags are purely to
    // reduce the noise in config.build.
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
      // Note: go straight for the public variable pool.
      //
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
      // Note: go straight for the public variable pool.
      //
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

    // Helper functions for assigning/appending config.x.y value to x.y,
    // essentially:
    //
    // rs.assign (var) =  lookup_config (rs, "config." + var, default_value);
    // rs.append (var) += lookup_config (rs, "config." + var, default_value);
    //
    template <typename V, typename T>
    inline const V*
    assign_config (scope& rs, scope& bs, string var, T&& default_value)
    {
      const V* cv (
        cast_null<V> (
          lookup_config (rs,
                         rs.var_pool (true).insert<V> ("config." + var),
                         std::forward<T> (default_value)))); // VC14

      value& v (bs.assign<V> (move (var)));

      if (cv != nullptr)
        v = *cv;

      return v.null ? nullptr : &v.as<V> ();
    }

    template <typename V, typename T>
    inline const V*
    append_config (scope& rs, scope& bs, string var, T&& default_value)
    {
      const V* cv (
        cast_null<V> (
          lookup_config (rs,
                         rs.var_pool (true).insert<V> ("config." + var),
                         std::forward<T> (default_value)))); // VC14

      value& v (bs.append<V> (move (var)));

      if (cv != nullptr)
        v += *cv;

      return v.null ? nullptr : &v.as<V> ();
    }

    // Check whether there are any variables specified from the config.<name>
    // namespace. The idea is that we can check if there are any, say,
    // config.install.* values. If there are none, then we can assume this
    // functionality is not (yet) used and omit writing a whole bunch of NULL
    // config.install.* values to the config.build file. We call this
    // omitted/delayed configuration.
    //
    // Note that this function detects and ignores special config.* variables
    // (such as config.*.configured) which may be used by a module to remember
    // that it is unconfigured (e.g., in order to avoid re-running the tests,
    // etc; see below). Additional variables (e.g., unsaved) can be ignored
    // with the third argument. If specified, it should contain the part(s)
    // after config.<name>.
    //
    LIBBUILD2_SYMEXPORT bool
    specified_config (scope& rs,
                      const string& var,
                      initializer_list<const char*> ignore);

    inline bool
    specified_config (scope& rs, const string& var)
    {
      return specified_config (rs, var, {});
    }

    // Check if there is a false config.*.configured value. This mechanism can
    // be used to "remember" that the module is left unconfigured in order to
    // avoid re-running the tests, etc.
    //
    // @@ This functionality is WIP/unused and still has a number of issues:
    //
    // - This seems to be a subset of a bigger problem of caching discovered
    //   configuration results. In fact, what we do in the configured case,
    //   for example in the cc module (multiple path extraction runs, etc), is
    //   a lot more expensive.
    //
    // - The current semantics does not work well for the case where, say, the
    //   missing tool has appeared in PATH and can now be used via the default
    //   configuration. In fact, even reconfiguring will not help without a
    //   "nudge" (e.g., config.<tool>=<tool>). So maybe this value should be
    //   ignored during configuration? See the "Tool importation: unconfigured
    //   state" page for more notes.
    //
    LIBBUILD2_SYMEXPORT bool
    unconfigured (scope& rs, const string& var);

    // Set the config.*.configured value. Note that you only need to set it to
    // false. It will be automatically ignored if there are any other config.*
    // values for this module. Return true if this sets a new value.
    //
    LIBBUILD2_SYMEXPORT bool
    unconfigured (scope& rs, const string& var, bool value);

    // Return the origin of the value of the specified configuration variable
    // plus the value itself. See $config.origin() for details.
    //
    // Throws invalid_argument if the passed variable is not config.*.
    //
    LIBBUILD2_SYMEXPORT pair<variable_origin, lookup>
    origin (const scope& rs, const string& name);

    LIBBUILD2_SYMEXPORT pair<variable_origin, lookup>
    origin (const scope& rs, const variable&);

    // As above but using the result of scope::lookup_original() or
    // semantically equivalent (e.g., lookup_namespace()).
    //
    // Note that this version does not check that the variable is config.*.
    //
    LIBBUILD2_SYMEXPORT pair<variable_origin, lookup>
    origin (const scope& rs, const variable&, pair<lookup, size_t> original);
  }
}

#include <libbuild2/config/utility.ixx>
#include <libbuild2/config/utility.txx>

#endif // LIBBUILD2_CONFIG_UTILITY_HXX
