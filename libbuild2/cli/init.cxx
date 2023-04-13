// file      : libbuild2/cli/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cli/init.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/cxx/target.hxx>

#include <libbuild2/cli/rule.hxx>
#include <libbuild2/cli/module.hxx>
#include <libbuild2/cli/target.hxx>

namespace build2
{
  namespace cli
  {
    // Remaining issues/semantics change:
    //
    // @@ Unconfigured caching.
    //
    // @@ Default-found cli used to result in config.cli=cli and now it's just
    //    omitted (and default-not-found -- in config.cli.configured=false).
    //
    //    - Writing any default will take precedence over config.import.cli.
    //      In fact, this duality is a bigger problem: if we have a config
    //      that uses config.cli there is no way to reconfigure it to use
    //      config.import.cli.
    //
    //    - We could have saved it commented.
    //
    //    - We could do this at the module level only since we also have
    //      config.cli.options?
    //
    //    - Note that in the CLI compiler itself we now rely on default cli
    //      being NULL/undefined. So if faving, should probably be commented
    //      out. BUT: it will still be defined, so will need to be defined
    //      NULL. Note also that long term the CLI compiler will not use the
    //      module relying on an ad hoc recipe instead.
    //
    //    ! Maybe reserving NULL (instead of making it the same as NULL) for
    //      this "configured to default" state and saving commented is not a
    //      bad idea. Feels right to have some marker in config.build that
    //      things are in effect. And I believe if config.import.cli is
    //      specified, it will just be dropped.

    bool
    guess_init (scope& rs,
                scope& bs,
                const location& loc,
                bool,
                bool opt,
                module_init_extra& extra)
    {
      tracer trace ("cli::guess_init");
      l5 ([&]{trace << "for " << rs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cli.guess module must be loaded in project root";

      // Adjust module config.build save priority (code generator).
      //
      config::save_module (rs, "cli", 150);

      // Enter metadata variables.
      //
      // They are all qualified so go straight for the public variable pool.
      //
      auto& vp (rs.var_pool (true /* public */));

      auto& v_ver (vp.insert<string> ("cli.version"));
      auto& v_sum (vp.insert<string> ("cli.checksum"));

      // Import the CLI compiler target.
      //
      // Note that the special config.cli=false value (recognized by the
      // import machinery) is treated as an explicit request to leave the
      // module unconfigured.
      //
      bool new_cfg (false);
      import_result<exe> ir (
        import_direct<exe> (
          new_cfg,
          rs,
          name ("cli", dir_path (), "exe", "cli"), // cli%exe{cli}
          true      /* phase2 */,
          opt,
          true      /* metadata */,
          loc,
          "module load"));

      const exe* tgt (ir.target);

      // Extract metadata.
      //
      auto* ver (tgt != nullptr ? &cast<string> (tgt->vars[v_ver]) : nullptr);
      auto* sum (tgt != nullptr ? &cast<string> (tgt->vars[v_sum]) : nullptr);

      // Print the report.
      //
      // If this is a configuration with new values, then print the report
      // at verbosity level 2 and up (-v).
      //
      if (verb >= (new_cfg ? 2 : 3))
      {
        diag_record dr (text);
        dr << "cli " << project (rs) << '@' << rs << '\n';

        if (tgt != nullptr)
          dr << "  cli        " << ir << '\n'
             << "  version    " << *ver << '\n'
             << "  checksum   " << *sum;
        else
          dr << "  cli        " << "not found, leaving unconfigured";
      }

      if (tgt == nullptr)
        return false;

      // The cli variable (untyped) is an imported compiler target name.
      //
      rs.assign ("cli") = move (ir.name);
      rs.assign (v_sum) = *sum;
      rs.assign (v_ver) = *ver;

      {
        standard_version v (*ver);

        rs.assign<uint64_t> ("cli.version.number") = v.version;
        rs.assign<uint64_t> ("cli.version.major") = v.major ();
        rs.assign<uint64_t> ("cli.version.minor") = v.minor ();
        rs.assign<uint64_t> ("cli.version.patch") = v.patch ();
      }

      // Cache some values in the module for easier access in the rule.
      //
      extra.set_module (new module (data {*tgt, *sum}));

      return true;
    }

    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 bool,
                 bool opt,
                 module_init_extra& extra)
    {
      tracer trace ("cli::config_init");
      l5 ([&]{trace << "for " << rs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cli.config module must be loaded in project root";

      // Load cli.guess and share its module instance as ours.
      //
      if (optional<shared_ptr<build2::module>> r = load_module (
            rs, rs, "cli.guess", loc, opt, extra.hints))
      {
        extra.module = *r;
      }
      else
      {
        // This can happen if someone already optionally loaded cli.guess
        // and it has failed to configure.
        //
        if (!opt)
          fail (loc) << "cli could not be configured" <<
            info << "re-run with -V for more information";

        return false;
      }

      // Configuration.
      //
      using config::append_config;

      // config.cli.options
      //
      // Note that we merge it into the corresponding cli.* variable.
      //
      append_config<strings> (rs, rs, "cli.options", nullptr);

      return true;
    }

    bool
    init (scope& rs,
          scope& bs,
          const location& loc,
          bool,
          bool opt,
          module_init_extra& extra)
    {
      tracer trace ("cli::init");
      l5 ([&]{trace << "for " << rs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << "cli module must be loaded in project root";

      // Make sure the cxx module has been loaded since we need its targets
      // types (?xx{}). Note that we don't try to load it ourselves because of
      // the non-trivial variable merging semantics. So it is better to let
      // the user load cxx explicitly. @@ Not sure the reason still holds
      // though it might still make sense to expect the user to load cxx.
      //
      if (!cast_false<bool> (rs["cxx.loaded"]))
        fail (loc) << "cxx module must be loaded before cli";

      // Load cli.config and get its module instance.
      //
      if (optional<shared_ptr<build2::module>> r = load_module (
            rs, rs, "cli.config", loc, opt, extra.hints))
      {
        extra.module = *r;
      }
      else
      {
        // This can happen if someone already optionally loaded cli.config
        // and it has failed to configure.
        //
        if (!opt)
          fail (loc) << "cli could not be configured" <<
            info << "re-run with -V for more information";

        return false;
      }

      auto& m (extra.module_as<module> ());

      // Register target types.
      //
      rs.insert_target_type<cli> ();
      rs.insert_target_type<cli_cxx> ();

      // Register our rules.
      //
      // Other rules (e.g., cc::compile) may need to have the group members
      // resolved/linked up. Looks like a general pattern: groups should
      // resolve on *(update).
      {
        auto reg = [&rs, &m] (meta_operation_id mid, operation_id oid)
        {
          rs.insert_rule<cli_cxx>  (mid, oid, "cli.compile", m);
          rs.insert_rule<cxx::hxx> (mid, oid, "cli.compile", m);
          rs.insert_rule<cxx::cxx> (mid, oid, "cli.compile", m);
          rs.insert_rule<cxx::ixx> (mid, oid, "cli.compile", m);
        };

        reg (0 /* wildcard */, update_id);
        reg (perform_id,       clean_id);
      }

      return true;
    }

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"cli.guess",  nullptr, guess_init},
      {"cli.config", nullptr, config_init},
      {"cli",        nullptr, init},
      {nullptr,      nullptr, nullptr}
    };

    const module_functions*
    build2_cli_load ()
    {
      return mod_functions;
    }
  }
}
