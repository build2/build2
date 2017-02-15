// file      : build2/dist/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/dist/operation>

#include <build2/file>
#include <build2/dump>
#include <build2/scope>
#include <build2/target>
#include <build2/context>
#include <build2/algorithm>
#include <build2/filesystem>
#include <build2/diagnostics>

using namespace std;
using namespace butl;

namespace build2
{
  namespace dist
  {
    // install -d <dir>
    //
    static void
    install (const process_path& cmd, const dir_path&);

    // install <file> <dir>
    //
    static void
    install (const process_path& cmd, const file&, const dir_path&);

    // cd <root> && tar|zip ... <dir>/<pkg>.<ext> <pkg>
    //
    static void
    archive (const dir_path& root,
             const string& pkg,
             const dir_path& dir,
             const string& ext);

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

    static void
    dist_execute (action, action_targets& ts, bool)
    {
      tracer trace ("dist_execute");

      // For now we assume all the targets are from the same project.
      //
      const target& t (*static_cast<const target*> (ts[0]));
      const scope* rs (t.base_scope ().root_scope ());

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

      const dir_path& dist_root (cast<dir_path> (l));

      if (!exists (dist_root))
        fail << "root distribution directory " << dist_root
             << " does not exist";

      l = rs->vars["dist.package"];

      if (!l || l->empty ())
        fail << "unknown distribution package name" <<
          info << "did you forget to set dist.package?";

      const string& dist_package (cast<string> (l));
      const process_path& dist_cmd (cast<process_path> (rs->vars["dist.cmd"]));

      // Verify all the targets are from the same project.
      //
      for (const void* v: ts)
      {
        const target& t (*static_cast<const target*> (v));

        if (rs != t.base_scope ().root_scope ())
          fail << "target " << t << " is from a different project" <<
            info << "one dist() meta-operation can handle one project" <<
            info << "consider using several dist() meta-operations";
      }

      // Match a rule for every operation supported by this project. Skip
      // default_id.
      //
      for (operations::size_type id (default_id + 1);
           id < rs->operations.size ();
           ++id)
      {
        if (const operation_info* oif = rs->operations[id])
        {
          // Note that we are not calling operation_pre/post() callbacks here
          // since the meta operation is dist and we know what we are doing.
          //
          set_current_oif (*oif);

          match (action (dist_id, oif->id), ts); // Standard (perform) match.
        }
      }

      // Add buildfiles that are not normally loaded as part of the
      // project, for example, the export stub. They will still be
      // ignored on the next step if the user explicitly marked them
      // nodist.
      //
      auto add_adhoc = [&trace] (const scope& rs, const path& f)
      {
        path p (rs.src_path () / f);
        if (exists (p))
        {
          dir_path d (p.directory ());

          // Figure out if we need out.
          //
          dir_path out (rs.src_path () != rs.out_path ()
                        ? out_src (d, rs)
                        : dir_path ());

          targets.insert<buildfile> (
            move (d),
            move (out),
            p.leaf ().base ().string (),
            p.extension (),              // Specified.
            trace);
        }
      };

      add_adhoc (*rs, export_file);

      // The same for subprojects that have been loaded.
      //
      if (auto l = rs->vars[var_subprojects])
      {
        for (auto p: cast<subprojects> (l))
        {
          const dir_path& pd (p.second);
          dir_path out_nroot (out_root / pd);
          const scope& nrs (scopes.find (out_nroot));

          if (nrs.out_path () != out_nroot) // This subproject not loaded.
            continue;

          if (!nrs.src_path ().sub (src_root)) // Not a strong amalgamation.
            continue;

          add_adhoc (nrs, export_file);
        }
      }

      // Collect the files. We want to take the snapshot of targets
      // since updating some of them may result in more targets being
      // entered.
      //
      action_targets files;
      const variable& dist_var (var_pool["dist"]);

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

          if (l && !cast<bool> (l))
            l5 ([&]{trace << "excluding " << *ft;});
          else
            files.push_back (ft);

          continue;
        }

        if (ft->dir.sub (out_root))
        {
          // Exclude unless explicitly included.
          //
          auto l ((*ft)[dist_var]);

          if (l && cast<bool> (l))
          {
            l5 ([&]{trace << "including " << *ft;});
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

        // This is a hack since according to the rules we need to completely
        // reset the state. We could have done that (i.e., saved target names
        // and then re-searched them in the new tree) but that would just slow
        // things down while this little cheat seems harmless (i.e., assume
        // the dist mete-opreation is "compatible" with perform).
        //
        size_t on (current_on);
        set_current_mif (perform);
        current_on = on + 1;

        if (perform.operation_pre != nullptr)
          perform.operation_pre (update_id);

        set_current_oif (update);

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
      for (const void* v: files)
      {
        const file& t (*static_cast<const file*> (v));

        // Figure out where this file is inside the target directory.
        //
        dir_path d (td);
        d /= t.dir.sub (src_root)
          ? t.dir.leaf (src_root)
          : t.dir.leaf (out_root);

        if (!exists (d))
          install (dist_cmd, d);

        install (dist_cmd, t, d);
      }

      // Archive if requested.
      //
      if (auto l = rs->vars["dist.archives"])
      {
        for (const path& p: cast<paths> (l))
        {
          dir_path d (p.relative () ? dist_root : dir_path ());
          d /= p.directory ();

          const string& s (p.string ());
          size_t i (path::traits::find_leaf (s));

          if (i == string::npos)
            fail << "invalid archive '" << s << "' in dist.archives";

          if (s[i] == '.') // Skip dot if specified.
            ++i;

          archive (dist_root, dist_package, d, string (s, i));
        }
      }
    }

    // install -d <dir>
    //
    static void
    install (const process_path& cmd, const dir_path& d)
    {
      path reld (relative (d));

      cstrings args {cmd.recall_string (), "-d"};

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
        process pr (cmd, args.data ());

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    // install <file> <dir>
    //
    static void
    install (const process_path& cmd, const file& t, const dir_path& d)
    {
      dir_path reld (relative (d));
      path relf (relative (t.path ()));

      cstrings args {cmd.recall_string ()};

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
        process pr (cmd, args.data ());

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    static void
    archive (const dir_path& root,
             const string& pkg,
             const dir_path& dir,
             const string& e)
    {
      string a (pkg + '.' + e);

      // Delete old archive for good measure.
      //
      path ap (dir / path (a));
      if (exists (ap, false))
        rmfile (ap);

      // Use zip for .zip archives. Everything else goes to tar in the
      // auto-compress mode (-a).
      //
      cstrings args;
      if (e == "zip")
        args = {"zip", "-rq",
                ap.string ().c_str (), pkg.c_str (), nullptr};
      else
        args = {"tar", "-a", "-cf",
                ap.string ().c_str (), pkg.c_str (), nullptr};

      try
      {
        process_path pp (process::path_search (args[0]));

        if (verb >= 2)
          print_process (args);
        else if (verb)
          text << args[0] << " " << ap;

        // Change child's working directory to dist_root.
        //
        process pr (root.string ().c_str (), pp, args.data ());

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child ())
          exit (1);

        throw failed ();
      }
    }

    const meta_operation_info dist {
      dist_id,
      "dist",
      "distribute",
      "distributing",
      "distributed",
      "has nothing to distribute", // We cannot "be distributed".
      nullptr, // meta-operation pre
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
