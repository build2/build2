// file      : libbuild2/dist/operation.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/dist/operation.hxx>

#include <libbutl/sha1.mxx>
#include <libbutl/sha256.mxx>

#include <libbutl/path-pattern.mxx>

#include <libbuild2/file.hxx>
#include <libbuild2/dump.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/dist/module.hxx>

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
    archive (context& ctx,
             const dir_path& root,
             const string& pkg,
             const dir_path& dir,
             const string& ext);

    // <ext>sum <arc> > <dir>/<arc>.<ext>
    //
    // Return the checksum file path.
    //
    static path
    checksum (context&,
              const path& arc, const dir_path& dir, const string& ext);

    static operation_id
    dist_operation_pre (const values&, operation_id o)
    {
      if (o != default_id)
        fail << "explicit operation specified for dist meta-operation";

      return o;
    }

    // Enter the specified source file as a target of type T. The path is
    // expected to be normalized and relative to src_root. If the third
    // argument is false, then first check if the file exists. If the fourth
    // argument is true, then set the target's path.
    //
    template <typename T>
    static const T*
    add_target (const scope& rs, const path& f, bool e = false, bool s = false)
    {
      tracer trace ("dist::add_target");

      path p (rs.src_path () / f);
      if (e || exists (p))
      {
        dir_path d (p.directory ());

        // Figure out if we need out.
        //
        dir_path out (rs.src_path () != rs.out_path ()
                      ? out_src (d, rs)
                      : dir_path ());

        const T& t (rs.ctx.targets.insert<T> (
                      move (d),
                      move (out),
                      p.leaf ().base ().string (),
                      p.extension (),              // Specified.
                      trace));

        if (s)
          t.path (move (p));

        return &t;
      }

      return nullptr;
    }

    // Recursively traverse an src_root subdirectory entering/collecting the
    // contained files and file symlinks as the file targets and skipping
    // entries that start with a dot. Follow directory symlinks (preserving
    // their names) and fail on dangling symlinks.
    //
    static void
    add_subdir (const scope& rs, const dir_path& sd, action_targets& files)
    {
      dir_path d (rs.src_path () / sd);

      try
      {
        for (const dir_entry& e: dir_iterator (d, false /* ignore_dangling */))
        {
          const path& n (e.path ());

          if (n.string ()[0] != '.')
          try
          {
            if (e.type () == entry_type::directory) // Can throw.
              add_subdir (rs, sd / path_cast<dir_path> (n), files);
            else
              files.push_back (add_target<file> (rs, sd / n, true, true));
          }
          catch (const system_error& e)
          {
            fail << "unable to stat " << (d / n) << ": " << e;
          }
        }
      }
      catch (const system_error& e)
      {
        fail << "unable to iterate over " << d << ": " << e;
      }
    }

    // If tgt is NULL, then this is the bootstrap mode.
    //
    static void
    dist_project (const scope& rs, const target* tgt, bool prog)
    {
      tracer trace ("dist::dist_project");

      context& ctx (rs.ctx);

      const dir_path& out_root (rs.out_path ());
      const dir_path& src_root (rs.src_path ());

      // Make sure we have the necessary configuration before we get down to
      // business.
      //
      auto l (rs.vars["dist.root"]);

      if (!l || l->empty ())
        fail << "unknown root distribution directory" <<
          info << "did you forget to specify config.dist.root?";

      // We used to complain if dist.root does not exist but then, similar
      // to install, got tired of user's complaints. So now we just let
      // install -d for the package directory create it if necessary.
      //
      const dir_path& dist_root (cast<dir_path> (l));

      l = rs.vars["dist.package"];

      if (!l || l->empty ())
        fail << "unknown distribution package name" <<
          info << "did you forget to set dist.package?";

      const string& dist_package (cast<string> (l));
      const process_path& dist_cmd (cast<process_path> (rs.vars["dist.cmd"]));

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

      // Get the list of files to distribute.
      //
      action_targets files;

      if (tgt != nullptr)
      {
        l5 ([&]{trace << "load dist " << rs;});

        // Match a rule for every operation supported by this project. Skip
        // default_id.
        //
        // Note that we are not calling operation_pre/post() callbacks here
        // since the meta operation is dist and we know what we are doing.
        //
        values params;
        path_name pn ("<dist>");
        const location loc (pn); // Dummy location.
        {
          action_targets ts {tgt};

          auto mog = make_guard ([&ctx] () {ctx.match_only = false;});
          ctx.match_only = true;

          const operations& ops (rs.root_extra->operations);
          for (operations::size_type id (default_id + 1); // Skip default_id.
               id < ops.size ();
               ++id)
          {
            if (const operation_info* oif = ops[id])
            {
              // Skip aliases (e.g., update-for-install). In fact, one can
              // argue the default update should be sufficient since it is
              // assumed to update all prerequisites and we no longer support
              // ad hoc stuff like test.input. Though here we are using the
              // dist meta-operation, not perform.
              //
              if (oif->id != id)
                continue;

              // Use standard (perform) match.
              //
              if (oif->pre != nullptr)
              {
                if (operation_id pid = oif->pre (params, dist_id, loc))
                {
                  const operation_info* poif (ops[pid]);
                  ctx.current_operation (*poif, oif, false /* diag_noise */);
                  action a (dist_id, poif->id, oif->id);
                  match (params, a, ts,
                         1     /* diag (failures only) */,
                         false /* progress */);
                }
              }

              ctx.current_operation (*oif, nullptr, false /* diag_noise */);
              action a (dist_id, oif->id);
              match (params, a, ts,
                     1     /* diag (failures only) */,
                     false /* progress */);

              if (oif->post != nullptr)
              {
                if (operation_id pid = oif->post (params, dist_id))
                {
                  const operation_info* poif (ops[pid]);
                  ctx.current_operation (*poif, oif, false /* diag_noise */);
                  action a (dist_id, poif->id, oif->id);
                  match (params, a, ts,
                         1     /* diag (failures only) */,
                         false /* progress */);
                }
              }
            }
          }
        }

        // Add ad hoc files and buildfiles that are not normally loaded as
        // part of the project, for example, the export stub. They will still
        // be ignored on the next step if the user explicitly marked them
        // dist=false.
        //
        auto add_adhoc = [] (const scope& rs)
        {
          add_target<buildfile> (rs, rs.root_extra->export_file);

          if (auto* m = rs.find_module<module> (module::name))
          {
            for (const path& f: m->adhoc)
            {
              if (!path_pattern (f))
                add_target<file> (rs, f);
              else
              try
              {
                path_search (f,
                             [&rs] (path&& pe, const string&, bool interm)
                             {
                               if (!interm)
                                 add_target<file> (rs, pe, true /* exists */);

                               return true;
                             },
                             rs.src_path (),
                             path_match_flags::none /* no follow_symlinks */);
              }
              catch (const system_error& e)
              {
                fail << "unable to scan " << rs.src_path () / f.directory ()
                     << ": " << e;
              }
            }
          }
        };

        add_adhoc (rs);

        // The same for subprojects that have been loaded.
        //
        if (const subprojects* ps = *rs.root_extra->subprojects)
        {
          for (auto p: *ps)
          {
            const dir_path& pd (p.second);
            dir_path out_nroot (out_root / pd);
            const scope& nrs (ctx.scopes.find (out_nroot));

            if (nrs.out_path () != out_nroot) // This subproject not loaded.
              continue;

            if (!nrs.src_path ().sub (src_root)) // Not a strong amalgamation.
              continue;

            add_adhoc (nrs);
          }
        }

        // Collect the files. We want to take the snapshot of targets since
        // updating some of them may result in more targets being entered.
        //
        // Note that we are not showing progress here (e.g., "N targets to
        // distribute") since it will be useless (too fast).
        //
        const variable& dist_var (ctx.var_pool["dist"]);

        for (const auto& pt: ctx.targets)
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
          // reset the state. We could have done that (i.e., saved target
          // names and then re-searched them in the new tree) but that would
          // just slow things down while this little cheat seems harmless
          // (i.e., assume the dist mete-opreation is "compatible" with
          // perform).
          //
          // Note also that we don't do any structured result printing.
          //
          size_t on (ctx.current_on);
          ctx.current_meta_operation (mo_perform);
          ctx.current_on = on + 1;

          if (mo_perform.operation_pre != nullptr)
            mo_perform.operation_pre (params, update_id);

          ctx.current_operation (op_update, nullptr, false /* diag_noise */);

          action a (perform_update_id);

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
      }
      else
      {
        l5 ([&]{trace << "bootstrap dist " << rs;});

        // Recursively enter/collect file targets in src_root ignoring those
        // that start with a dot.
        //
        // Note that, in particular, we also collect the symlinks which point
        // outside src_root (think of third-party project packaging with the
        // upstream git submodule at the root of the git project). Also note
        // that we could probably exclude symlinks which point outside the VCS
        // project (e.g., backlinks in a forwarded configuration) but that
        // would require the user to supply this boundary (since we don't have
        // the notion of VCS root at this level). So let's keep it simple for
        // now.
        //
        add_subdir (rs, dir_path (), files);
      }

      dir_path td (dist_root / dir_path (dist_package));

      // Clean up the target directory.
      //
      if (rmdir_r (ctx, td, true, 2) == rmdir_status::not_empty)
        fail << "unable to clean target directory " << td;

      auto_rmdir rm_td (td); // Clean it up if things go bad.
      install (dist_cmd, td);

      // Copy over all the files. Apply post-processing callbacks.
      //
      module& mod (*rs.find_module<module> (module::name));

      prog = prog && show_progress (1 /* max_verb */);
      size_t prog_percent (0);

      for (size_t i (0), n (files.size ()); i != n; ++i)
      {
        const file& t (*files[i].as<target> ().is_a<file> ());

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
        const scope* srs (&rs);
        const module::callbacks* cbs (&mod.callbacks_);

        if (const subprojects* ps = *rs.root_extra->subprojects)
        {
          for (auto p: *ps)
          {
            const dir_path& pd (p.second);
            if (dl.sub (pd))
            {
              srs = &ctx.scopes.find (out_root / pd);

              if (auto* m = srs->find_module<module> (module::name))
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

          if (path_match (t.path ().leaf ().string (), pat.leaf ().string ()))
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
      if (lookup as = rs.vars["dist.archives"])
      {
        lookup cs (rs.vars["dist.checksums"]);

        // Split the dist.{archives,checksums} value into a directory and
        // extension.
        //
        auto split = [] (const path& p, const dir_path& r, const char* what)
        {
          dir_path d (p.relative () ? r : dir_path ());
          d /= p.directory ();

          const string& s (p.string ());
          size_t i (path::traits_type::find_leaf (s));

          if (i == string::npos)
            fail << "invalid extension '" << s << "' in " << what;

          if (s[i] == '.') // Skip the dot if specified.
            ++i;

          return pair<dir_path, string> (move (d), string (s, i));
        };

        for (const path& p: cast<paths> (as))
        {
          auto ap (split (p, dist_root, "dist.archives"));
          path a (archive (ctx, dist_root, dist_package, ap.first, ap.second));

          if (cs)
          {
            for (const path& p: cast<paths> (cs))
            {
              auto cp (split (p, ap.first, "dist.checksums"));
              checksum (ctx, a, cp.first, cp.second);
            }
          }
        }
      }
    }

    static void
    dist_load_execute (const values&, action, action_targets& ts,
                       uint16_t, bool prog)
    {
      // We cannot do multiple projects because we need to start with a clean
      // set of targets.
      //
      if (ts.size () != 1)
        fail << "multiple targets in dist meta-operation" <<
          info << "one dist meta-operation can handle one project" <<
          info << "consider using several dist meta-operations";

      const target& t (ts[0].as<target> ());
      const scope* rs (t.base_scope ().root_scope ());

      if (rs == nullptr   ||
          !t.is_a<dir> () ||
          (rs->out_path () != t.dir && rs->src_path () != t.dir))
        fail << "dist meta-operation target must be project root directory";

      if (rs->out_path () == rs->src_path ())
        fail << "in-tree distribution of target " << t <<
          info << "distribution requires out-of-tree build";

      dist_project (*rs, &t, prog);
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
        (path_perms (t.path ()) & permissions::xu) == permissions::xu
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
    archive (context& ctx,
             const dir_path& root,
             const string& pkg,
             const dir_path& dir,
             const string& e)
    {
      path an (pkg + '.' + e);

      // Delete old archive for good measure.
      //
      path ap (dir / an);
      if (exists (ap, false))
        rmfile (ctx, ap);

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
        // On Windows we use libarchive's bsdtar (zip is an MSYS executabales).
        //
        // While not explicitly stated, the compression-level option works
        // for zip archives.
        //
#ifdef _WIN32
        args = {"bsdtar",
                "-a", // -a with the .zip extension seems to be the only way.
                "--options=compression-level=9",
                "-cf", ap.string ().c_str (),
                pkg.c_str (),
                nullptr};
#else
        args = {"zip",
                "-9",
                "-rq", ap.string ().c_str (),
                pkg.c_str (),
                nullptr};
#endif
      }
      else
      {
        // On Windows we use libarchive's bsdtar with auto-compression (tar
        // itself and quite a few compressors are MSYS executables).
        //
        // For gzip we also suppress the timestamp with -n to produce the
        // same archive for the same distribution.
        //
        const char* o (nullptr); // Option (compression level, etc).

#ifdef _WIN32
        const char* tar = "bsdtar";

        if (e == "tar.gz")
          o = "--options=compression-level=9,!timestamp";
#else
        const char* tar = "tar";

        // For gzip it's a good idea to use -9 by default. For bzip2, -9 is
        // the default. And for xz, -9 is not recommended as the default due
        // memory requirements.
        //
        // Note also that the compression level can be altered via the GZIP
        // (GZIP_OPT also seems to work), BZIP2, and XZ_OPT environment
        // variables, respectively.
        //
        const char* c (nullptr);

        if      (e == "tar.gz")  { c = "gzip";  o = "-9n"; }
        else if (e == "tar.xz")  { c = "xz";               }
        else if (e == "tar.bz2") { c = "bzip2";            }

        if (c != nullptr)
        {
          args = {tar,
                  "--format", "ustar",
                  "-cf", "-",
                  pkg.c_str (),
                  nullptr};

          i = args.size ();
          args.push_back (c);
          if (o != nullptr)
            args.push_back (o);
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
        else
#endif
        if (e == "tar")
          args = {tar,
                  "--format", "ustar",
                  "-cf", ap.string ().c_str (),
                  pkg.c_str (),
                  nullptr};
        else
        {
          args = {tar,
                  "--format", "ustar",
                  "-a"};

          if (o != nullptr)
            args.push_back (o);

          args.push_back ("-cf");
          args.push_back (ap.string ().c_str ());
          args.push_back (pkg.c_str ());
          args.push_back (nullptr);
        }
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
                       args,
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
    checksum (context& ctx,
              const path& ap, const dir_path& dir, const string& e)
    {
      path     an (ap.leaf ());
      dir_path ad (ap.directory ());

      path cn (an + '.' + e);

      // Delete old checksum for good measure.
      //
      path cp (dir / cn);
      if (exists (cp, false))
        rmfile (ctx, cp);

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
                               0             /* stdin */,
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
          fail << "unable to write to " << cp << ": " << e;
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
      tracer trace ("dist::dist_include");

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

    const meta_operation_info mo_dist_load {
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
      nullptr, // no match (see dist_execute()).
      &dist_load_execute,
      nullptr, // operation post
      nullptr, // meta-operation post
      &dist_include
    };

    // The bootstrap distribution mode.
    //
    // Note: pretty similar overall idea as the info meta-operation.
    //
    void
    init_config (scope&); // init.cxx

    static void
    dist_bootstrap_load (const values&,
                         scope& rs,
                         const path&,
                         const dir_path& out_base,
                         const dir_path& src_base,
                         const location& l)
    {
      if (rs.out_path () != out_base || rs.src_path () != src_base)
        fail (l) << "dist meta-operation target must be project root directory";

      setup_base (rs.ctx.scopes.rw (rs).insert (out_base), out_base, src_base);

      // Also initialize the dist.* variables (needed in dist_project()).
      //
      init_config (rs);
    }

    void
    dist_bootstrap_search (const values&,
                           const scope& rs,
                           const scope&,
                           const path&,
                           const target_key& tk,
                           const location& l,
                           action_targets& ts)
    {
      if (!tk.type->is_a<dir> ())
        fail (l) << "dist meta-operation target must be project root directory";

      ts.push_back (&rs);
    }

    static void
    dist_bootstrap_execute (const values&, action, action_targets& ts,
                            uint16_t, bool prog)
    {
      for (const action_target& at: ts)
        dist_project (at.as<scope> (), nullptr, prog);
    }

    const meta_operation_info mo_dist_bootstrap {
      dist_id,
      "dist",
      "distribute",
      "distributing",
      "distributed",
      "has nothing to distribute",
      true,    // bootstrap_outer //@@ Maybe not? (But will overrides work)?
      nullptr, // meta-operation pre
      &dist_operation_pre,
      &dist_bootstrap_load,
      &dist_bootstrap_search,
      nullptr, // no match (see dist_bootstrap_execute()).
      &dist_bootstrap_execute,
      nullptr, // operation post
      nullptr, // meta-operation post
      nullptr  // include
    };
  }
}
