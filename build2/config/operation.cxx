// file      : build2/config/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/config/operation>

#include <set>

#include <build2/file>
#include <build2/scope>
#include <build2/target>
#include <build2/context>
#include <build2/algorithm>
#include <build2/filesystem>
#include <build2/diagnostics>

#include <build2/config/module>
#include <build2/config/utility>

using namespace std;
using namespace butl;

namespace build2
{
  namespace config
  {
    static const path config_file ("build/config.build");

    // configure
    //
    static operation_id
    configure_operation_pre (operation_id o)
    {
      // Don't translate default to update. In our case unspecified
      // means configure everything.
      //
      return o;
    }

    static void
    save_src_root (const dir_path& out_root, const dir_path& src_root)
    {
      path f (out_root / src_root_file);

      if (verb)
        text << (verb >= 2 ? "cat >" : "save ") << f;

      try
      {
        ofdstream ofs (f);

        ofs << "# Created automatically by the config module." << endl
            << "#" << endl
            << "src_root = ";
        to_stream (ofs, name (src_root, false), true, '@'); // Quote.
        ofs << endl;

        ofs.close ();
      }
      catch (const ofdstream::failure& e)
      {
        fail << "unable to write " << f << ": " << e.what ();
      }
    }

    static void
    save_config (scope& root, const set<scope*>& projects)
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

        if (auto l = root.vars["amalgamation"])
        {
          const dir_path& d (cast<dir_path> (l));

          ofs << endl
              << "# Base configuration inherited from " << d << endl
              << "#" << endl;
        }

        // Save config variables.
        //
        names storage;

        for (const saved_modules::const_iterator& i:
               mod.saved_modules.sequence)
        {
          const string& sname (i->first);
          const saved_variables& svars (i->second);

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
              scope* r (&root);
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
                fail (loc) << "inherited variable " << var.name << " value "
                           << "is not from a root scope";

              // If none of the outer project's configurations use this value,
              // then we warn and save as our own. One special case where we
              // don't want to warn the user is if the variable is overriden.
              //
              if (org.first == ovr.first)
              {
                diag_record dr;
                dr << warn (loc) << "saving previously inherited variable "
                   << var.name;

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

              ofs << n << " = ";
              to_stream (ofs, reverse (v, storage), true, '@'); // Quote.
              ofs << endl;
            }
            else
              ofs << n << " = [null]" << endl;
          }
        }

        ofs.close ();
      }
      catch (const ofdstream::failure& e)
      {
        fail << "unable to write " << f << ": " << e.what ();
      }
    }

    static void
    configure_project (action a, scope& root, set<scope*>& projects)
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
        mkdir_p (out_root);
        mkdir (out_root / build_dir);
        mkdir (out_root / bootstrap_dir);
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
      if (auto l = root.vars["subprojects"])
      {
        for (auto p: cast<subprojects> (l))
        {
          const dir_path& pd (p.second);
          dir_path out_nroot (out_root / pd);
          scope& nroot (scopes.find (out_nroot));

          // @@ Strictly speaking we need to check whether the config
          // module was loaded for this subproject.
          //
          if (nroot.out_path () != out_nroot) // This subproject not loaded.
            continue;

          configure_project (a, nroot, projects);
        }
      }
    }

    static void
    configure_match (action, action_targets&)
    {
      // Don't match anything -- see execute ().
    }

    static void
    configure_execute (action a, const action_targets& ts, bool)
    {
      // Match rules to configure every operation supported by each
      // project. Note that we are not calling operation_pre/post()
      // callbacks here since the meta operation is configure and we
      // know what we are doing.
      //
      set<scope*> projects;

      for (void* v: ts)
      {
        target& t (*static_cast<target*> (v));
        scope* rs (t.base_scope ().root_scope ());

        if (rs == nullptr)
          fail << "out of project target " << t;

        for (operations::size_type id (default_id + 1); // Skip default_id
             id < rs->operations.size ();
             ++id)
        {
          const operation_info* oi (rs->operations[id]);
          if (oi == nullptr)
            continue;

          current_inner_oif = oi;
          current_outer_oif = nullptr;
          current_mode = oi->mode;
          dependency_count = 0;

          match (action (configure_id, id), t);
        }

        configure_project (a, *rs, projects);
      }
    }

    meta_operation_info configure {
      configure_id,
      "configure",
      "configure",
      "configuring",
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
    static operation_id
    disfigure_operation_pre (operation_id o)
    {
      // Don't translate default to update. In our case unspecified
      // means disfigure everything.
      //
      return o;
    }

    static void
    disfigure_load (const path& bf,
                    scope&,
                    const dir_path&,
                    const dir_path&,
                    const location&)
    {
      tracer trace ("disfigure_load");
      l6 ([&]{trace << "skipping " << bf;});
    }

    static void
    disfigure_search (scope& root,
                      const target_key&,
                      const location&,
                      action_targets& ts)
    {
      tracer trace ("disfigure_search");
      l6 ([&]{trace << "collecting " << root.out_path ();});
      ts.push_back (&root);
    }

    static void
    disfigure_match (action, action_targets&) {}

    static bool
    disfigure_project (action a, scope& root, set<scope*>& projects)
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
      if (auto l = root.vars["subprojects"])
      {
        for (auto p: cast<subprojects> (l))
        {
          const dir_path& pd (p.second);

          // Create and bootstrap subproject's root scope.
          //
          dir_path out_nroot (out_root / pd);

          // The same logic for src_root as in create_bootstrap_inner().
          //
          scope& nroot (create_root (out_nroot, dir_path ()));

          if (!bootstrapped (nroot))
          {
            bootstrap_out (nroot);

            value& val (nroot.assign ("src_root"));

            if (!val)
              val = is_src_root (out_nroot) ? out_nroot : (src_root / pd);

            setup_root (nroot);
            bootstrap_src (nroot);
          }

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
              rmdir_status s (rmdir (out_root / d));

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
          m = rmfile (out_root / src_root_file) || m;

          // Clean up the directories.
          //
          m = rmdir (out_root / bootstrap_dir) || m;
          m = rmdir (out_root / build_dir) || m;

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
    disfigure_execute (action a, const action_targets& ts, bool quiet)
    {
      tracer trace ("disfigure_execute");

      set<scope*> projects;

      for (void* v: ts)
      {
        scope& root (*static_cast<scope*> (v));

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
                            nullptr,
                            trace).first);

          if (!quiet)
            info << diag_done (a, t);
        }
      }
    }

    meta_operation_info disfigure {
      disfigure_id,
      "disfigure",
      "disfigure",
      "disfiguring",
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
  }
}
