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
#ifndef BUILD2_BOOTSTRAP
    context_data::
    context_data (const path* mf)
        : manifest_name (mf),
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
    context_data (const path*)
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
        // See if we need to write the installation manifest.
        //
        // Note: go straight for the public variable pool.
        //
        const path* mf (
          cast_null<path> (
            ctx.global_scope[*ctx.var_pool.find ("config.install.manifest")]));

        // Note that we cannot calculate whether the manifest should use
        // relocatable (relative) paths once here since we don't know the
        // value of config.install.root.

        ctx.current_inner_odata = context::current_data_ptr (
          new context_data (mf),
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
      nullptr,
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
