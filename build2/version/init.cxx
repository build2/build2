// file      : build2/version/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/version/init>

#include <butl/manifest-parser>
#include <butl/manifest-serializer>

#include <build2/scope>
#include <build2/context>
#include <build2/variable>
#include <build2/diagnostics>

#include <build2/dist/module>

#include <build2/version/rule>
#include <build2/version/module>
#include <build2/version/snapshot>

using namespace std;
using namespace butl;

namespace build2
{
  namespace version
  {
    static const path manifest ("manifest");

    static const version_doc version_doc_;

    void
    boot (scope& rs, const location& l, unique_ptr<module_base>& mod)
    {
      tracer trace ("version::boot");
      l5 ([&]{trace << "for " << rs.out_path ();});

      // Extract the version from the manifest file.
      //
      standard_version v;
      {
        path f (rs.src_path () / manifest);

        try
        {
          if (!file_exists (f))
            fail (l) << "no manifest file in " << rs.src_path ();

          ifdstream ifs (f);
          manifest_parser p (ifs, f.string ());

          manifest_name_value nv (p.next ());
          if (!nv.name.empty () || nv.value != "1")
            fail (l) << "unsupported manifest format in " << f;

          for (nv = p.next (); !nv.empty (); nv = p.next ())
          {
            if (nv.name == "version")
            {
              try
              {
                v = standard_version (nv.value);
              }
              catch (const invalid_argument& e)
              {
                fail << "invalid standard version '" << nv.value << "': " << e;
              }
              break;
            }
          }
        }
        catch (const manifest_parsing& e)
        {
          location l (&f, e.line, e.column);
          fail (l) << e.description;
        }
        catch (const io_error& e)
        {
          fail (l) << "unable to read from " << f << ": " << e;
        }
        catch (const system_error& e) // EACCES, etc.
        {
          fail (l) << "unable to access manifest " << f << ": " << e;
        }

        if (v.empty ())
          fail (l) << "no version in " << f;
      }

      // If this is the latest snapshot (i.e., the -a.1.z kind), then load the
      // snapshot sn and id (e.g., commit date and id from git). If there is
      // uncommitted stuff, then leave it as .z.
      //
      bool patched (false);
      if (v.snapshot () && v.snapshot_sn == standard_version::latest_sn)
      {
        snapshot ss (extract_snapshot (rs));

        if (!ss.empty ())
        {
          v.snapshot_sn = ss.sn;
          v.snapshot_id = move (ss.id);
          patched = true;
        }
      }

      // Set all the version.* variables.
      //
      auto& vp (var_pool.rw (rs));

      auto set = [&vp, &rs] (const char* var, auto val)
      {
        using T = decltype (val);
        auto& v (vp.insert<T> (var, variable_visibility::project));
        rs.assign (v) = move (val);
      };

      // Enough of project version for unique identification (can be used in
      // places like soname, etc).
      //
      string id (v.string_version ());
      if (v.snapshot ()) // Trailing dot already in id.
      {
        id += (v.snapshot_sn == standard_version::latest_sn
               ? "z"
               : (v.snapshot_id.empty ()
                  ? to_string (v.snapshot_sn):
                  v.snapshot_id));
      }

      set ("version", v.string ());         // Package version.

      set ("version.project",        v.string_project ());
      set ("version.project_number", v.version);
      set ("version.project_id",     move (id));

      set ("version.epoch", uint64_t (v.epoch));

      set ("version.major", uint64_t (v.major ()));
      set ("version.minor", uint64_t (v.minor ()));
      set ("version.patch", uint64_t (v.patch ()));

      set ("version.alpha",              v.alpha ()); // bool
      set ("version.beta",               v.beta ());  // bool
      set ("version.pre_release",        v.alpha () || v.beta ());
      set ("version.pre_release_string", v.string_pre_release ());
      set ("version.pre_release_number", uint64_t (v.pre_release ()));

      set ("version.snapshot",        v.snapshot ()); // bool
      set ("version.snapshot_sn",     v.snapshot_sn); // uint64
      set ("version.snapshot_id",     v.snapshot_id); // string
      set ("version.snapshot_string", v.string_snapshot ());

      set ("version.revision", uint64_t (v.revision));

      // Create the module.
      //
      mod.reset (new module (move (v), patched));
    }

    static void
    dist_callback (const path&, const scope&, void*);

    bool
    init (scope& rs,
          scope&,
          const location& l,
          unique_ptr<module_base>& mod,
          bool first,
          bool,
          const variable_map&)
    {
      tracer trace ("version::init");

      if (!first)
        fail (l) << "multiple version module initializations";

      module& m (static_cast<module&> (*mod));
      const standard_version& v (m.version);

      // If the dist module has been loaded, set its dist.package and register
      // the post-processing callback.
      //
      if (auto* dm = rs.modules.lookup<dist::module> (dist::module::name))
      {
        // Don't touch if dist.package was set by the user.
        //
        value& val (rs.assign (dm->var_dist_package));

        if (!val)
        {
          string p (cast<string> (rs.vars[var_project]));
          p += '-';
          p += v.string ();
          val = move (p);

          // Only register the post-processing callback if this a snapshot.
          //
          if (v.snapshot ())
            dm->register_callback (dir_path (".") / manifest,
                                   &dist_callback,
                                   &m);
        }
      }

      // Register rules.
      //
      {
        auto& r (rs.rules);

        r.insert<doc> (perform_update_id,   "version.doc", version_doc_);
        r.insert<doc> (perform_clean_id,    "version.doc", version_doc_);
        r.insert<doc> (configure_update_id, "version.doc", version_doc_);
      }

      return true;
    }

    static void
    dist_callback (const path& f, const scope& rs, void* data)
    {
      module& m (*static_cast<module*> (data));
      const standard_version v (m.version);

      // Complain if this is an uncommitted snapshot.
      //
      if (v.snapshot_sn == standard_version::latest_sn)
        fail << "distribution of uncommitted project " << rs.src_path ();

      // The plan is simple, re-serialize the manifest into a temporary file
      // fixing up the version. Then move the temporary file to the original.
      //
      path t;
      try
      {
        permissions perm (path_permissions (f));

        ifdstream ifs (f);
        manifest_parser p (ifs, f.string ());

        t = path::temp_path ("manifest");
        auto_fd ofd (fdopen (t,
                             fdopen_mode::out       |
                             fdopen_mode::create    |
                             fdopen_mode::exclusive |
                             fdopen_mode::binary,
                             perm));
        auto_rmfile arm (t); // Try to remove on failure ignoring errors.

        ofdstream ofs (move (ofd));
        manifest_serializer s (ofs, t.string ());

        manifest_name_value nv (p.next ());
        assert (nv.name.empty () && nv.value == "1"); // We just loaded it.
        s.next (nv.name, nv.value);

        for (nv = p.next (); !nv.empty (); nv = p.next ())
        {
          if (nv.name == "version")
            nv.value = v.string ();

          s.next (nv.name, nv.value);
        }

        s.next (nv.name, nv.value); // End of manifest.
        s.next (nv.name, nv.value); // End of stream.

        ofs.close ();
        ifs.close ();

        mvfile (t, f, (cpflags::overwrite_content |
                       cpflags::overwrite_permissions));
        arm.cancel ();
      }
      catch (const manifest_parsing& e)
      {
        location l (&f, e.line, e.column);
        fail (l) << e.description;
      }
      catch (const manifest_serialization& e)
      {
        location l (&t);
        fail (l) << e.description;
      }
      catch (const io_error& e)
      {
        fail << "unable to overwrite " << f << ": " << e;
      }
      catch (const system_error& e) // EACCES, etc.
      {
        fail << "unable to overwrite " << f << ": " << e;
      }
    }
  }
}
