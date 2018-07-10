// file      : build2/dist/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/dist/operation.hxx>

#include <libbutl/sha1.mxx>
#include <libbutl/sha256.mxx>

#include <libbutl/filesystem.mxx> // path_match()

#include <build2/file.hxx>
#include <build2/dump.hxx>
#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/context.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>

#include <build2/dist/module.hxx>

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
    // Return the destination file path.
    //
    static path
    install (const process_path& cmd, const file&, const dir_path&);

    // tar|zip ... <dir>/<pkg>.<ext> <pkg>
    //
    // Return the archive file path.
    //
    static path
    archive (const dir_path& root,
             const string& pkg,
             const dir_path& dir,
             const string& ext);

    // <ext>sum <arc> > <dir>/<arc>.<ext>
    //
    // Return the checksum file path.
    //
    static path
    checksum (const path& arc, const dir_path& dir, const string& ext);

    static operation_id
    dist_operation_pre (const values&, operation_id o)
    {
      if (o != default_id)
        fail << "explicit operation specified for meta-operation dist";

      return o;
    }

    static void
    dist_execute (const values&, action, action_targets& ts,
                  uint16_t, bool prog)
    {
      tracer trace ("dist_execute");

      // For now we assume all the targets are from the same project.
      //
      const target& t (ts[0].as_target ());
      const scope* rs (t.base_scope ().root_scope ());

      if (rs == nullptr)
        fail << "out of project target " << t;

      const dir_path& out_root (rs->out_path ());
      const dir_path& src_root (rs->src_path ());

      if (out_root == src_root)
        fail << "in-tree distribution of target " << t <<
          info << "distribution requires out-of-tree build";

      // Make sure we have the necessary configuration before we get down to
      // business.
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
      for (const action_target& at: ts)
      {
        const target& t (at.as_target ());

        if (rs != t.base_scope ().root_scope ())
          fail << "target " << t << " is from a different project" <<
            info << "one dist meta-operation can handle one project" <<
            info << "consider using several dist meta-operations";
      }

      // We used to print 'dist <target>' at verbosity level 1 but that has
      // proven to be just noise. Though we still want to print something
      // since otherwise, once the progress line is cleared, we may end up
      // with nothing printed at all.
      //
      // Note that because of this we can also suppress diagnostics noise
      // (e.g., output directory creation) in all the operations below.
      //
      if (verb == 1)
        text << "dist " << dist_package;

      // Match a rule for every operation supported by this project. Skip
      // default_id.
      //
      // Note that we are not calling operation_pre/post() callbacks here
      // since the meta operation is dist and we know what we are doing.
      //
      values params;
      const path locf ("<dist>");
      const location loc (&locf); // Dummy location.

      for (operations::size_type id (default_id + 1);
           id < rs->operations.size ();
           ++id)
      {
        if (const operation_info* oif = rs->operations[id])
        {
          // Skip aliases (e.g., update-for-install). In fact, one can argue
          // the default update should be sufficient since it is assumed to
          // update all prerequisites and we no longer support ad hoc stuff
          // like test.input. Though here we are using the dist meta-operation,
          // not perform.
          //
          if (oif->id != id)
            continue;

          // Use standard (perform) match.
          //
          if (oif->pre != nullptr)
          {
            if (operation_id pid = oif->pre (params, dist_id, loc))
            {
              const operation_info* poif (rs->operations[pid]);
              set_current_oif (*poif, oif, false /* diag_noise */);
              action a (dist_id, poif->id, oif->id);
              match (params, a, ts,
                     1     /* diag (failures only) */,
                     false /* progress */);
            }
          }

          set_current_oif (*oif, nullptr, false /* diag_noise */);
          action a (dist_id, oif->id);
          match (params, a, ts,
                 1     /* diag (failures only) */,
                 false /* progress */);

          if (oif->post != nullptr)
          {
            if (operation_id pid = oif->post (params, dist_id))
            {
              const operation_info* poif (rs->operations[pid]);
              set_current_oif (*poif, oif, false /* diag_noise */);
              action a (dist_id, poif->id, oif->id);
              match (params, a, ts,
                     1     /* diag (failures only) */,
                     false /* progress */);
            }
          }
        }
      }

      // Add buildfiles that are not normally loaded as part of the project,
      // for example, the export stub. They will still be ignored on the next
      // step if the user explicitly marked them dist=false.
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

      // Collect the files. We want to take the snapshot of targets since
      // updating some of them may result in more targets being entered.
      //
      // Note that we are not showing progress here (e.g., "N targets to
      // distribute") since it will be useless (too fast).
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
        if (mo_perform.meta_operation_pre != nullptr)
          mo_perform.meta_operation_pre (params, loc);

        // This is a hack since according to the rules we need to completely
        // reset the state. We could have done that (i.e., saved target names
        // and then re-searched them in the new tree) but that would just slow
        // things down while this little cheat seems harmless (i.e., assume
        // the dist mete-opreation is "compatible" with perform).
        //
        // Note also that we don't do any structured result printing.
        //
        size_t on (current_on);
        set_current_mif (mo_perform);
        current_on = on + 1;

        if (mo_perform.operation_pre != nullptr)
          mo_perform.operation_pre (params, update_id);

        set_current_oif (op_update, nullptr, false /* diag_noise */);

        action a (perform_id, update_id);

        mo_perform.match   (params, a, files,
                            1    /* diag (failures only) */,
                            prog /* progress */);

        mo_perform.execute (params, a, files,
                            1    /* diag (failures only) */,
                            prog /* progress */);

        if (mo_perform.operation_post != nullptr)
          mo_perform.operation_post (params, update_id);

        if (mo_perform.meta_operation_post != nullptr)
          mo_perform.meta_operation_post (params);
      }

      dir_path td (dist_root / dir_path (dist_package));

      // Clean up the target directory.
      //
      if (build2::rmdir_r (td, true, 2) == rmdir_status::not_empty)
        fail << "unable to clean target directory " << td;

      auto_rmdir rm_td (td); // Clean it up if things go bad.
      install (dist_cmd, td);

      // Copy over all the files. Apply post-processing callbacks.
      //
      module& mod (*rs->modules.lookup<module> (module::name));

      prog = prog && show_progress (1 /* max_verb */);
      size_t prog_percent (0);

      for (size_t i (0), n (files.size ()); i != n; ++i)
      {
        const file& t (*files[i].as_target ().is_a<file> ());

        // Figure out where this file is inside the target directory.
        //
        bool src (t.dir.sub (src_root));
        dir_path dl (src ? t.dir.leaf (src_root) : t.dir.leaf (out_root));

        dir_path d (td / dl);
        if (!exists (d))
          install (dist_cmd, d);

        path r (install (dist_cmd, t, d));

        // See if this file is in a subproject.
        //
        const scope* srs (rs);
        const module::callbacks* cbs (&mod.callbacks_);

        if (auto l = rs->vars[var_subprojects])
        {
          for (auto p: cast<subprojects> (l))
          {
            const dir_path& pd (p.second);
            if (dl.sub (pd))
            {
              srs = &scopes.find (out_root / pd);

              if (auto* m = srs->modules.lookup<module> (module::name))
                cbs = &m->callbacks_;
              else
                fail << "dist module not loaded in subproject " << pd;

              break;
            }
          }
        }

        for (module::callback cb: *cbs)
        {
          const path& pat (cb.pattern);

          // If we have a directory, then it should be relative to the project
          // root.
          //
          if (!pat.simple ())
          {
            assert (pat.relative ());

            dir_path d ((src ? srs->src_path () : srs->out_path ()) /
                        pat.directory ());
            d.normalize ();

            if (d != t.dir)
              continue;
          }

          if (path_match (pat.leaf ().string (), t.path ().leaf ().string ()))
            cb.function (r, *srs, cb.data);
        }

        if (prog)
        {
          // Note that this is not merely an optimization since if stderr is
          // not a terminal, we print real lines for progress.
          //
          size_t p ((i * 100) / n);

          if (prog_percent != p)
          {
            prog_percent = p;

            diag_progress_lock pl;
            diag_progress  = ' ';
            diag_progress += to_string (prog_percent);
            diag_progress += "% of targets distributed";
          }
        }
      }

      // Clear the progress if shown.
      //
      if (prog)
      {
        diag_progress_lock pl;
        diag_progress.clear ();
      }

      rm_td.cancel ();

      // Archive and checksum if requested.
      //
      if (lookup as = rs->vars["dist.archives"])
      {
        lookup cs (rs->vars["dist.checksums"]);

        // Split the dist.{archives,checksums} value into a directory and
        // extension.
        //
        auto split = [] (const path& p, const dir_path& r, const char* what)
        {
          dir_path d (p.relative () ? r : dir_path ());
          d /= p.directory ();

          const string& s (p.string ());
          size_t i (path::traits::find_leaf (s));

          if (i == string::npos)
            fail << "invalid extension '" << s << "' in " << what;

          if (s[i] == '.') // Skip the dot if specified.
            ++i;

          return pair<dir_path, string> (move (d), string (s, i));
        };

        for (const path& p: cast<paths> (as))
        {
          auto ap (split (p, dist_root, "dist.archives"));
          path a (archive (dist_root, dist_package, ap.first, ap.second));

          for (const path& p: cast<paths> (cs))
          {
            auto cp (split (p, ap.first, "dist.checksums"));
            checksum (a, cp.first, cp.second);
          }
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

      run (cmd, args);
    }

    // install <file> <dir>
    //
    static path
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

      run (cmd, args);

      return d / relf.leaf ();
    }

    static path
    archive (const dir_path& root,
             const string& pkg,
             const dir_path& dir,
             const string& e)
    {
      path an (pkg + '.' + e);

      // Delete old archive for good measure.
      //
      path ap (dir / an);
      if (exists (ap, false))
        rmfile (ap);

      // Use zip for .zip archives. Also recognize and handle a few well-known
      // tar.xx cases (in case tar doesn't support -a or has other issues like
      // MSYS). Everything else goes to tar in the auto-compress mode (-a).
      //
      cstrings args;

      // Separate compressor (gzip, xz, etc) state.
      //
      size_t i (0);        // Command line start or 0 if not used.
      auto_rmfile out_rm;  // Output file cleanup (must come first).
      auto_fd out_fd;      // Output file.

      if (e == "zip")
      {
        args = {"zip",
                "-rq", ap.string ().c_str (),
                pkg.c_str (),
                nullptr};
      }
      else
      {
        if (const char* c = (e == "tar.gz"  ? "gzip"  :
                             e == "tar.xz"  ? "xz"    :
                             e == "tar.bz2" ? "bzip2" :
                             nullptr))
        {
          args = {"tar",
                  "-cf", "-",
                  pkg.c_str (),
                  nullptr};

          i = args.size ();
          args.push_back (c);
          args.push_back (nullptr);
          args.push_back (nullptr); // Pipe end.

          try
          {
            out_fd = fdopen (ap,
                             fdopen_mode::out      | fdopen_mode::binary |
                             fdopen_mode::truncate | fdopen_mode::create);
            out_rm = auto_rmfile (ap);
          }
          catch (const io_error& e)
          {
            fail << "unable to open " << ap << ": " << e;
          }
        }
        else if (e == "tar")
          args = {"tar",
                  "-cf", ap.string ().c_str (),
                  pkg.c_str (),
                  nullptr};
        else
          args = {"tar",
                  "-a",
                  "-cf", ap.string ().c_str (),
                  pkg.c_str (),
                  nullptr};
      }

      process_path app; // Archiver path.
      process_path cpp; // Compressor path.

      app = run_search (args[0]);

      if (i != 0)
        cpp = run_search (args[i]);

      if (verb >= 2)
        print_process (args);
      else if (verb)
        text << args[0] << ' ' << ap;

      process apr;
      process cpr;

      // Change the archiver's working directory to dist_root.
      //
      apr = run_start (app,
                       args.data (),
                       0                 /* stdin  */,
                       (i != 0 ? -1 : 1) /* stdout */,
                       true              /* error */,
                       root);

      // Start the compressor if required.
      //
      if (i != 0)
      {
        cpr = run_start (cpp,
                         args.data () + i,
                         apr.in_ofd.get () /* stdin  */,
                         out_fd.get ()     /* stdout */);

        cpr.in_ofd.reset (); // Close the archiver's stdout on our side.
        run_finish (args.data () + i, cpr);
      }

      run_finish (args.data (), apr);

      out_rm.cancel ();
      return ap;
    }

    static path
    checksum (const path& ap, const dir_path& dir, const string& e)
    {
      path     an (ap.leaf ());
      dir_path ad (ap.directory ());

      path cn (an + '.' + e);

      // Delete old checksum for good measure.
      //
      path cp (dir / cn);
      if (exists (cp, false))
        rmfile (cp);

      auto_rmfile c_rm; // Note: must come first.
      auto_fd c_fd;
      try
      {
        c_fd = fdopen (cp,
                       fdopen_mode::out     |
                       fdopen_mode::create  |
                       fdopen_mode::truncate);
        c_rm = auto_rmfile (cp);
      }
      catch (const io_error& e)
      {
        fail << "unable to open " << cp << ": " << e;
      }

      // The plan is as follows: look for the <ext>sum program (e.g., sha1sum,
      // md5sum, etc). If found, then use that, otherwise, fall back to our
      // built-in checksum calculation support.
      //
      // There are two benefits to first trying the external program: it may
      // supports more checksum algorithms and could be faster than our
      // built-in code.
      //
      string pn (e + "sum");
      process_path pp (process::try_path_search (pn, true /* init */));

      if (!pp.empty ())
      {
        const char* args[] {
          pp.recall_string (),
          "-b" /* binary */,
          an.string ().c_str (),
          nullptr};

        if (verb >= 2)
          print_process (args);
        else if (verb)
          text << args[0] << ' ' << cp;

        // Note that to only get the archive name (without the directory) in
        // the output we have to run from the archive's directory.
        //
        process pr (run_start (pp,
                               args,
                               1             /* stdin */,
                               c_fd.get ()   /* stdout */,
                               true          /* error */,
                               ad            /* cwd */));
        run_finish (args, pr);
      }
      else
      {
        string (*f) (ifdstream&);

        // Note: remember to update info: below if adding another algorithm.
        //
        if (e == "sha1")
          f = [] (ifdstream& i) -> string {return sha1 (i).string ();};
        else if (e == "sha256")
          f = [] (ifdstream& i) -> string {return sha256 (i).string ();};
        else
          fail << "no built-in support for checksum algorithm " << e
               << " nor " << e << "sum program found" <<
            info << "built-in support is available for sha1, sha256" << endf;

        if (verb >= 2)
          text << "cat >" << cp;
        else if (verb)
          text << e << "sum " << cp;

        string c;
        try
        {
          ifdstream is (ap, fdopen_mode::in | fdopen_mode::binary);
          c = f (is);
          is.close ();
        }
        catch (const io_error& e)
        {
          fail << "unable to read " << ap << ": " << e;
        }

        try
        {
          ofdstream os (move (c_fd));
          os << c << " *" << an << endl;
          os.close ();
        }
        catch (const io_error& e)
        {
          fail << "unable to write " << cp << ": " << e;
        }
      }

      c_rm.cancel ();
      return cp;
    }

    static include_type
    dist_include (action,
                  const target&,
                  const prerequisite_member& p,
                  include_type i)
    {
      tracer trace ("dist_include");

      // Override excluded to adhoc so that every source is included into the
      // distribution. Note that this should be harmless to a custom rule
      // given the prescribed semantics of adhoc (match/execute but otherwise
      // ignore) is followed.
      //
      if (i == include_type::excluded)
      {
        l5 ([&]{trace << "overriding exclusion of " << p;});
        i = include_type::adhoc;
      }

      return i;
    }

    const meta_operation_info mo_dist {
      dist_id,
      "dist",
      "distribute",
      "distributing",
      "distributed",
      "has nothing to distribute", // We cannot "be distributed".
      true,    // bootstrap_outer
      nullptr, // meta-operation pre
      &dist_operation_pre,
      &load,   // normal load
      &search, // normal search
      nullptr, // no match (see execute()).
      &dist_execute,
      nullptr, // operation post
      nullptr, // meta-operation post
      &dist_include
    };
  }
}
