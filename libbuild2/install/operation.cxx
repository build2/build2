// file      : libbuild2/install/operation.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/install/operation.hxx>

#include <sstream>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/install/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace install
  {
    bool context_data::
    filter (const scope& rs,
            const dir_path& base,
            const path& leaf,
            entry_type type)
    {
      assert (type != entry_type::unknown &&
              (type == entry_type::directory) == leaf.empty ());

      context& ctx (rs.ctx);

      auto& d (*static_cast<context_data*> (ctx.current_inner_odata.get ()));

      if (d.filters == nullptr || d.filters->empty ())
        return true;

      tracer trace ("install::context_data::filter");

      // Parse, resolve, and apply each filter in order.
      //
      // If redoing all this work for every entry proves too slow, we can
      // consider some form of caching (e.g., on the per-project basis).
      //
      size_t limit (0); // See below.

      for (const pair<string, string>& kv: *d.filters)
      {
        path k;
        try
        {
          k = path (kv.first);

          if (k.absolute ())
            k.normalize ();
        }
        catch (const invalid_path&)
        {
          fail << "invalid path '" << kv.first << "' in config.install.filter "
               << "value";
        }

        bool v;
        {
          const string& s (kv.second);

          size_t p (s.find (','));

          if (s.compare (0, p, "true") == 0)
            v = true;
          else if (s.compare (0, p, "false") == 0)
            v = false;
          else
            fail << "expected true or false instead of '" << string (s, 0, p)
                 << "' in config.install.filter value";

          if (p != string::npos)
          {
            if (s.compare (p + 1, string::npos, "symlink") == 0)
            {
              if (type != entry_type::symlink)
                continue;
            }
            else
              fail << "unknown modifier '" << string (s, p + 1) << "' in "
                   << "config.install.filter value";
          }
        }

        // @@ TODO (see below for all the corner cases). Note that in a sense
        //    we already have the match file in any subdirectory support via
        //    simple patterns so perhaps this is not worth the trouble. Or we
        //    could support some limited form (e.g., `**` should be in the
        //    last component). But it may still be tricky to determine if
        //    it is a sub-filter.
        //
        if (path_pattern_recursive (k))
          fail << "recursive wildcard pattern '" << kv.first << "' in "
               << "config.install.filter value";

        if (k.simple () && !k.to_directory ())
        {
          // Simple name/pattern matched against the leaf.
          //
          // @@ What if it is `**`?
          //
          if (path_pattern (k))
          {
            if (!path_match (leaf, k))
              continue;
          }
          else
          {
            if (k != leaf)
              continue;
          }
        }
        else
        {
          // Split into directory and leaf.
          //
          // @@ What if leaf is `**`?
          //
          dir_path d;
          if (k.to_directory ())
          {
            d = path_cast<dir_path> (move (k));
            k = path (); // No leaf.
          }
          else
          {
            d = k.directory ();
            k.make_leaf ();
          }

          // Resolve relative directory.
          //
          // Note that this resolution is potentially project-specific (that
          // is, different projects may have different install.* locaitons).
          //
          // Note that if the first component is/contains a wildcard (e.g.,
          // `*/`), then the resulution will fail, which feels correct (what
          // does */ mean?).
          //
          if (d.relative ())
          {
            // @@ Strictly speaking, this should be base, not root scope.
            //
            d = resolve_dir (rs, move (d));
          }

          // Return the number of path components in the path.
          //
          auto path_comp = [] (const path& p)
          {
            size_t n (0);
            for (auto i (p.begin ()); i != p.end (); ++i)
              ++n;
            return n;
          };

          // We need the sub() semantics but which uses pattern match instead
          // of equality for the prefix. Looks like chopping off the path and
          // calling path_match() on that is the best we can do.
          //
          // @@ Assumes no `**` components.
          //
          auto path_sub = [&path_comp] (const dir_path& ent,
                                        const dir_path& pat,
                                        size_t n = 0)
          {
            if (n == 0)
              n = path_comp (pat);

            dir_path p;
            for (auto i (ent.begin ()); n != 0 && i != ent.end (); --n, ++i)
              p.combine (*i, i.separator ());

            return path_match (p, pat);
          };

          // The following checks should continue on no match and fall through
          // to return.
          //
          if (k.empty ()) // Directory.
          {
            // Directories have special semantics.
            //
            // Consider this sequence of filters:
            //
            //   include/x86_64-linux-gnu/@true
            //   include/x86_64-linux-gnu/details/@false
            //   include/@false
            //
            // It seems the semantics we want is that only subcomponent
            // filters should apply. Maybe remember the latest matched
            // directory as a current limit? But perhaps we don't need to
            // remember the directory itself but the number of path
            // components?
            //
            // I guess for patterns we will use the actual matched directory,
            // not the pattern, to calculate the limit? @@ Because we
            // currently don't support `**`, we for now can count components
            // in the pattern.

            // Check if this is a sub-filter.
            //
            size_t n (path_comp (d));
            if (n <= limit)
              continue;

            if (path_pattern (d))
            {
              if (!path_sub (base, d, n))
                continue;
            }
            else
            {
              if (!base.sub (d))
                continue;
            }

            if (v)
            {
              limit = n;
              continue; // Continue looking for sub-filters.
            }
          }
          else
          {
            if (path_pattern (d))
            {
              if (!path_sub (base, d))
                continue;
            }
            else
            {
              if (!base.sub (d))
                continue;
            }

            if (path_pattern (k))
            {
              // @@ Does not handle `**`.
              //
              if (!path_match (leaf, k))
                continue;
            }
            else
            {
              if (k != leaf)
                continue;
            }
          }
        }

        l4 ([&]{trace << (base / leaf)
                      << (v ? " included by " : " excluded by ")
                      << kv.first << '@' << kv.second;});
        return v;
      }

      return true;
    }

#ifndef BUILD2_BOOTSTRAP
    context_data::
    context_data (const install::filters* fs, const path* mf)
        : filters (fs),
          manifest_name (mf),
          manifest_os (mf != nullptr
                       ? open_file_or_stdout (manifest_name, manifest_ofs)
                       : manifest_ofs),
          manifest_autorm (manifest_ofs.is_open () ? *mf : path ()),
          manifest_json (manifest_os, 0 /* indentation */)
    {
      if (manifest_ofs.is_open ())
      {
        manifest_file = *mf;
        manifest_file.complete ();
        manifest_file.normalize ();
      }
    }

    static path
    relocatable_path (context_data& d, const target& t, path p)
    {
      // This is both inefficient (re-detecting relocatable manifest for every
      // path) and a bit dirty (if multiple projects are being installed with
      // different install.{relocatable,root} values, we may end up producing
      // some paths relative and some absolute). But doing either of these
      // properly is probably not worth the extra complexity.
      //
      if (!d.manifest_file.empty ()) // Not stdout.
      {
        const scope& rs (t.root_scope ());

        if (cast_false<bool> (rs["install.relocatable"]))
        {
          // Note: install.root is abs_dir_path so absolute and normalized.
          //
          const dir_path* root (cast_null<dir_path> (rs["install.root"]));
          if (root == nullptr)
            fail << "unknown installation root directory in " << rs <<
              info << "did you forget to specify config.install.root?";

          // The manifest path would include chroot so if used, we need to add
          // it to root and the file path (we could also strip it, but then
          // making it absolute gets tricky on Windows).
          //
          dir_path md (d.manifest_file.directory ());

          if (md.sub (chroot_path (rs, *root))) // Inside installation root.
          {
            p = chroot_path (rs, p);
            try
            {
              p = p.relative (md);
            }
            catch (const invalid_path&)
            {
              fail << "unable to make filesystem entry path " << p
                   << " relative to " << md <<
                info << "required for relocatable installation manifest";
            }
          }
        }
      }

      return p;
    }

    // Serialize current target and, if tgt is not NULL, start the new target.
    //
    // Note that we always serialize directories as top-level entries. And
    // theoretically we can end up "splitting" a target with a directory
    // creation. For example, if some files that belong to the target are
    // installed into subdirectories that have not yet been created. So we
    // have to cache the information for the current target in memory and only
    // flush it once we see the next target (or the end).
    //
    // You may be wondering why not just serialize directories as target
    // entries. While we could do that, it's not quite correct conceptually,
    // since this would be the first of potentially many targets that caused
    // the directory's creation. To put it another way, while files and
    // symlinks belong to tragets, directories do not.
    //
    static void
    manifest_flush_target (context_data& d, const target* tgt)
    {
      if (d.manifest_target != nullptr)
      {
        assert (!d.manifest_target_entries.empty ());

        // Target name format is the same as in the structured result output.
        //
        ostringstream os;
        stream_verb (os, stream_verbosity (1, 0));
        os << *d.manifest_target;

        try
        {
          auto& s (d.manifest_json);

          s.begin_object ();
          s.member ("type", "target");
          s.member ("name", os.str ());
          s.member_name ("entries");
          s.begin_array ();

          for (const auto& e: d.manifest_target_entries)
          {
            path p (relocatable_path (d, *d.manifest_target, move (e.path)));

            s.begin_object ();

            if (e.target.empty ())
            {
              s.member ("type", "file");
              s.member ("path", p.string ());
              s.member ("mode", e.mode);
            }
            else
            {
              s.member ("type", "symlink");
              s.member ("path", p.string ());
              s.member ("target", e.target.string ());
            }

            s.end_object ();
          }

          s.end_array ();  // entries member
          s.end_object (); // target object
        }
        catch (const json::invalid_json_output& e)
        {
          fail << "invalid " << d.manifest_name << " json output: " << e;
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << d.manifest_name << ": " << e;
        }

        d.manifest_target_entries.clear ();
      }

      d.manifest_target = tgt;
    }

    void context_data::
    manifest_install_d (context& ctx,
                        const target& tgt,
                        const dir_path& dir,
                        const string& mode)
    {
      auto& d (*static_cast<context_data*> (ctx.current_inner_odata.get ()));

      if (d.manifest_name.path != nullptr)
      {
        try
        {
          auto& s (d.manifest_json);

          // If we moved to the next target, flush the current one.
          //
          if (d.manifest_target != &tgt)
            manifest_flush_target (d, nullptr);

          s.begin_object ();
          s.member ("type", "directory");
          s.member ("path", relocatable_path (d, tgt, dir).string ());
          s.member ("mode", mode);
          s.end_object ();
        }
        catch (const json::invalid_json_output& e)
        {
          fail << "invalid " << d.manifest_name << " json output: " << e;
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << d.manifest_name << ": " << e;
        }
      }
    }

    void context_data::
    manifest_install_f (context& ctx,
                        const target& tgt,
                        const dir_path& dir,
                        const path& name,
                        const string& mode)
    {
      auto& d (*static_cast<context_data*> (ctx.current_inner_odata.get ()));

      if (d.manifest_name.path != nullptr)
      {
        if (d.manifest_target != &tgt)
          manifest_flush_target (d, &tgt);

        d.manifest_target_entries.push_back (
          manifest_target_entry {dir / name, mode, path ()});
      }
    }

    void context_data::
    manifest_install_l (context& ctx,
                        const target& tgt,
                        const path& link_target,
                        const dir_path& dir,
                        const path& link)
    {
      auto& d (*static_cast<context_data*> (ctx.current_inner_odata.get ()));

      if (d.manifest_name.path != nullptr)
      {
        if (d.manifest_target != &tgt)
          manifest_flush_target (d, &tgt);

        d.manifest_target_entries.push_back (
          manifest_target_entry {dir / link, "", link_target});
      }
    }

    static void
    manifest_close (context& ctx)
    {
      auto& d (*static_cast<context_data*> (ctx.current_inner_odata.get ()));

      if (d.manifest_name.path != nullptr)
      {
        try
        {
          manifest_flush_target (d, nullptr);

          d.manifest_os << '\n'; // Final newline.

          if (d.manifest_ofs.is_open ())
          {
            d.manifest_ofs.close ();
            d.manifest_autorm.cancel ();
          }
        }
        catch (const json::invalid_json_output& e)
        {
          fail << "invalid " << d.manifest_name << " json output: " << e;
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << d.manifest_name << ": " << e;
        }
      }
    }
#else
    context_data::
    context_data (const install::filters* fs, const path*)
        : filters (fs)
    {
    }

    void context_data::
    manifest_install_d (context&,
                        const target&,
                        const dir_path&,
                        const string&)
    {
    }

    void context_data::
    manifest_install_f (context&,
                        const target&,
                        const dir_path&,
                        const path&,
                        const string&)
    {
    }

    void context_data::
    manifest_install_l (context&,
                        const target&,
                        const path&,
                        const dir_path&,
                        const path&)
    {
    }

    static void
    manifest_close (context&)
    {
    }
#endif

    static operation_id
    pre_install (context&,
                 const values&,
                 meta_operation_id mo,
                 const location&)
    {
      // Run update as a pre-operation, unless we are disfiguring.
      //
      return mo != disfigure_id ? update_id : 0;
    }

    static operation_id
    pre_uninstall (context&,
                   const values&,
                   meta_operation_id mo,
                   const location&)
    {
      // Run update as a pre-operation, unless we are disfiguring.
      //
      return mo != disfigure_id ? update_id : 0;
    }

    static void
    install_pre (context& ctx,
                 const values& params,
                 bool inner,
                 const location& l)
    {
      if (!params.empty ())
        fail (l) << "unexpected parameters for operation install";

      if (inner)
      {
        // See if we need to filter and/or write the installation manifest.
        //
        // Note: go straight for the public variable pool.
        //
        const filters* fs (
          cast_null<filters> (
            ctx.global_scope[*ctx.var_pool.find ("config.install.filter")]));

        const path* mf (
          cast_null<path> (
            ctx.global_scope[*ctx.var_pool.find ("config.install.manifest")]));

        // Note that we cannot calculate whether the manifest should use
        // relocatable (relative) paths once here since we don't know the
        // value of config.install.root.

        ctx.current_inner_odata = context::current_data_ptr (
          new context_data (fs, mf),
          [] (void* p) {delete static_cast<context_data*> (p);});
      }
    }

    static void
    uninstall_pre (context& ctx,
                   const values& params,
                   bool inner,
                   const location& l)
    {
      // Note: a subset of install_pre().
      //
      if (!params.empty ())
        fail (l) << "unexpected parameters for operation uninstall";

      if (inner)
      {
        const filters* fs (
          cast_null<filters> (
            ctx.global_scope[*ctx.var_pool.find ("config.install.filter")]));

        ctx.current_inner_odata = context::current_data_ptr (
          new context_data (fs, nullptr),
          [] (void* p) {delete static_cast<context_data*> (p);});
      }
    }

    static void
    install_post (context& ctx, const values&, bool inner)
    {
      if (inner)
        manifest_close (ctx);
    }

    // Note that we run both install and uninstall serially. The reason for
    // this is all the fuzzy things we are trying to do like removing empty
    // outer directories if they are empty. If we do this in parallel, then
    // those things get racy. Also, since all we do here is creating/removing
    // files, there is not going to be much speedup from doing it in parallel.
    // There is also now the installation manifest, which relies on us
    // installing all the filesystem entries of a target serially.

    const operation_info op_install {
      install_id,
      0,
      "install",
      "install",
      "installing",
      "installed",
      "has nothing to install", // We cannot "be installed".
      execution_mode::first,
      0 /* concurrency */,      // Run serially.
      &pre_install,
      nullptr,
      &install_pre,
      &install_post,
      nullptr,
      nullptr
    };

    // Note that we run update as a pre-operation, just like install. Which
    // may seem bizarre at first. We do it to obtain the exact same dependency
    // graph as install so that we uninstall exactly the same set of files as
    // install would install. Note that just matching the rules without
    // executing them may not be enough: for example, a presence of an ad hoc
    // group member may only be discovered after executing the rule (e.g., VC
    // link.exe only creates a DLL's import library if there are any exported
    // symbols).
    //
    const operation_info op_uninstall {
      uninstall_id,
      0,
      "uninstall",
      "uninstall",
      "uninstalling",
      "uninstalled",
      "is not installed",
      execution_mode::last,
      0 /* concurrency */,      // Run serially
      &pre_uninstall,
      nullptr,
      &uninstall_pre,
      nullptr,
      nullptr,
      nullptr
    };

    // Also the explicit update-for-install operation alias.
    //
    const operation_info op_update_for_install {
      update_id, // Note: not update_for_install_id.
      install_id,
      op_update.name,
      op_update.name_do,
      op_update.name_doing,
      op_update.name_did,
      op_update.name_done,
      op_update.mode,
      op_update.concurrency,
      op_update.pre_operation,
      op_update.post_operation,
      op_update.operation_pre,
      op_update.operation_post,
      op_update.adhoc_match,
      op_update.adhoc_apply
    };
  }
}
