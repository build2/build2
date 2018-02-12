// file      : build2/config/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/config/operation.hxx>

#include <set>

#include <build2/file.hxx>
#include <build2/spec.hxx>
#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/context.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/config/module.hxx>
#include <build2/config/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace config
  {
    // configure
    //
    static void
    save_src_root (const dir_path& out_root, const dir_path& src_root)
    {
      path f (out_root / src_root_file);

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

    using project_set = set<const scope*>; // Use pointers to get comparison.

    static void
    save_config (const scope& root, const project_set& projects)
    {
      const dir_path& out_root (root.out_path ());
      path f (out_root / config_file);

      if (verb)
        text << (verb >= 2 ? "cat >" : "save ") << f;

      const module& mod (*root.modules.lookup<const module> (module::name));

      try
      {
        ofdstream ofs (f);

        ofs << "# Created automatically by the config module, but feel " <<
          "free to edit." << endl
            << "#" << endl;

        ofs << "config.version = " << module::version << endl;

        if (auto l = root.vars[var_amalgamation])
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

            pair<lookup, size_t> org (root.find_original (var));
            pair<lookup, size_t> ovr (var.override == nullptr
                                      ? org
                                      : root.find_override (var, org));
            const lookup& l (ovr.first);

            // We definitely write values that are set on our root scope or
            // are global overrides. Anything in-between is presumably
            // inherited. We might also not have any value at all (see
            // unconfigured()).
            //
            if (!l.defined ())
              continue;

            if (!(l.belongs (root) || l.belongs (*global_scope)))
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
              const scope* r (&root);
              while ((r = r->parent_scope ()->root_scope ()) != nullptr)
              {
                if (l.belongs (*r))
                {
                  // Find the config module.
                  //
                  if (auto* m = r->modules.lookup<const module> (module::name))
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

                dr << info (loc) << "because project " << r->out_path ()
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
    configure_project (action a, const scope& root, project_set& projects)
    {
      tracer trace ("configure_project");

      const dir_path& out_root (root.out_path ());
      const dir_path& src_root (root.src_path ());

      if (!projects.insert (&root).second)
      {
        l5 ([&]{trace << "skipping already configured " << out_root;});
        return;
      }

      // Make sure the directories exist.
      //
      if (out_root != src_root)
      {
        mkdir_p (out_root / build_dir);
        mkdir (out_root / bootstrap_dir, 2);
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
          save_src_root (out_root, src_root);

        // Save config.build.
        //
        save_config (root, projects);
      }
      else
      {
      }

      // Configure subprojects that have been loaded.
      //
      if (auto l = root.vars[var_subprojects])
      {
        for (auto p: cast<subprojects> (l))
        {
          const dir_path& pd (p.second);
          dir_path out_nroot (out_root / pd);
          const scope& nroot (scopes.find (out_nroot));

          // @@ Strictly speaking we need to check whether the config
          // module was loaded for this subproject.
          //
          if (nroot.out_path () != out_nroot) // This subproject not loaded.
            continue;

          configure_project (a, nroot, projects);
        }
      }
    }

    static operation_id
    configure_operation_pre (const values&, operation_id o)
    {
      // Don't translate default to update. In our case unspecified
      // means configure everything.
      //
      return o;
    }

    static void
    configure_match (const values&, action, action_targets&, uint16_t)
    {
      // Don't match anything -- see execute ().
    }

    static void
    configure_execute (const values&, action a, action_targets& ts, uint16_t)
    {
      // Match rules to configure every operation supported by each
      // project. Note that we are not calling operation_pre/post()
      // callbacks here since the meta operation is configure and we
      // know what we are doing.
      //
      project_set projects;

      // Note that we cannot do this in parallel. We cannot parallelize the
      // outer loop because we should match for a single action at a time.
      // And we cannot swap the loops because the list of operations is
      // target-specific. However, inside match(), things can proceed in
      // parallel.
      //
      for (const action_target& at: ts)
      {
        const target& t (at.as_target ());
        const scope* rs (t.base_scope ().root_scope ());

        if (rs == nullptr)
          fail << "out of project target " << t;

        for (operation_id id (default_id + 1); // Skip default_id
             id < rs->operations.size ();
             ++id)
        {
          if (const operation_info* oif = rs->operations[id])
          {
            // Skip aliases (e.g., update-for-install).
            //
            if (oif->id != id)
              continue;

            set_current_oif (*oif);

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
      nullptr, // meta-operation pre
      &configure_operation_pre,
      &load,   // normal load
      &search, // normal search
      &configure_match,
      &configure_execute,
      nullptr, // operation post
      nullptr  // meta-operation post
    };

    // disfigure
    //
    static void
    bootstrap_project (scope& root)
    {
      if (auto l = root.vars[var_subprojects])
      {
        const dir_path& out_root (root.out_path ());
        const dir_path& src_root (root.src_path ());

        for (auto p: cast<subprojects> (l))
        {
          const dir_path& pd (p.second);

          // Create and bootstrap subproject's root scope.
          //
          dir_path out_nroot (out_root / pd);

          // The same logic for src_root as in create_bootstrap_inner().
          //
          scope& nroot (create_root (root, out_nroot, dir_path ())->second);

          if (!bootstrapped (nroot))
          {
            bootstrap_out (nroot);

            value& val (nroot.assign (var_src_root));

            if (!val)
              val = is_src_root (out_nroot) ? out_nroot : (src_root / pd);

            setup_root (nroot);
            bootstrap_src (nroot);
          }

          bootstrap_project (nroot);
        }
      }
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
                    const path& bf,
                    const dir_path&,
                    const dir_path&,
                    const location&)
    {
      tracer trace ("disfigure_load");
      l6 ([&]{trace << "skipping " << bf;});

      // Since we don't load buildfiles during disfigure but still want to
      // disfigure all the subprojects (see disfigure_project() below), we
      // bootstrap all known subprojects.
      //
      bootstrap_project (root);
    }

    static void
    disfigure_search (const values&,
                      const scope& root,
                      const scope&,
                      const target_key&,
                      const location&,
                      action_targets& ts)
    {
      tracer trace ("disfigure_search");
      l6 ([&]{trace << "collecting " << root.out_path ();});
      ts.push_back (&root);
    }

    static void
    disfigure_match (const values&, action, action_targets&, uint16_t)
    {
    }

    static bool
    disfigure_project (action a, const scope& root, project_set& projects)
    {
      tracer trace ("disfigure_project");

      const dir_path& out_root (root.out_path ());
      const dir_path& src_root (root.src_path ());

      if (!projects.insert (&root).second)
      {
        l5 ([&]{trace << "skipping already disfigured " << out_root;});
        return true;
      }

      bool m (false); // Keep track of whether we actually did anything.

      // Disfigure subprojects. Since we don't load buildfiles during
      // disfigure, we do it for all known subprojects.
      //
      if (auto l = root.vars[var_subprojects])
      {
        for (auto p: cast<subprojects> (l))
        {
          const dir_path& pd (p.second);
          dir_path out_nroot (out_root / pd);
          const scope& nroot (scopes.find (out_nroot));
          assert (nroot.out_path () == out_nroot); // See disfigure_load().

          m = disfigure_project (a, nroot, projects) || m;

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

              m = (s == rmdir_status::success) || m;
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

        m = rmfile (out_root / config_file) || m;

        if (out_root != src_root)
        {
          m = rmfile (out_root / src_root_file, 2) || m;

          // Clean up the directories.
          //
          m = rmdir (out_root / bootstrap_dir, 2) || m;
          m = rmdir (out_root / build_dir, 2) || m;

          switch (rmdir (out_root))
          {
          case rmdir_status::not_empty:
            {
              warn << "directory " << out_root << " is "
                   << (out_root == work
                       ? "current working directory"
                       : "not empty") << ", not removing";
              break;
            }
          case rmdir_status::success:
            m = true;
          default:
            break;
          }
        }
      }
      else
      {
      }

      return m;
    }

    static void
    disfigure_execute (const values&,
                       action a,
                       action_targets& ts,
                       uint16_t diag)
    {
      tracer trace ("disfigure_execute");

      project_set projects;

      // Note: doing everything in the load phase (disfigure_project () does
      // modify the model).
      //
      for (const action_target& at: ts)
      {
        const scope& root (*static_cast<const scope*> (at.target));

        if (!disfigure_project (a, root, projects))
        {
          // Create a dir{$out_root/} target to signify the project's
          // root in diagnostics. Not very clean but seems harmless.
          //
          target& t (
            targets.insert (dir::static_type,
                            root.out_path (),
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
      nullptr, // meta-operation pre
      &disfigure_operation_pre,
      &disfigure_load,
      &disfigure_search,
      &disfigure_match,
      &disfigure_execute,
      nullptr, // operation post
      nullptr, // meta-operation post
    };

    // create
    //
    static void
    save_config (const dir_path& d, const variable_overrides& var_ovs)
    {
      // Since there aren't any sub-projects yet, any config.import.* values
      // that the user may want to specify won't be saved in config.build. So
      // let's go ahead and mark them all to be saved. To do this, however, we
      // need the config module (which is where this information is stored).
      // And the module is created by init() during bootstrap. So what we are
      // going to do is bootstrap the newly created project, similar to the
      // way main() does it.
      //
      scope& gs (*scope::global_);
      scope& rs (load_project (gs, d, d, false /* load */));
      module& m (*rs.modules.lookup<module> (module::name));

      // Save all the global config.import.* variables.
      //
      variable_pool& vp (var_pool.rw (rs));
      for (auto p (gs.vars.find_namespace (vp.insert ("config.import")));
           p.first != p.second;
           ++p.first)
      {
        const variable& var (p.first->first);

        // Annoyingly, this is one of the __override/__prefix/__suffix
        // values. So we strip the last component.
        //
        size_t n (var.name.size ());

        if (var.name.compare (n - 11, 11, ".__override") == 0)
          n -= 11;
        else if (var.name.compare (n - 9, 9, ".__prefix") == 0)
          n -= 9;
        else if (var.name.compare (n - 9, 9, ".__suffix") == 0)
          n -= 9;

        m.save_variable (*vp.find (string (var.name, 0, n)));
      }

      // Now project-specific. For now we just save all of them and let
      // save_config() above weed out the ones that don't apply.
      //
      for (const variable_override& vo: var_ovs)
      {
        const variable& var (vo.var);

        if (var.name.compare (0, 14, "config.import.") == 0)
          m.save_variable (var);
      }
    }

    const string&
    preprocess_create (const variable_overrides& var_ovs,
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
      // default is test and install (config is mandatory).
      //
      strings bmod {"test", "install"};
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

          // Figure out the project directory. This code must be consistent
          // with find_target_type() and other places.
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

          save_config (d, var_ovs);
        }
      }

      params.clear ();
      return mo_configure.name;
    }
  }
}
