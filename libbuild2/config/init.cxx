// file      : libbuild2/config/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/config/init.hxx>

#include <sstream>
#include <cstdlib> // getenv()
#include <cstring> // strncmp()

#include <libbuild2/file.hxx>
#include <libbuild2/rule.hxx>
#include <libbuild2/lexer.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/filesystem.hxx>  // exists()
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/module.hxx>
#include <libbuild2/config/utility.hxx>
#include <libbuild2/config/operation.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace config
  {
    void
    functions (function_map&); // functions.cxx

    // Compare environment variable names. Note that on Windows they are
    // case-insensitive.
    //
    static inline bool
    compare_env (const string& x, size_t xn, const string& y, size_t yn)
    {
      if (xn == string::npos) xn = x.size ();
      if (yn == string::npos) yn = y.size ();

      return xn == yn &&
#ifdef _WIN32
        icasecmp (x.c_str (), y.c_str (), xn) == 0
#else
        strncmp (x.c_str (), y.c_str (), xn) == 0
#endif
        ;
    }

    // Custom save function for config.config.environment.
    //
    // It tries to optimize the storage in subprojects by appending the
    // difference (compared to the amalgamation's values) instead of storing
    // the entire values.
    //
    static pair<names_view, const char*>
    save_environment (const value& d, const value* b, names& storage)
    {
      if (b == nullptr)
        return make_pair (reverse (d, storage), "=");

      // The plan is to iterator over environment variables adding those that
      // are not in base to storage. There is, however, a complication: we may
      // see multiple entries for the same variable and only the last entry
      // should have effect. So we need to iterate in reverse and check if
      // we've already seen this variable.
      //
      const strings& ds (d.as<strings> ());
      const strings& bs (b->as<strings> ());

      for (auto i (ds.rbegin ()), e (ds.rend ()); i != e; ++i)
      {
        // Note that we must only consider variable names (up to first '=' if
        // any).
        //
        const string& v (*i);
        size_t p (v.find ('='));

        // Check if we have already seen this variable.
        //
        if (find_if (ds.rbegin (), i,
                     [&v, p] (const string& v1)
                     {
                       return compare_env (v, p, v1, v1.find ('='));
                     }) != i)
          continue;

        // Check if there is the same value in base.
        //
        auto j (find_if (bs.rbegin (), bs.rend (),
                         [&v, p] (const string& v1)
                         {
                           return compare_env (v, p, v1, v1.find ('='));
                         }));

        if (j == bs.rend () || *j != v)
          storage.push_back (name (v));
      }

      return make_pair (names_view (storage), "+=");
    }

    void
    boot (scope& rs, const location&, module_boot_extra& extra)
    {
      tracer trace ("config::boot");

      context& ctx (rs.ctx);

      l5 ([&]{trace << "for " << rs;});

      // Note that the config.<name>* variables belong to the module/project
      // <name>. So the only "special" variables we can allocate in config.**
      // are config.config.**, names that have been "gifted" to us by other
      // modules (like config.version below) as well as names that we have
      // reserved to not be valid module names (`build`). We also currently
      // treat `import` as special.
      //
      auto& vp (rs.var_pool ());

      // NOTE: all config.** variables are by default made (via a pattern) to
      // be overridable with global visibility. So we must override this if a
      // different semantics is required.
      //
      const auto v_p (variable_visibility::project);

      // While config.config.load (see below) could theoretically be specified
      // in a buildfile, config.config.save is expected to always be specified
      // as a command line override.
      //
      // Note: must be entered during bootstrap since we need it in
      // configure_execute().
      //
      vp.insert<path> ("config.config.save", true /* ovr */);

      // Configuration variables persistence mode.
      //
      // By default a config.* variable is saved in the config.build file if
      // (1) it is explicitly marked as persistent with save_variable() and
      // (2) it is not inherited from an amalgamation that also saves this
      // variable (however, there are some exception; see save_config() for
      // details). If the first condition is not met, then the variable is
      // presumed to be no longer used.
      //
      // The config.config.persist can be used to adjust this default logic.
      // It contains a list of key-value pairs with the key being a variable
      // name pattern and the value specifying the condition/action:
      //
      // <pair>      = <pattern>@<condition>=<action>
      // <condition> = unused|inherited|inherited-used|inherited-unused
      // <action>    = (save|drop)[+warn]
      //
      // The last pattern and condition that matches is used (we may want to
      // change this to more specific pattern later).
      //
      // Note that support for inherited conditions is still a @@ TODO.
      //
      // The create meta-operation by default (i.e., unless a custom value is
      // specified) saves unused config.import.* variables without a warning
      // (since there is no way to "use" such variables in a configuration).
      //
      // Note that variable patterns must be quoted, for example:
      //
      // b "config.config.persist='config.*'@unused=save+warn"
      //
      // Use the NULL value to clear.
      //
      auto& c_p (vp.insert<vector<pair<string, string>>> (
                   "config.config.persist", true /* ovr */, v_p));

      // Configuration environment variables.
      //
      // Environment variables used by tools (e.g., compilers), buildfiles
      // (e.g., $getenv()), and the build system itself (e.g., to locate
      // tools) in ways that affect the build result are in essence part of
      // the project configuration.
      //
      // This variable allows storing environment variable overrides that
      // should be applied to the environment when executing tools, etc., as
      // part of a project build. Specifically, it contains a list of
      // environment variable "sets" (<name>=<value>) and "unsets" (<name>).
      // If multiple entries are specified for the same environment variable,
      // the last entry has effect. For example:
      //
      // config.config.environment="LC_ALL=C LANG"
      //
      // Note that a subproject inherits overrides from its amalgamation (this
      // semantics is the result of the way we optimize the storage of this
      // variable in subproject's config.build; the thinking is that if a
      // variable is not overridden by the subproject then it doesn't affect
      // the build result and therefore it's irrelevant whether it has a value
      // that came from the original environment of from the amalgamation
      // override).
      //
      // Use the NULL or empty value to clear.
      //
      // @@ We could use =<name> as a "pass-through" instruction (e.g., if
      //    we need to use original value in subproject).
      //
      auto& c_e (vp.insert<strings> ("config.config.environment", true /* ovr */));

      // Only create the module if we are configuring, creating, or
      // disfiguring or if it was requested with config.config.module (useful
      // if we need to call $config.save() during other meta-operations).
      //
      // Detecting the former (configure/disfigure/creating) is a bit tricky
      // since the build2 core may not yet know if this is the case. But we
      // know.
      //
      auto& c_m (vp.insert<bool> ("config.config.module", false /*ovr*/, v_p));

      bool d;
      if ((d = ctx.bootstrap_meta_operation ("disfigure")) ||
          ctx.bootstrap_meta_operation ("configure")       ||
          ctx.bootstrap_meta_operation ("create")          ||
          cast_false<bool> (rs.vars[c_m]))
      {
        auto& m (extra.set_module (new module));

        if (!d)
        {
          // Used as a variable prefix by configure_execute().
          //
          vp.insert ("config");

          // Adjust priority for the config module and import pseudo-module so
          // that their variables come first in config.build.
          //
          m.save_module ("config", INT32_MIN);
          m.save_module ("import", INT32_MIN);

          m.save_variable (c_p, save_null_omitted);
          m.save_variable (c_e,
                           save_null_omitted | save_empty_omitted | save_base,
                           &save_environment);
        }
      }

      // Register the config function family if this is the first instance of
      // the config module.
      //
      if (!function_family::defined (ctx.functions, "config"))
        functions (ctx.functions);

      // Register meta-operations. Note that we don't register create_id
      // since it will be pre-processed into configure.
      //
      rs.insert_meta_operation (configure_id, mo_configure);
      rs.insert_meta_operation (disfigure_id, mo_disfigure);

      // Initialize first (load config.build).
      //
      extra.init = module_boot_init::before_first;
    }

    // host-config.cxx.in
    //
#ifndef BUILD2_BOOTSTRAP
    extern const char host_config[];
    extern const char build2_config[];
#endif

    bool
    init (scope& rs,
          scope&,
          const location& l,
          bool first,
          bool,
          module_init_extra& extra)
    {
      tracer trace ("config::init");

      if (!first)
      {
        warn (l) << "multiple config module initializations";
        return true;
      }

      l5 ([&]{trace << "for " << rs;});

      auto& vp (rs.var_pool ());

      // Note: config.* is pattern-typed to global visibility.
      //
      const auto v_p (variable_visibility::project);

      auto& c_l (vp.insert<paths> ("config.config.load", true /* ovr */));
      auto& c_v (vp.insert<uint64_t> ("config.version", false /*ovr*/, v_p));

      // Load config.build if one exists followed by extra files specified in
      // config.config.load (we don't need to worry about disfigure since we
      // will never be init'ed).
      //
      auto load_config = [&rs, &c_v] (istream& is,
                                      const path_name& in,
                                      const location& l)
      {
        // Check the config version. We assume that old versions cannot
        // understand new configs and new versions are incompatible with old
        // configs.
        //
        // We extract the value manually instead of loading and then checking
        // in order to be able to fixup/migrate the file which we may want to
        // do in the future.
        //

        // This is tricky for stdin since we cannot reopen it (or put more
        // than one character back). So what we are going to do is continue
        // reading after extracting the variable. One side effect of this is
        // that we won't have the config.version variable entered in the scope
        // but that is harmless (we could do it manually if necessary though
        // it's not clear which it should be if we load multiple files).
        //
        lexer lex (is, in);

        // Assume missing version is 0.
        //
        optional<value> ov (extract_variable (rs.ctx, lex, c_v));
        uint64_t v (ov ? cast<uint64_t> (*ov) : 0);

        if (v != module::version)
          fail (l) << "incompatible config file " << in <<
            info << "config file version   " << v << (ov ? "" : " (missing)") <<
            info << "config module version " << module::version <<
            info << "consider reconfiguring " << project (rs) << '@'
                   << rs.out_path ();

        // Treat it as continuation of bootstrap to avoid project switching
        // (see switch_scope() for details).
        //
        source (rs, rs, lex, load_stage::boot);
      };

      auto load_config_file = [&load_config] (const path& f, const location& l)
      {
        path_name fn (f);
        ifdstream ifs;
        load_config (open_file_or_stdin (fn, ifs), fn, l);
      };

      {
        path f (config_file (rs));

        if (exists (f))
          load_config_file (f, l);
      }

      if (lookup l = rs[c_l])
      {
        // Only load files that were specified on our root scope as well as
        // global overrides. This way we can use our override "positioning"
        // machinery (i.e., where the override applies) to decide where the
        // extra config is loaded. The resulting semantics feels quite natural
        // and consistent with command line variable overrides:
        //
        // b   config.config.load=.../config.build  # outermost amalgamation
        // b ./config.config.load=.../config.build  # this project
        // b  !config.config.load=.../config.build  # every project
        //
        if (l.belongs (rs) || l.belongs (rs.ctx.global_scope))
        {
          for (const path& f: cast<paths> (l))
          {
            location l (f);

            const string& s (f.string ());

            if (s[0] != '~')
              load_config_file (f, l);
            else if (s == "~host" || s == "~build2")
            {
#ifdef BUILD2_BOOTSTRAP
              assert (false);
#else
              istringstream is (s[1] == 'h' ? host_config : build2_config);
              load_config (is, path_name (s), l);
#endif
            }
            else
              fail << "unknown special configuration name '" << s << "' in "
                   << "config.config.load";
          }
        }
      }

      // Cache the config.config.persist value, if any.
      //
      if (extra.module != nullptr)
      {
        auto& m (extra.module_as<module> ());

        m.persist =
          cast_null<vector<pair<string, string>>> (
            rs["config.config.persist"]);
      }

      // Copy config.config.environment to scope::root_extra::environment.
      //
      // Note that we store shallow copies that point to the c.c.environment
      // value which means it shall not change.
      //
      if (const strings* src = cast_null<strings> (
            rs["config.config.environment"]))
      {
        vector<const char*>& dst (rs.root_extra->environment);

        // The idea is to only copy entries that are effective, that is those
        // that actually override something in the environment. This should be
        // both more efficient and less noisy (e.g., if we decide to print
        // this in diagnostics).
        //
        // Note that config.config.environment may contain duplicates and the
        // last entry should have effect.
        //
        // Note also that we use std::getenv() instead of butl::getenv() to
        // disregard any thread environment overrides.
        //
        for (auto i (src->rbegin ()), e (src->rend ()); i != e; ++i)
        {
          // Note that we must only consider variable names (up to first '='
          // if any).
          //
          const string& v (*i);
          size_t p (v.find ('='));

          // Check if we have already seen this variable.
          //
          if (find_if (src->rbegin (), i,
                       [&v, p] (const string& v1)
                       {
                         return compare_env (v, p, v1, v1.find ('='));
                       }) != i)
            continue;

          // If it's an unset, see if it actually unsets anything.
          //
          if (p == string::npos)
          {
            if (std::getenv (v.c_str ()) == nullptr)
              continue;
          }
          //
          // And if it's a set, see if it sets a different value.
          //
          else
          {
            const char* v1 (std::getenv (string (v, 0, p).c_str ()));
            if (v1 != nullptr && v.compare (p + 1, string::npos, v1) == 0)
              continue;
          }

          dst.push_back (v.c_str ());
        }

        if (!dst.empty ())
          dst.push_back (nullptr);
      }

      // Register alias and fallback rule for the configure meta-operation.
      //
      // We need this rule for out-of-any-project dependencies (e.g.,
      // libraries imported from /usr/lib). We are registring it on the
      // global scope similar to builtin rules.
      //
      rs.global_scope ().insert_rule<mtime_target> (
        configure_id, 0, "config.file", file_rule::instance);

      //@@ outer
      rs.insert_rule<alias> (configure_id, 0, "config.alias", alias_rule::instance);

      // This allows a custom configure rule while doing nothing by default.
      //
      rs.insert_rule<target> (configure_id, 0, "config", noop_rule::instance);
      rs.insert_rule<file> (configure_id, 0, "config.file", noop_rule::instance);

      return true;
    }

    static const module_functions mod_functions[] =
    {
      {"config", &boot,   &init},
      {nullptr,  nullptr, nullptr}
    };

    const module_functions*
    build2_config_load ()
    {
      // Initialize the config entry points in the build system core.
      //
      config_save_variable = &module::save_variable;
      config_save_module = &module::save_module;
      config_preprocess_create = &preprocess_create;
      config_configure_post = &module::configure_post;
      config_disfigure_pre = &module::disfigure_pre;

      return mod_functions;
    }
  }
}
