// file      : build/config/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/config/operation>

#include <fstream>

#include <butl/filesystem>

#include <build/file>
#include <build/scope>
#include <build/target>
#include <build/context>
#include <build/diagnostics>

using namespace std;
using namespace butl;

namespace build
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

      text << (verb ? "config::save_src_root " : "save ") << f;

      try
      {
        ofstream ofs (f.string ());
        if (!ofs.is_open ())
          fail << "unable to open " << f;

        ofs.exceptions (ofstream::failbit | ofstream::badbit);

        //@@ TODO: quote path
        //
        ofs << "# Created automatically by the config module." << endl
            << "#" << endl
            << "src_root = " << src_root << endl;
      }
      catch (const ios_base::failure&)
      {
        fail << "failed to write to " << f;
      }
    }

    static void
    save_config (scope& root)
    {
      const dir_path& out_root (root.path ());
      path f (out_root / config_file);

      text << (verb ? "config::save_config " : "save ") << f;

      try
      {
        ofstream ofs (f.string ());
        if (!ofs.is_open ())
          fail << "unable to open " << f;

        ofs.exceptions (ofstream::failbit | ofstream::badbit);

        ofs << "# Created automatically by the config module, but" << endl
            << "# feel free to edit." << endl
            << "#" << endl;

        if (auto v = root.vars["amalgamation"])
        {
          const dir_path& d (v.as<const dir_path&> ());

          ofs << "# Base configuration inherited from " << d << endl
              << "#" << endl;
        }

        // Save all the variables in the config namespace that are set
        // on the project's root scope.
        //
        for (auto p (root.vars.find_namespace ("config"));
             p.first != p.second;
             ++p.first)
        {
          const variable& var (p.first->first);
          const value_ptr& pval (p.first->second);

          // Warn the user if the value that we are saving differs
          // from the one they specified on the command line.
          //
          if (auto gval = (*global_scope)[var])
          {
            if (pval == nullptr || !pval->compare (gval.as<const value&> ()))
              warn << "variable " << var.name << " configured value "
                   << "differs from command line value" <<
                info << "reconfigure the project to use command line value";
          }

          if (pval != nullptr)
          {
            //@@ TODO: assuming list
            //
            const list_value& lv (dynamic_cast<const list_value&> (*pval));

            ofs << var.name << " = " << lv << endl;
            //text << var.name << " = " << lv;
          }
          else
          {
            ofs << var.name << " = #[null]" << endl; // @@ TODO: [null]
            //text << var.name << " = [null]";
          }
        }
      }
      catch (const ios_base::failure&)
      {
        fail << "failed to write to " << f;
      }
    }

    static void
    configure_project (action a, scope& root)
    {
      tracer trace ("configure_project");

      const dir_path& out_root (root.path ());
      const dir_path& src_root (root.src_path ());

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
        level4 ([&]{trace << "completely configuring " << out_root;});

        // Save src-root.build unless out_root is the same as src.
        //
        if (out_root != src_root)
          save_src_root (out_root, src_root);

        // Save config.build.
        //
        save_config (root);
      }
      else
      {
      }

      // Configure subprojects that have been loaded.
      //
      if (auto v = root.vars["subprojects"])
      {
        for (const name& n: v.as<const list_value&> ())
        {
          if (n.pair != '\0')
            continue; // Skip project names.

          dir_path out_nroot (out_root / n.dir);
          scope& nroot (scopes.find (out_nroot));

          // @@ Strictly speaking we need to check whether the config
          // module was loaded for this subproject.
          //
          if (nroot.path () != out_nroot) // This subproject was not loaded.
            continue;

          configure_project (a, nroot);
        }
      }
    }

    static void
    configure_execute (action a, const action_targets& ts)
    {
      for (void* v: ts)
      {
        target& t (*static_cast<target*> (v));
        scope* rs (t.base_scope ().root_scope ());

        if (rs == nullptr)
          fail << "out of project target " << t;

        configure_project (a, *rs);
      }
    }

    meta_operation_info configure {
      "configure",
      "configure",
      "configuring",
      "is configured",
      nullptr, // meta-operation pre
      &configure_operation_pre,
      &load,   // normal load
      &search, // normal search
      &match,  // normal match
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
      level5 ([&]{trace << "skipping " << bf;});
    }

    static void
    disfigure_search (scope& root,
                      const target_key&,
                      const location&,
                      action_targets& ts)
    {
      tracer trace ("disfigure_search");
      level5 ([&]{trace << "collecting " << root.path ();});
      ts.push_back (&root);
    }

    static void
    disfigure_match (action, action_targets&) {}

    static bool
    disfigure_project (action a, scope& root)
    {
      tracer trace ("disfigure_project");

      bool m (false); // Keep track of whether we actually did anything.

      const dir_path& out_root (root.path ());
      const dir_path& src_root (root.src_path ());

      // Disfigure subprojects. Since we don't load buildfiles during
      // disfigure, we do it for all known subprojects.
      //
      if (auto v = root.vars["subprojects"])
      {
        for (const name& n: v.as<const list_value&> ())
        {
          if (n.pair != '\0')
            continue; // Skip project names.

          // Create and bootstrap subproject's root scope.
          //
          dir_path out_nroot (out_root / n.dir);

          // The same logic for src_root as in create_bootstrap_inner().
          //
          scope& nroot (create_root (out_nroot, dir_path ()));
          bootstrap_out (nroot);

          auto val (nroot.assign ("src_root"));

          if (!val)
            val = is_src_root (out_nroot)
              ? out_nroot
              : (src_root / n.dir);

          nroot.src_path_ = &val.as<const dir_path&> ();

          bootstrap_src (nroot);

          m = disfigure_project (a, nroot) || m;

          // We use mkdir_p() to create the out_root of a subproject
          // which means there could be empty parent directories left
          // behind. Clean them up.
          //
          if (!n.dir.simple () && out_root != src_root)
          {
            for (dir_path d (n.dir.directory ());
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
        level4 ([&]{trace << "completely disfiguring " << out_root;});

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
    disfigure_execute (action a, const action_targets& ts)
    {
      tracer trace ("disfigure_execute");

      for (void* v: ts)
      {
        scope& root (*static_cast<scope*> (v));

        if (!disfigure_project (a, root))
        {
          // Create a dir{$out_root/} target to signify the project's
          // root in diagnostics. Not very clean but seems harmless.
          //
          target& t (
            targets.insert (
              dir::static_type, root.path (), "", nullptr, trace).first);

          info << diag_done (a, t);
        }
      }
    }

    static void
    disfigure_meta_operation_post ()
    {
      tracer trace ("disfigure_meta_operation_post");

      // Reset the dependency state since anything that could have been
      // loaded earlier using a previous configuration is now invalid.
      //
      level5 ([&]{trace << "resetting dependency state";});
      reset ();
    }

    meta_operation_info disfigure {
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
      &disfigure_meta_operation_post
    };
  }
}
