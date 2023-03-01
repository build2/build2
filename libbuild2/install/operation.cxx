// file      : libbuild2/install/operation.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/install/operation.hxx>

#include <sstream>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace install
  {
#ifndef BUILD2_BOOTSTRAP
    install_context_data::
    install_context_data (const path* mf)
        : manifest_file (mf),
          manifest_os (mf != nullptr
                       ? open_file_or_stdout (manifest_file, manifest_ofs)
                       : manifest_ofs),
          manifest_autorm (manifest_ofs.is_open () ? *mf : path ()),
          manifest_json (manifest_os, 0 /* indentation */)
    {
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
    manifest_flush_target (install_context_data& d, const target* tgt)
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
            s.begin_object ();

            if (e.target.empty ())
            {
              s.member ("type", "file");
              s.member ("path", e.path);
              s.member ("mode", e.mode);
            }
            else
            {
              s.member ("type", "symlink");
              s.member ("path", e.path);
              s.member ("target", e.target);
            }

            s.end_object ();
          }

          s.end_array ();  // entries member
          s.end_object (); // target object
        }
        catch (const json::invalid_json_output& e)
        {
          fail << "invalid " << d.manifest_file << " json output: " << e;
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << d.manifest_file << ": " << e;
        }

        d.manifest_target_entries.clear ();
      }

      d.manifest_target = tgt;
    }

    void install_context_data::
    manifest_install_d (context& ctx,
                        const target& tgt,
                        const dir_path& dir,
                        const string& mode)
    {
      auto& d (
        *static_cast<install_context_data*> (ctx.current_inner_odata.get ()));

      if (d.manifest_file.path != nullptr)
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
          s.member ("path", dir.string ());
          s.member ("mode", mode);
          s.end_object ();
        }
        catch (const json::invalid_json_output& e)
        {
          fail << "invalid " << d.manifest_file << " json output: " << e;
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << d.manifest_file << ": " << e;
        }
      }
    }

    void install_context_data::
    manifest_install_f (context& ctx,
                        const target& tgt,
                        const dir_path& dir,
                        const path& name,
                        const string& mode)
    {
      auto& d (
        *static_cast<install_context_data*> (ctx.current_inner_odata.get ()));

      if (d.manifest_file.path != nullptr)
      {
        if (d.manifest_target != &tgt)
          manifest_flush_target (d, &tgt);

        d.manifest_target_entries.push_back (
          manifest_target_entry {(dir / name).string (), mode, ""});
      }
    }

    void install_context_data::
    manifest_install_l (context& ctx,
                        const target& tgt,
                        const path& link_target,
                        const dir_path& dir,
                        const path& link)
    {
      auto& d (
        *static_cast<install_context_data*> (ctx.current_inner_odata.get ()));

      if (d.manifest_file.path != nullptr)
      {
        if (d.manifest_target != &tgt)
          manifest_flush_target (d, &tgt);

        d.manifest_target_entries.push_back (
          manifest_target_entry {
            (dir / link).string (), "", link_target.string ()});
      }
    }

    static void
    manifest_close (context& ctx)
    {
      auto& d (
        *static_cast<install_context_data*> (ctx.current_inner_odata.get ()));

      if (d.manifest_file.path != nullptr)
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
          fail << "invalid " << d.manifest_file << " json output: " << e;
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << d.manifest_file << ": " << e;
        }
      }
    }
#else
    install_context_data::
    install_context_data (const path*)
    {
    }

    void install_context_data::
    manifest_install_d (context&,
                        const target&,
                        const dir_path&,
                        const string&)
    {
    }

    void install_context_data::
    manifest_install_f (context&,
                        const target&,
                        const dir_path&,
                        const path&,
                        const string&)
    {
    }

    void install_context_data::
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
        const variable& var (*ctx.var_pool.find ("config.install.manifest"));
        const path* mf (cast_null<path> (ctx.global_scope[var]));

        ctx.current_inner_odata = context::current_data_ptr (
          new install_context_data (mf),
          [] (void* p) {delete static_cast<install_context_data*> (p);});
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
