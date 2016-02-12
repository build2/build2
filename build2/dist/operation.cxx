// file      : build2/dist/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/dist/operation>

#include <butl/process>
#include <butl/filesystem>

#include <build2/file>
#include <build2/dump>
#include <build2/scope>
#include <build2/target>
#include <build2/context>
#include <build2/algorithm>
#include <build2/diagnostics>

using namespace std;
using namespace butl;

namespace build2
{
  namespace dist
  {
    static void
    dist_meta_operation_pre ()
    {
      // Reset the dependency state so that we don't end up with stray
      // files from previous batches.
      //
      // @@ This is called too late, after we have bootstrapped the
      //    project.
      //
      //reset ();
    }

    static operation_id
    dist_operation_pre (operation_id o)
    {
      if (o != default_id)
        fail << "explicit operation specified for dist meta-operation";

      return o;
    }

    static void
    dist_match (action, action_targets&)
    {
      // Don't match anything -- see execute ().
    }

    // install -d <dir>
    //
    static void
    install (const string& cmd, const dir_path&);

    // install <file> <dir>
    //
    static void
    install (const string& cmd, file&, const dir_path&);

    // cd <root> && tar|zip ... <pkg>.<ext> <pkg>
    //
    static void
    archive (const dir_path& root, const string& pkg, const string& ext);

    static void
    dist_execute (action, const action_targets& ts, bool)
    {
      tracer trace ("dist_execute");

      // For now we assume all the targets are from the same project.
      //
      target& t (*static_cast<target*> (ts[0]));
      scope* rs (t.base_scope ().root_scope ());

      if (rs == nullptr)
        fail << "out of project target " << t;

      const dir_path& out_root (rs->out_path ());
      const dir_path& src_root (rs->src_path ());

      if (out_root == src_root)
        fail << "in-tree distribution of target " << t <<
          info << "distribution requires out-of-tree build";

      // Make sure we have the necessary configuration before
      // we get down to business.
      //
      auto l (rs->vars["dist.root"]);

      if (!l || l->empty ())
        fail << "unknown root distribution directory" <<
          info << "did you forget to specify config.dist.root?";

      const dir_path& dist_root (as<dir_path> (*l));

      if (!dir_exists (dist_root))
        fail << "root distribution directory " << dist_root
             << " does not exist";

      l = rs->vars["dist.package"];

      if (!l || l->empty ())
        fail << "unknown distribution package name" <<
          info << "did you forget to set dist.package?";

      const string& dist_package (as<string> (*l));
      const string& dist_cmd (as<string> (*rs->vars["dist.cmd"]));

      // Get the list of operations supported by this project. Skip
      // default_id.
      //
      for (operations::size_type id (default_id + 1);
           id < rs->operations.size ();
           ++id)
      {
        const operation_info* oi (rs->operations[id]);
        if (oi == nullptr)
          continue;

        // Note that we are not calling operation_pre/post() callbacks
        // here since the meta operation is dist and we know what we
        // are doing.
        //
        current_inner_oif = oi;
        current_outer_oif = nullptr;
        current_mode = oi->mode;
        dependency_count = 0;

        action a (dist_id, id);

        if (verb >= 6)
          dump (a);

        for (void* v: ts)
        {
          target& t (*static_cast<target*> (v));

          if (rs != t.base_scope ().root_scope ())
            fail << "out of project target " << t;

          level5 ([&]{trace << diag_doing (a, t);});

          match (a, t);
        }

        if (verb >= 6)
          dump (a);
      }

      // Add buildfiles that are not normally loaded as part of the
      // project, for example, the export stub. They will still be
      // ignored on the next step if the user explicitly marked them
      // nodist.
      //
      auto add_adhoc = [&src_root, &trace] (const dir_path& d, const char* f)
        {
          path p (d / path (f));
          if (file_exists (p))
          {
            const char* e (p.extension ());
            targets.insert<buildfile> (
              p.directory (),
              p.leaf ().base ().string (),
              &extension_pool.find (e == nullptr ? "" : e), // Specified.
              trace);
          }
        };

      add_adhoc (src_root, "build/export.build");

      // The same for subprojects that have been loaded.
      //
      if (auto l = rs->vars["subprojects"])
      {
        for (auto p: as<subprojects> (*l))
        {
          const dir_path& pd (p.second);
          dir_path out_nroot (out_root / pd);
          scope& nrs (scopes.find (out_nroot));

          if (nrs.out_path () != out_nroot) // This subproject not loaded.
            continue;

          const dir_path& src_nroot (nrs.src_path ());

          if (!src_nroot.sub (src_root)) // Not a strong amalgamation.
            continue;

          add_adhoc (src_nroot, "build/export.build");
        }
      }

      // Collect the files. We want to take the snapshot of targets
      // since updating some of them may result in more targets being
      // entered.
      //
      action_targets files;
      const variable& dist_var (var_pool.find ("dist"));

      for (const auto& pt: targets)
      {
        file* ft (pt->is_a<file> ());

        if (ft == nullptr) // Not a file.
          continue;

        if (ft->dir.sub (src_root))
        {
          // Include unless explicitly excluded.
          //
          auto l ((*ft)[dist_var]);

          if (l && !as<bool> (*l))
            level5 ([&]{trace << "excluding " << *ft;});
          else
            files.push_back (ft);

          continue;
        }

        if (ft->dir.sub (out_root))
        {
          // Exclude unless explicitly included.
          //
          auto l ((*ft)[dist_var]);

          if (l && as<bool> (*l))
          {
            level5 ([&]{trace << "including " << *ft;});
            files.push_back (ft);
          }

          continue;
        }
      }

      // Make sure what we need to distribute is up to date.
      //
      {
        if (perform.meta_operation_pre != nullptr)
          perform.meta_operation_pre ();

        current_mif = &perform;

        if (perform.operation_pre != nullptr)
          perform.operation_pre (update_id);

        current_inner_oif = &update;
        current_outer_oif = nullptr;
        current_mode = update.mode;
        dependency_count = 0;

        action a (perform_id, update_id);

        perform.match (a, files);
        perform.execute (a, files, true); // Run quiet.

        if (perform.operation_post != nullptr)
          perform.operation_post (update_id);

        if (perform.meta_operation_post != nullptr)
          perform.meta_operation_post ();
      }

      dir_path td (dist_root / dir_path (dist_package));

      // Clean up the target directory.
      //
      // @@ Not for incremental dist?
      //
      if (build2::rmdir_r (td) == rmdir_status::not_empty)
        fail << "unable to clean target directory " << td;

      install (dist_cmd, td);

      // Copy over all the files.
      //
      for (void* v: files)
      {
        file& t (*static_cast<file*> (v));

        // Figure out where this file is inside the target directory.
        //
        dir_path d (td);
        d /= t.dir.sub (src_root)
          ? t.dir.leaf (src_root)
          : t.dir.leaf (out_root);

        if (!dir_exists (d))
          install (dist_cmd, d);

        install (dist_cmd, t, d);
      }

      // Archive if requested.
      //
      if (auto l = rs->vars["dist.archives"])
      {
        for (const string& e: as<strings> (*l))
          archive (dist_root, dist_package, e);
      }
    }

    // install -d <dir>
    //
    static void
    install (const string& cmd, const dir_path& d)
    {
      path reld (relative (d));

      cstrings args {cmd.c_str (), "-d"};

      args.push_back ("-m");
      args.push_back ("755");
      args.push_back (reld.string ().c_str ());
      args.push_back (nullptr);

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "dist -d " << d;

      try
      {
        process pr (args.data ());

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    // install <file> <dir>
    //
    static void
    install (const string& cmd, file& t, const dir_path& d)
    {
      path reld (relative (d));
      path relf (relative (t.path ()));

      cstrings args {cmd.c_str ()};

      // Preserve timestamps. This could becomes important if, for
      // example, we have pre-generated sources. Note that the
      // install-sh script doesn't support this option, while both
      // Linux and BSD install's do.
      //
      args.push_back ("-p");

      // Assume the file is executable if the owner has execute
      // permission, in which case we make it executable for
      // everyone.
      //
      args.push_back ("-m");
      args.push_back (
        (path_permissions (t.path ()) & permissions::xu) == permissions::xu
        ? "755"
        : "644");

      args.push_back (relf.string ().c_str ());
      args.push_back (reld.string ().c_str ());
      args.push_back (nullptr);

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << "dist " << t;

      try
      {
        process pr (args.data ());

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    static void
    archive (const dir_path& root, const string& pkg, const string& e)
    {
      string a (pkg + '.' + e);

      // Delete old archive for good measure.
      //
      path ap (root / path (a));
      if (file_exists (ap))
        rmfile (ap);

      // Use zip for .zip archives. Everything else goes to tar in the
      // auto-compress mode (-a).
      //
      cstrings args;
      if (e == "zip")
        args = {"zip", "-rq", a.c_str (), pkg.c_str (), nullptr};
      else
        args = {"tar", "-a", "-cf", a.c_str (), pkg.c_str (), nullptr};

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << args[0] << " " << ap;

      try
      {
        // Change child's working directory to dist_root.
        //
        process pr (root.string ().c_str (), args.data ());

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e.what ();

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    meta_operation_info dist {
      "dist",
      "distribute",
      "distributing",
      "has nothing to distribute", // We cannot "be distributed".
      &dist_meta_operation_pre,
      &dist_operation_pre,
      &load,   // normal load
      &search, // normal search
      &dist_match,
      &dist_execute,
      nullptr, // operation post
      nullptr  // meta-operation post
    };
  }
}
