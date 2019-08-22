// file      : libbuild2/config/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/config/operation.hxx>

#include <set>

#include <libbuild2/file.hxx>
#include <libbuild2/spec.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/module.hxx>
#include <libbuild2/config/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace config
  {
    // configure
    //
    static void
    save_src_root (const scope& rs)
    {
      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      path f (out_root / rs.root_extra->src_root_file);

      if (verb >= 2)
        text << "cat >" << f;

      try
      {
        ofdstream ofs (f);

        ofs << "# Created automatically by the config module." << endl
            << "#" << endl
            << "src_root = ";
        to_stream (ofs, name (src_root), true, '@'); // Quote.
        ofs << endl;

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write " << f << ": " << e;
      }
    }

    static void
    save_out_root (const scope& rs)
    {
      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      path f (src_root / rs.root_extra->out_root_file);

      if (verb)
        text << (verb >= 2 ? "cat >" : "save ") << f;

      try
      {
        ofdstream ofs (f);

        ofs << "# Created automatically by the config module." << endl
            << "#" << endl
            << "out_root = ";
        to_stream (ofs, name (out_root), true, '@'); // Quote.
        ofs << endl;

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write " << f << ": " << e;
      }
    }

    using project_set = set<const scope*>; // Use pointers to get comparison.

    static void
    save_config (const scope& rs, const project_set& projects)
    {
      path f (config_file (rs));

      if (verb)
        text << (verb >= 2 ? "cat >" : "save ") << f;

      const module& mod (*rs.lookup_module<const module> (module::name));

      try
      {
        ofdstream ofs (f);

        ofs << "# Created automatically by the config module, but feel " <<
          "free to edit." << endl
            << "#" << endl;

        ofs << "config.version = " << module::version << endl;

        if (auto l = rs.vars[var_amalgamation])
        {
          const dir_path& d (cast<dir_path> (l));

          ofs << endl
              << "# Base configuration inherited from " << d << endl
              << "#" << endl;
        }

        // Save config variables.
        //
        names storage;

        for (auto p: mod.saved_modules.order)
        {
          const string& sname (p.second->first);
          const saved_variables& svars (p.second->second);

          bool first (true); // Separate modules with a blank line.
          for (const saved_variable& sv: svars)
          {
            const variable& var (sv.var);

            pair<lookup, size_t> org (rs.find_original (var));
            pair<lookup, size_t> ovr (var.overrides == nullptr
                                      ? org
                                      : rs.find_override (var, org));
            const lookup& l (ovr.first);

            // We definitely write values that are set on our root scope or
            // are global overrides. Anything in-between is presumably
            // inherited. We might also not have any value at all (see
            // unconfigured()).
            //
            if (!l.defined ())
              continue;

            if (!(l.belongs (rs) || l.belongs (rs.ctx.global_scope)))
            {
              // This is presumably an inherited value. But it could also be
              // some left-over garbage. For example, an amalgamation could
              // have used a module but then dropped it while its config
              // values are still lingering in config.build. They are probably
              // still valid and we should probably continue using them but we
              // definitely want to move them to our config.build since they
              // will be dropped from the amalgamation's config.build. Let's
              // also warn the user just in case.
              //
              // There is also another case that falls under this now that
              // overrides are by default amalgamation-wide rather than just
              // "project and subprojects": we may be (re-)configuring a
              // subproject but the override is now set on the outer project's
              // root.
              //
              bool found (false);
              const scope* r (&rs);
              while ((r = r->parent_scope ()->root_scope ()) != nullptr)
              {
                if (l.belongs (*r))
                {
                  // Find the config module.
                  //
                  if (auto* m = r->lookup_module<const module> (module::name))
                  {
                    // Find the corresponding saved module.
                    //
                    auto i (m->saved_modules.find (sname));

                    if (i != m->saved_modules.end ())
                    {
                      // Find the variable.
                      //
                      const saved_variables& sv (i->second);
                      found = sv.find (var) != sv.end ();

                      // Handle that other case: if this is an override but
                      // the outer project itself is not being configured,
                      // then we need to save this override.
                      //
                      // One problem with using the already configured project
                      // set is that the outer project may be configured only
                      // after us in which case both projects will save the
                      // value. But perhaps this is a feature, not a bug since
                      // this is how project-local (%) override behaves.
                      //
                      if (found &&
                          org.first != ovr.first &&
                          projects.find (r) == projects.end ())
                        found = false;
                    }
                  }

                  break;
                }
              }

              if (found) // Inherited.
                continue;

              location loc (&f);

              // If this value is not defined in a project's root scope, then
              // something is broken.
              //
              if (r == nullptr)
                fail (loc) << "inherited variable " << var << " value "
                           << "is not from a root scope";

              // If none of the outer project's configurations use this value,
              // then we warn and save as our own. One special case where we
              // don't want to warn the user is if the variable is overriden.
              //
              if (org.first == ovr.first)
              {
                diag_record dr;
                dr << warn (loc) << "saving previously inherited variable "
                   << var;

                dr << info (loc) << "because project " << *r
                   << " no longer uses it in its configuration";

                if (verb >= 2)
                {
                  dr << info (loc) << "variable value: ";

                  if (*l)
                  {
                    storage.clear ();
                    dr << "'" << reverse (*l, storage) << "'";
                  }
                  else
                    dr << "[null]";
                }
              }
            }

            const string& n (var.name);
            const value& v (*l);

            // We will only write config.*.configured if it is false (true is
            // implied by its absence). We will also ignore false values if
            // there is any other value for this module (see unconfigured()).
            //
            if (n.size () > 11 &&
                n.compare (n.size () - 11, 11, ".configured") == 0)
            {
              if (cast<bool> (v) || svars.size () != 1)
                continue;
            }

            // If we got here then we are saving this variable. Handle the
            // blank line.
            //
            if (first)
            {
              ofs << endl;
              first = false;
            }

            // Handle the save_commented flag.
            //
            if ((org.first.defined () && org.first->extra) && // Default value.
                org.first == ovr.first &&                     // Not overriden.
                (sv.flags & save_commented) == save_commented)
            {
              ofs << '#' << n << " =" << endl;
              continue;
            }

            if (v)
            {
              storage.clear ();
              names_view ns (reverse (v, storage));

              ofs << n;

              if (ns.empty ())
                ofs << " =";
              else
              {
                ofs << " = ";
                to_stream (ofs, ns, true, '@'); // Quote.
              }

              ofs << endl;
            }
            else
              ofs << n << " = [null]" << endl;
          }
        }

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write " << f << ": " << e;
      }
    }

    static void
    configure_project (action a, const scope& rs, project_set& projects)
    {
      tracer trace ("configure_project");

      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      if (!projects.insert (&rs).second)
      {
        l5 ([&]{trace << "skipping already configured " << out_root;});
        return;
      }

      // Make sure the directories exist.
      //
      if (out_root != src_root)
      {
        mkdir_p (out_root / rs.root_extra->build_dir);
        mkdir (out_root / rs.root_extra->bootstrap_dir, 2);
      }

      // We distinguish between a complete configure and operation-
      // specific.
      //
      if (a.operation () == default_id)
      {
        l5 ([&]{trace << "completely configuring " << out_root;});

        // Save src-root.build unless out_root is the same as src.
        //
        if (out_root != src_root)
          save_src_root (rs);

        // Save config.build.
        //
        save_config (rs, projects);
      }
      else
      {
      }

      // Configure subprojects that have been loaded.
      //
      if (auto l = rs.vars[var_subprojects])
      {
        for (auto p: cast<subprojects> (l))
        {
          const dir_path& pd (p.second);
          dir_path out_nroot (out_root / pd);
          const scope& nrs (rs.ctx.scopes.find (out_nroot));

          // @@ Strictly speaking we need to check whether the config
          // module was loaded for this subproject.
          //
          if (nrs.out_path () != out_nroot) // This subproject not loaded.
            continue;

          configure_project (a, nrs, projects);
        }
      }
    }

    static void
    configure_forward (const scope& rs, project_set& projects)
    {
      tracer trace ("configure_forward");

      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      if (!projects.insert (&rs).second)
      {
        l5 ([&]{trace << "skipping already configured " << src_root;});
        return;
      }

      mkdir (src_root / rs.root_extra->bootstrap_dir, 2); // Make sure exists.
      save_out_root (rs);

      // Configure subprojects. Since we don't load buildfiles if configuring
      // a forward, we do it for all known subprojects.
      //
      if (auto l = rs.vars[var_subprojects])
      {
        for (auto p: cast<subprojects> (l))
        {
          dir_path out_nroot (out_root / p.second);
          const scope& nrs (rs.ctx.scopes.find (out_nroot));
          assert (nrs.out_path () == out_nroot);

          configure_forward (nrs, projects);
        }
      }
    }

    operation_id (*pre) (const values&, meta_operation_id, const location&);

    static operation_id
    configure_operation_pre (const values&, operation_id o)
    {
      // Don't translate default to update. In our case unspecified
      // means configure everything.
      //
      return o;
    }

    // The (vague) idea is that in the future we may turn this into to some
    // sort of key-value sequence (similar to the config initializer idea),
    // for example:
    //
    // configure(out/@src/, forward foo bar@123)
    //
    // Though using commas instead spaces and '=' instead of '@' would have
    // been nicer.
    //
    static bool
    forward (const values& params,
             const char* mo = nullptr,
             const location& l = location ())
    {
      if (params.size () == 1)
      {
        const names& ns (cast<names> (params[0]));

        if (ns.size () == 1 && ns[0].simple () && ns[0].value == "forward")
          return true;
        else if (!ns.empty ())
          fail (l) << "unexpected parameter '" << ns << "' for "
                   << "meta-operation " << mo;
      }
      else if (!params.empty ())
        fail (l) << "unexpected parameters for meta-operation " << mo;

      return false;
    }

    static void
    configure_pre (const values& params, const location& l)
    {
      forward (params, "configure", l); // Validate.
    }

    static void
    configure_load (const values& params,
                    scope& rs,
                    const path& buildfile,
                    const dir_path& out_base,
                    const dir_path& src_base,
                    const location& l)
    {
      if (forward (params))
      {
        // We don't need to load the buildfiles in order to configure
        // forwarding but in order to configure subprojects we have to
        // bootstrap them (similar to disfigure).
        //
        create_bootstrap_inner (rs);

        if (rs.out_path () == rs.src_path ())
          fail (l) << "forwarding to source directory " << rs.src_path ();
      }
      else
        load (params, rs, buildfile, out_base, src_base, l); // Normal load.
    }

    static void
    configure_search (const values& params,
                      const scope& rs,
                      const scope& bs,
                      const path& bf,
                      const target_key& tk,
                      const location& l,
                      action_targets& ts)
    {
      if (forward (params))
      {
        // For forwarding we only collect the projects (again, similar to
        // disfigure).
        //
        ts.push_back (&rs);
      }
      else
        search (params, rs, bs, bf, tk, l, ts); // Normal search.
    }

    static void
    configure_match (const values&, action, action_targets&, uint16_t, bool)
    {
      // Don't match anything -- see execute ().
    }

    static void
    configure_execute (const values& params,
                       action a,
                       action_targets& ts,
                       uint16_t,
                       bool)
    {
      bool fwd (forward (params));

      project_set projects;

      for (const action_target& at: ts)
      {
        if (fwd)
        {
          // Forward configuration.
          //
          const scope& rs (*static_cast<const scope*> (at.target));
          configure_forward (rs, projects);
          continue;
        }

        // Normal configuration.
        //
        // Match rules to configure every operation supported by each project.
        // Note that we are not calling operation_pre/post() callbacks here
        // since the meta operation is configure and we know what we are
        // doing.
        //
        // Note that we cannot do this in parallel. We cannot parallelize the
        // outer loop because we should match for a single action at a time.
        // And we cannot swap the loops because the list of operations is
        // target-specific. However, inside match(), things can proceed in
        // parallel.
        //
        const target& t (at.as_target ());
        const scope* rs (t.base_scope ().root_scope ());

        if (rs == nullptr)
          fail << "out of project target " << t;

        const operations& ops (rs->root_extra->operations);

        for (operation_id id (default_id + 1); // Skip default_id.
             id < ops.size ();
             ++id)
        {
          if (const operation_info* oif = ops[id])
          {
            // Skip aliases (e.g., update-for-install).
            //
            if (oif->id != id)
              continue;

            rs->ctx.current_oif (*oif);

            phase_lock pl (run_phase::match);
            match (action (configure_id, id), t);
          }
        }

        configure_project (a, *rs, projects);
      }
    }

    const meta_operation_info mo_configure {
      configure_id,
      "configure",
      "configure",
      "configuring",
      "configured",
      "is configured",
      true,           // bootstrap_outer
      &configure_pre, // meta-operation pre
      &configure_operation_pre,
      &configure_load,   // normal load unless configuring forward
      &configure_search, // normal search unless configuring forward
      &configure_match,
      &configure_execute,
      nullptr, // operation post
      nullptr, // meta-operation post
      nullptr  // include
    };

    // disfigure
    //

    static bool
    disfigure_project (action a, const scope& rs, project_set& projects)
    {
      tracer trace ("disfigure_project");

      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      if (!projects.insert (&rs).second)
      {
        l5 ([&]{trace << "skipping already disfigured " << out_root;});
        return false;
      }

      bool r (false); // Keep track of whether we actually did anything.

      // Disfigure subprojects. Since we don't load buildfiles during
      // disfigure, we do it for all known subprojects.
      //
      if (auto l = rs.vars[var_subprojects])
      {
        for (auto p: cast<subprojects> (l))
        {
          const dir_path& pd (p.second);
          dir_path out_nroot (out_root / pd);
          const scope& nrs (rs.ctx.scopes.find (out_nroot));
          assert (nrs.out_path () == out_nroot); // See disfigure_load().

          r = disfigure_project (a, nrs, projects) || r;

          // We use mkdir_p() to create the out_root of a subproject
          // which means there could be empty parent directories left
          // behind. Clean them up.
          //
          if (!pd.simple () && out_root != src_root)
          {
            for (dir_path d (pd.directory ());
                 !d.empty ();
                 d = d.directory ())
            {
              rmdir_status s (rmdir (out_root / d, 2));

              if (s == rmdir_status::not_empty)
                break; // No use trying do remove parent ones.

              r = (s == rmdir_status::success) || r;
            }
          }
        }
      }

      // We distinguish between a complete disfigure and operation-
      // specific.
      //
      if (a.operation () == default_id)
      {
        l5 ([&]{trace << "completely disfiguring " << out_root;});

        r = rmfile (config_file (rs)) || r;

        if (out_root != src_root)
        {
          r = rmfile (out_root / rs.root_extra->src_root_file, 2) || r;

          // Clean up the directories.
          //
          // Note: try to remove the root/ hooks directory if it is empty.
          //
          r = rmdir (out_root / rs.root_extra->root_dir,      2) || r;
          r = rmdir (out_root / rs.root_extra->bootstrap_dir, 2) || r;
          r = rmdir (out_root / rs.root_extra->build_dir,     2) || r;

          switch (rmdir (out_root))
          {
          case rmdir_status::not_empty:
            {
              // We used to issue a warning but it is actually a valid usecase
              // to leave the build output around in case, for example, of a
              // reconfigure.
              //
              if (verb)
                info << "directory " << out_root << " is "
                     << (out_root == work
                         ? "current working directory"
                         : "not empty") << ", not removing";
              break;
            }
          case rmdir_status::success:
            r = true;
          default:
            break;
          }
        }
      }
      else
      {
      }

      return r;
    }

    static bool
    disfigure_forward (const scope& rs, project_set& projects)
    {
      // Pretty similar logic to disfigure_project().
      //
      tracer trace ("disfigure_forward");

      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      if (!projects.insert (&rs).second)
      {
        l5 ([&]{trace << "skipping already disfigured " << src_root;});
        return false;
      }

      bool r (false);

      if (auto l = rs.vars[var_subprojects])
      {
        for (auto p: cast<subprojects> (l))
        {
          dir_path out_nroot (out_root / p.second);
          const scope& nrs (rs.ctx.scopes.find (out_nroot));
          assert (nrs.out_path () == out_nroot);

          r = disfigure_forward (nrs, projects) || r;
        }
      }

      // Remove the out-root.build file and try to remove the bootstrap/
      // directory if it is empty.
      //
      r = rmfile (src_root / rs.root_extra->out_root_file)    || r;
      r = rmdir  (src_root / rs.root_extra->bootstrap_dir, 2) || r;

      return r;
    }

    static void
    disfigure_pre (const values& params, const location& l)
    {
      forward (params, "disfigure", l); // Validate.
    }

    static operation_id
    disfigure_operation_pre (const values&, operation_id o)
    {
      // Don't translate default to update. In our case unspecified
      // means disfigure everything.
      //
      return o;
    }

    static void
    disfigure_load (const values&,
                    scope& root,
                    const path&,
                    const dir_path&,
                    const dir_path&,
                    const location&)
    {
      // Since we don't load buildfiles during disfigure but still want to
      // disfigure all the subprojects (see disfigure_project() below), we
      // bootstrap all the known subprojects.
      //
      create_bootstrap_inner (root);
    }

    static void
    disfigure_search (const values&,
                      const scope& rs,
                      const scope&,
                      const path&,
                      const target_key&,
                      const location&,
                      action_targets& ts)
    {
      ts.push_back (&rs);
    }

    static void
    disfigure_match (const values&, action, action_targets&, uint16_t, bool)
    {
    }

    static void
    disfigure_execute (const values& params,
                       action a,
                       action_targets& ts,
                       uint16_t diag,
                       bool)
    {
      tracer trace ("disfigure_execute");

      bool fwd (forward (params));

      project_set projects;

      // Note: doing everything in the load phase (disfigure_project () does
      // modify the build state).
      //
      for (const action_target& at: ts)
      {
        const scope& rs (*static_cast<const scope*> (at.target));

        if (!(fwd
              ? disfigure_forward (   rs, projects)
              : disfigure_project (a, rs, projects)))
        {
          // Create a dir{$out_root/} target to signify the project's root in
          // diagnostics. Not very clean but seems harmless.
          //
          target& t (
            rs.ctx.targets.insert (dir::static_type,
                                   fwd ? rs.src_path () : rs.out_path (),
                                   dir_path (), // Out tree.
                                   "",
                                   nullopt,
                                   true,       // Implied.
                                   trace).first);

          if (verb != 0 && diag >= 2)
            info << diag_done (a, t);
        }
      }
    }

    const meta_operation_info mo_disfigure {
      disfigure_id,
      "disfigure",
      "disfigure",
      "disfiguring",
      "disfigured",
      "is disfigured",
      false,         // bootstrap_outer
      disfigure_pre, // meta-operation pre
      &disfigure_operation_pre,
      &disfigure_load,
      &disfigure_search,
      &disfigure_match,
      &disfigure_execute,
      nullptr, // operation post
      nullptr, // meta-operation post
      nullptr  // include
    };

    // create
    //
    static void
    save_config (context& ctx, const dir_path& d)
    {
      // Since there aren't any sub-projects yet, any config.import.* values
      // that the user may want to specify won't be saved in config.build. So
      // let's go ahead and mark them all to be saved. To do this, however, we
      // need the config module (which is where this information is stored).
      // And the module is created by init() during bootstrap. So what we are
      // going to do is bootstrap the newly created project, similar to the
      // way main() does it.
      //
      scope& gs (ctx.global_scope.rw ());
      scope& rs (load_project (gs, d, d, false /* fwd */, false /* load */));
      module& m (*rs.lookup_module<module> (module::name));

      // Save all the global config.import.* variables.
      //
      variable_pool& vp (ctx.var_pool.rw (rs));
      for (auto p (gs.vars.find_namespace (vp.insert ("config.import")));
           p.first != p.second;
           ++p.first)
      {
        const variable& var (p.first->first);

        // Annoyingly, this can be (always is?) one of the overrides
        // (__override, __prefix, etc).
        //
        size_t n (var.override ());
        m.save_variable (n != 0 ? *vp.find (string (var.name, 0, n)) : var);
      }

      // Now project-specific. For now we just save all of them and let
      // save_config() above weed out the ones that don't apply.
      //
      for (const variable_override& vo: ctx.var_overrides)
      {
        const variable& var (vo.var);

        if (var.name.compare (0, 14, "config.import.") == 0)
          m.save_variable (var);
      }
    }

    const string&
    preprocess_create (context& ctx,
                       values& params,
                       vector_view<opspec>& spec,
                       bool lifted,
                       const location& l)
    {
      tracer trace ("preprocess_create");

      // The overall plan is to create the project(s), update the buildspec,
      // clear the parameters, and then continue as if we were the configure
      // meta-operation.

      // Start with process parameters. The first parameter, if any, is a list
      // of root.build modules. The second parameter, if any, is a list of
      // bootstrap.build modules. If the second is not specified, then the
      // default is test, dist, and install (config is mandatory).
      //
      strings bmod {"test", "dist", "install"};
      strings rmod;
      try
      {
        size_t n (params.size ());

        if (n > 0)
          rmod = convert<strings> (move (params[0]));

        if (n > 1)
          bmod = convert<strings> (move (params[1]));

        if (n > 2)
          fail (l) << "unexpected parameters for meta-operation create";
      }
      catch (const invalid_argument& e)
      {
        fail (l) << "invalid module name: " << e.what ();
      }

      current_oname = empty_string; // Make sure valid.

      // Now handle each target in each operation spec.
      //
      for (const opspec& os: spec)
      {
        // First do some sanity checks: there should be no explicit operation
        // and our targets should all be directories.
        //
        if (!lifted && !os.name.empty ())
          fail (l) << "explicit operation specified for meta-operation create";

        for (const targetspec& ts: os)
        {
          const name& tn (ts.name);

          // Figure out the project directory. This logic must be consistent
          // with find_target_type() and other places (grep for "..").
          //
          dir_path d;

          if (tn.simple () &&
              (tn.empty () || tn.value == "." || tn.value == ".."))
            d = dir_path (tn.value);
          else if (tn.directory ())
            d = tn.dir;
          else if (tn.typed () && tn.type == "dir")
            d = tn.dir / dir_path (tn.value);
          else
            fail(l) << "non-directory target '" << ts << "' in "
                    << "meta-operation create";

          if (d.relative ())
            d = work / d;

          d.normalize (true);

          // If src_base was explicitly specified, make sure it is the same as
          // the project directory.
          //
          if (!ts.src_base.empty ())
          {
            dir_path s (ts.src_base);

            if (s.relative ())
              s = work / s;

            s.normalize (true);

            if (s != d)
              fail(l) << "different src/out directories for target '" << ts
                      << "' in meta-operation create";
          }

          l5 ([&]{trace << "creating project in " << d;});

          // For now we disable amalgamating this project. Sooner or later
          // someone will probably want to do this, though (i.e., nested
          // configurations).
          //
          create_project (d,
                          dir_path (), /* amalgamation */
                          bmod,
                          "",          /* root_pre */
                          rmod,
                          "",          /* root_post */
                          true,        /* config */
                          true,        /* buildfile */
                          "the create meta-operation");

          save_config (ctx, d);
        }
      }

      params.clear ();
      return mo_configure.name;
    }
  }
}
