// file      : libbuild2/config/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/config/init.hxx>

#include <sstream>

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

    bool
    boot (scope& rs, const location&, module_boot_extra& extra)
    {
      tracer trace ("config::boot");

      context& ctx (rs.ctx);

      l5 ([&]{trace << "for " << rs;});

      // Note that the config.<name>* variables belong to the module <name>.
      // So the only "special" variables we can allocate in config.* are
      // config.config.*, names that have been "gifted" to us by other modules
      // (like config.version below) as well as names that we have reserved to
      // not be valid module names (`build`). We also currently treat `import`
      // as special.
      //
      auto& vp (rs.var_pool ());

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
                   "config.config.persist",
                   true /* ovr */,
                   variable_visibility::project));

      // Only create the module if we are configuring or creating or if it was
      // requested with config.config.module (useful if we need to call
      // $config.save() during other meta-operations).
      //
      // Detecting the former (configuring/creating) is a bit tricky since the
      // build2 core may not yet know if this is the case. But we know.
      //
      auto& c_m (vp.insert<bool> ("config.config.module", false /*ovr*/));

      const string& mname (ctx.current_mname);
      const string& oname (ctx.current_oname);

      if ((                   mname == "configure" || mname == "create")  ||
          (mname.empty () && (oname == "configure" || oname == "create")) ||
          cast_false<bool> (rs.vars[c_m]))
      {
        // Used as a variable prefix by configure_execute().
        //
        vp.insert ("config");

        auto& m (extra.set_module (new module));

        // Adjust priority for the config module and import pseudo-module so
        // that their variables come first in config.build.
        //
        m.save_module ("config", INT32_MIN);
        m.save_module ("import", INT32_MIN);

        m.save_variable (c_p, omit_null);
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

      return true; // Initialize first (load config.build).
    }

#ifndef BUILD2_BOOTSTRAP
    extern const char host_config[]; // host-config.cxx.in
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
      auto& c_l (vp.insert<paths> ("config.config.load", true /* ovr */));
      auto& c_v (vp.insert<uint64_t> ("config.version", false /*ovr*/));

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
        auto p (extract_variable (rs.ctx, lex, c_v));
        uint64_t v (p.second ? cast<uint64_t> (p.first) : 0);

        if (v != module::version)
          fail (l) << "incompatible config file " << in <<
            info << "config file version   " << v
                   << (p.second ? "" : " (missing)") <<
            info << "config module version " << module::version <<
            info << "consider reconfiguring " << project (rs) << '@'
                   << rs.out_path ();

        source (rs, rs, lex);
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
            else if (s == "~host")
            {
#ifdef BUILD2_BOOTSTRAP
              assert (false);
#else
              istringstream is (host_config);
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
      config_save_variable = &save_variable;
      config_preprocess_create = &preprocess_create;

      return mod_functions;
    }
  }
}
