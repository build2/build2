// file      : libbuild2/config/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/config/init.hxx>

#include <sstream>
#include <cstdlib> // getenv()

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
    static const file_rule file_rule_ (true /* check_type */);

    void
    functions (function_map&); // functions.cxx

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
        return make_pair (reverse (d, storage, true /* reduce */), "=");

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
                       return saved_environment::compare (
                         v, v1, p, v1.find ('='));
                     }) != i)
          continue;

        // Check if there is the same value in base.
        //
        auto j (find_if (bs.rbegin (), bs.rend (),
                         [&v, p] (const string& v1)
                         {
                           return saved_environment::compare (
                             v, v1, p, v1.find ('='));
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
      // All the variables we enter are qualified so go straight for the
      // public variable pool.
      //
      auto& vp (rs.var_pool (true /* public */));

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
      // configure_execute() even for the forward case.
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
      // Note: must be entered during bootstrap since we need it in create's
      // save_config().
      //
      vp.insert<vector<pair<string, string>>> (
        "config.config.persist", true /* ovr */, v_p);

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
          // Adjust priority for the config module and import pseudo-module so
          // that their variables come first in config.build.
          //
          m.save_module ("config", INT32_MIN);
          m.save_module ("import", INT32_MIN);
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

    extern const char host_config_no_warnings[];
    extern const char build2_config_no_warnings[];
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

      context& ctx (rs.ctx);

      l5 ([&]{trace << "for " << rs;});

      // If we have the module, then we are configuring (or the project wishes
      // to call $config.save(); we don't get here on disfigure).
      //
      module* m (extra.module != nullptr
                 ? &extra.module_as<module> ()
                 : nullptr);

      auto& vp (rs.var_pool (true /* public */));

      // Note: config.* is pattern-typed to global visibility.
      //
      const auto v_p (variable_visibility::project);

      auto& c_v (vp.insert<uint64_t> ("config.version", false /*ovr*/, v_p));
      auto& c_l (vp.insert<paths> ("config.config.load", true /* ovr */));

      // Omit loading the configuration from the config.build file (it is
      // still loaded from config.config.load if specified). Similar to
      // config.config.load, only values specified on this project's root
      // scope and global scope are considered.
      //
      // Note that this variable is not saved in config.build and is expected
      // to always be specified as a command line override.
      //
      auto& c_u (vp.insert<bool> ("config.config.unload", true /*ovr*/));

      // Configuration variables to disfigure.
      //
      // The exact semantics is to ignore these variables when loading
      // config.build (and any files specified in config.config.load), letting
      // them to take on the default values (more precisely, the current
      // implementation undefined them after loading config.build). See also
      // config.config.unload.
      //
      // Besides names, variables can also be specified as patterns in the
      // config.<prefix>.(*|**)[<suffix>] form where `*` matches single
      // component names (i.e., `foo` but not `foo.bar`), and `**` matches
      // single and multi-component names. Currently only single wildcard (`*`
      // or `**`) is supported.  Additionally, a pattern in the
      // config.<prefix>(*|**) form (i.e., without `.` after <prefix>) matches
      // config.<prefix>.(*|**) plus config.<prefix> itself (but not
      // config.<prefix>foo).
      //
      // For example, to disfigure all the project configuration variables
      // (while preserving all the module configuration variables; note
      // quoting to prevent pattern expansion):
      //
      // b config.config.disfigure="'config.hello**'"
      //
      // Note that this variable is not saved in config.build and is expected
      // to always be specified as a command line override.
      //
      // We also had the idea of using NULL values as a more natural way to
      // undefine a configuration variable, which would only work for non-
      // nullable variables (such as project configuration variables) or for
      // those where NULL is the default value (most of the others). However,
      // this cannot work in our model since we cannot reset a NULL override
      // to a default value. So setting the variable itself to some special
      // value does not seem to be an option and we have to convey this in
      // some other way, such as in config.config.disfigure. Another idea is
      // to invent a parallel set of variables, such as disfig.*, that can be
      // used for that (though they would still have to be specified with some
      // dummy value, for example disfig.hello.fancy=). On the other hand,
      // this desire to disfigure individual variables does not seem to be
      // very common (we lived without it for years without noticing). So
      // it's not clear we need to do something like disfig.* which has a
      // wiff of hack to it.
      //
      auto& c_d (vp.insert<strings> ("config.config.disfigure", true /*ovr*/));

      // Hermetic configurations.
      //
      // A hermetic configuration stores environment variables that affect the
      // project in config.config.environment.
      //
      // Note that this is essentially a tri-state value: true means keep
      // hermetizing (save the environment in config.config.environment),
      // false means keep de-hermetizing (clear config.config.environment) and
      // undefined/NULL means don't touch config.config.environment.
      //
      // During reconfiguration things stay hermetic unless re-hermetization
      // is explicitly requested with config.config.hermetic.reload=true (or
      // de-hermetization is requested with config.config.hermetic=false).
      //
      // Use the NULL value to clear.
      //
      auto& c_h (vp.insert<bool> ("config.config.hermetic", true /* ovr */));

      if (m != nullptr)
        m->save_variable (c_h, save_null_omitted);

      // Request hermetic configuration re-hermetization.
      //
      // Note that this variable is not saved in config.build and is expected
      // to always be specified as a command line override.
      //
      auto& c_h_r (
        vp.insert<bool> ("config.config.hermetic.reload", true /* ovr */));

      // Hermetic configuration environment variables inclusion/exclusion.
      //
      // This configuration variable can be used to include additional or
      // exclude existing environment variables into/from the list that should
      // be saved in order to make the configuration hermetic. For example:
      //
      // config.config.hermetic.environment="LANG PATH@false"
      //
      // Use the NULL or empty value to clear.
      //
      auto& c_h_e (
        vp.insert<hermetic_environment> ("config.config.hermetic.environment"));

      if (m != nullptr)
        m->save_variable (c_h_e, save_null_omitted | save_empty_omitted);

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

      if (m != nullptr)
        m->save_variable (c_e,
                          save_null_omitted | save_empty_omitted | save_base,
                          &save_environment);

      // Load config.build if one exists (and unless config.config.unload is
      // specified) followed by extra files specified in config.config.load
      // (we don't need to worry about disfigure since we will never be
      // init'ed).
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
        try
        {
          ifdstream ifs;
          load_config (open_file_or_stdin (fn, ifs), fn, l);
        }
        catch (const io_error& e)
        {
          fail << "unable to read buildfile " << fn << ": " << e;
        }
      };

      // Load config.build unless requested not to.
      //
      {
        // The same semantics as in config.config.load below.
        //
        bool u;
        {
          lookup l (rs[c_u]);
          u = (l &&
               (l.belongs (rs) || l.belongs (ctx.global_scope)) &&
               cast_false<bool> (l));
        }

        if (!u)
        {
          path f (config_file (rs));

          if (exists (f))
            load_config_file (f, l);
        }
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
        if (l.belongs (rs) || l.belongs (ctx.global_scope))
        {
          for (const path& f: cast<paths> (l))
          {
            location l (f);

            const string& s (f.string ());

            if (s.empty ())
              fail << "empty path in config.config.load";
            else if (s[0] != '~')
              load_config_file (f, l);
            else if (s == "~host"   || s == "~host-no-warnings" ||
                     s == "~build2" || s == "~build2-no-warnings")
            {
#ifdef BUILD2_BOOTSTRAP
              assert (false);
#else
              istringstream is (s[1] == 'h'
                                ? (s.size () == 5
                                   ? host_config
                                   : host_config_no_warnings)
                                : (s.size () == 7
                                   ? build2_config
                                   : build2_config_no_warnings));
              load_config (is, path_name (s), l);
#endif
            }
            else
              fail << "unknown special configuration name '" << s << "' in "
                   << "config.config.load";
          }
        }
      }

      // Undefine variables specified with config.config.disfigure.
      //
      if (const strings* ns = cast_null<strings> (rs[c_d]))
      {
        auto p (rs.vars.lookup_namespace ("config"));

        for (auto i (p.first); i != p.second; )
        {
          const variable& var (i->first);

          // This can be one of the overrides (__override, __prefix, etc),
          // which we skip.
          //
          if (!var.override ())
          {
            bool m (false);

            for (const string& n: *ns)
            {
              if (n.compare (0, 7, "config.") != 0)
                fail << "config.* variable expected in "
                     << "config.config.disfigure instead of '" << n << "'";

              size_t p (n.find ('*'));

              if (p == string::npos)
              {
                if ((m = var.name == n))
                  break;
              }
              else
              {
                // Pattern in one of these forms:
                //
                // config.<prefix>.(*|**)[<suffix>]
                // config.<prefix>(*|**)
                //
                // BTW, an alternative way to handle this would be to
                // translate it to a path and use our path_match() machinery,
                // similar to how we do it for build config include/exclude.
                // Perhaps one day when/if we decide to support multiple
                // wildcards.
                //
                if (p == 7)
                  fail << "config.<prefix>* pattern expected in "
                       << "config.config.disfigure instead of '" << n << "'";

                bool r (n[p + 1] == '*'); // Recursive.

                size_t pe; // Prefix end/size.
                if (n[p - 1] != '.')
                {
                  // Second form should have no suffix.
                  //
                  if (p + (r ? 2 : 1) != n.size ())
                    fail << "config.<prefix>(*|**) pattern expected in "
                         << "config.config.disfigure instead of '" << n << "'";

                  // Match just <prefix>.
                  //
                  if ((m = n.compare (0, p, var.name) == 0))
                    break;

                  pe = p;
                }
                else
                  pe = p - 1;

                // Match <prefix> followed by `.`.
                //
                if (n.compare (0, pe, var.name, 0, pe) != 0 ||
                    var.name[pe] != '.')
                  continue;

                // Match suffix.
                //
                size_t sb (p + (r ? 2 : 1)); // Suffix begin.
                size_t sn (n.size () - sb);  // Suffix size.

                size_t te; // Stem end.
                if (sn == 0) // No suffix.
                  te = var.name.size ();
                else
                {
                  if (var.name.size () < pe + 1 + sn) // Too short.
                    continue;

                  te = var.name.size () - sn;

                  if (n.compare (sb, sn, var.name, te, sn) != 0)
                    continue;
                }

                // Match stem.
                //
                if ((m = r || var.name.find ('.', pe + 1) >= te))
                  break;
              }
            }

            if (m)
            {
              i = rs.vars.erase (i); // Undefine.
              continue;
            }
          }

          ++i;
        }
      }

      // Save and cache the config.config.persist value, if any.
      //
      if (m != nullptr)
      {
        auto& c_p (*vp.find ("config.config.persist"));
        m->save_variable (c_p, save_null_omitted);
        m->persist = cast_null<vector<pair<string, string>>> (rs[c_p]);
      }

      // If we are configuring, handle config.config.hermetic.
      //
      // The overall plan is to either clear config.config.environment (if
      // c.c.h=false) or populate it with the values that affect this project
      // (if c.c.h=true). We have to do it half here (because c.c.e is used as
      // a source for the project environment and we would naturally want the
      // semantics to be equivalent to what will be saved in config.build) and
      // half in configure_execute() (because that's where we have the final
      // list of all the environment variables we need to save).
      //
      // So here we must deal with the cases where the current c.c.e value
      // will be changed: either cleared (c.c.h=false) or set to new values
      // from the "outer" environment (c.c.h.reload=true). Note also that even
      // then a c.c.e value from an amalgamation, if any, should be in effect.
      //
      if (ctx.current_mif->id == configure_id &&
          (!cast_true<bool> (rs[c_h]) ||  // c.c.h=false
           cast_false<bool> (rs[c_h_r]))) // c.c.h.r=true
      {
        rs.vars.erase (c_e); // Undefine.
      }

      // Copy config.config.environment to scope::root_extra::environment and
      // calculate its checksum.
      //
      // Note that we store shallow copies that point to the c.c.environment
      // value which means it should not change.
      //
      if (const strings* src = cast_null<strings> (rs[c_e]))
      {
        sha256 cs;
        vector<const char*>& dst (rs.root_extra->environment);

        // The idea is to only copy entries that are effective, that is those
        // that actually override something in the environment. This should be
        // both more efficient and less noisy (e.g., if we need to print this
        // in diagnostics).
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
                         return saved_environment::compare (
                           v, v1, p, v1.find ('='));
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
          cs.append (v);
        }

        if (!dst.empty ())
        {
          dst.push_back (nullptr);
          rs.root_extra->environment_checksum = cs.string ();
        }
      }

      // Register alias and fallback rule for the configure meta-operation.
      //
      rs.insert_rule<alias> (configure_id, 0, "config.alias", alias_rule::instance);

      // This allows a custom configure rule while doing nothing by default.
      //
      rs.insert_rule<target> (configure_id, 0, "config.noop", noop_rule::instance);

      // We need this rule for out-of-any-project dependencies (for example,
      // libraries imported from /usr/lib). We are registering it on the
      // global scope similar to builtin rules.
      //
      // Note: use target instead of anything more specific (such as
      // mtime_target) in order not to take precedence over the rules above.
      //
      // See a similar rule in the dist module.
      //
      rs.global_scope ().insert_rule<target> (
        configure_id, 0, "config.file", file_rule_);

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
      config_save_environment = &module::save_environment;
      config_save_module = &module::save_module;
      config_preprocess_create = &preprocess_create;
      config_configure_post = &module::configure_post;
      config_disfigure_pre = &module::disfigure_pre;

      return mod_functions;
    }
  }
}
