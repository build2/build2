// file      : build2/version/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/version/init.hxx>

#include <libbutl/manifest-parser.mxx>
#include <libbutl/manifest-serializer.mxx>

#include <build2/scope.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>
#include <build2/diagnostics.hxx>

#include <build2/dist/module.hxx>

#include <build2/version/rule.hxx>
#include <build2/version/module.hxx>
#include <build2/version/snapshot.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace version
  {
    static const path manifest ("manifest");

    static const doc_rule doc_rule_;
    static const in_rule in_rule_;

    bool
    boot (scope& rs, const location& l, unique_ptr<module_base>& mod)
    {
      tracer trace ("version::boot");
      l5 ([&]{trace << "for " << rs.out_path ();});

      // Extract the version from the manifest file. As well as summary and
      // url while at it.
      //
      string sum;
      string url;

      standard_version v;
      dependency_constraints ds;
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
            if (nv.name == "summary")
              sum = move (nv.value);
            else if (nv.name == "url")
              url = move (nv.value);
            else if (nv.name == "version")
            {
              try
              {
                // Allow the package stub versions in the 0+<revision> form.
                // While not standard, we want to use the version module for
                // packaging stubs.
                //
                v = standard_version (nv.value, standard_version::allow_stub);
              }
              catch (const invalid_argument& e)
              {
                fail << "invalid standard version '" << nv.value << "': " << e;
              }
            }
            else if (nv.name == "depends")
            {
              // According to the package manifest spec, the format of the
              // 'depends' value is as follows:
              //
              // depends: [?][*] <alternatives> [; <comment>]
              //
              // <alternatives> := <dependency> [ '|' <dependency>]*
              // <dependency>   := <name> [<constraint>]
              // <constraint>   := <comparison> | <range>
              // <comparison>   := ('==' | '>' | '<' | '>=' | '<=') <version>
              // <range>        := ('(' | '[') <version> <version> (')' | ']')
              //
              // Note that we don't do exhaustive validation here leaving it
              // to the package manager.
              //
              string v (move (nv.value));

              size_t p;

              // Get rid of the comment.
              //
              if ((p = v.find (';')) != string::npos)
                v.resize (p);

              // Get rid of conditional/runtime markers. Note that enither of
              // them is valid in the rest of the value.
              //
              if ((p = v.find_last_of ("?*")) != string::npos)
                v.erase (0, p + 1);

              // Parse as |-separated "words".
              //
              for (size_t b (0), e (0); next_word (v, b, e, '|'); )
              {
                string d (v, b, e - b);
                trim (d);

                p = d.find_first_of (" \t=<>[(");
                string n (d, 0, p);
                string c (p != string::npos ? string (d, p) : string ());

                trim (n);
                trim (c);

                // If this is a dependency on the build system itself, check
                // it (so there is no need for explicit using build@X.Y.Z).
                //
                if (n == "build2" && !c.empty ())
                try
                {
                  check_build_version (standard_version_constraint (c), l);
                }
                catch (const invalid_argument& e)
                {
                  fail (l) << "invalid version constraint for dependency "
                           << b << ": " << e;
                }

                ds.emplace (move (n), move (c));
              }
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
      if (v.snapshot () && v.snapshot_sn == standard_version::latest_sn)
      {
        snapshot ss (extract_snapshot (rs));

        if (!ss.empty ())
        {
          v.snapshot_sn = ss.sn;
          v.snapshot_id = move (ss.id);
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

      if (!sum.empty ()) rs.assign (var_project_summary) = move (sum);
      if (!url.empty ()) rs.assign (var_project_url)     = move (url);

      set ("version", v.string ());  // Project version (var_version).

      set ("version.project",        v.string_project ());
      set ("version.project_number", v.version);

      // Enough of project version for unique identification (can be used in
      // places like soname, etc).
      //
      set ("version.project_id",     v.string_project_id ());

      set ("version.stub", v.stub ()); // bool

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
      mod.reset (new module (move (v), move (ds)));

      return true; // Init first (dist.package, etc).
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

      // Enter variables.
      //
      {
        auto& vp (var_pool.rw (rs));

        // @@ Note: these should be moved to the 'in' module once we have it.
        //

        // Alternative variable substitution symbol.
        //
        m.in_symbol = &vp.insert<string> ("in.symbol");

        // Substitution mode. Valid values are 'strict' (default) and 'lax'.
        // In the strict mode every substitution symbol is expected to start a
        // substitution with the double symbol (e.g., $$) serving as an
        // escape sequence.
        //
        // In the lax mode a pair of substitution symbols is only treated as a
        // substitution if what's between them looks like a build2 variable
        // name (i.e., doesn't contain spaces, etc). Everything else,
        // including unterminated substitution symbols is copied as is. Note
        // also that in this mode the double symbol is not treated as an
        // escape sequence.
        //
        // The lax mode is mostly useful when trying to reuse existing .in
        // files, for example from autoconf. Note, however, that the lax mode
        // is still stricter than the autoconf's semantics which also leaves
        // unknown substitutions as is.
        //
        m.in_substitution = &vp.insert<string> ("in.substitution");
      }

      // Register rules.
      //
      {
        auto& r (rs.rules);

        r.insert<doc> (perform_update_id,   "version.doc", doc_rule_);
        r.insert<doc> (perform_clean_id,    "version.doc", doc_rule_);
        r.insert<doc> (configure_update_id, "version.doc", doc_rule_);

        r.insert<file> (perform_update_id,   "version.in", in_rule_);
        r.insert<file> (perform_clean_id,    "version.in", in_rule_);
        r.insert<file> (configure_update_id, "version.in", in_rule_);
      }

      return true;
    }

    static void
    dist_callback (const path& f, const scope& rs, void* data)
    {
      module& m (*static_cast<module*> (data));
      const standard_version& v (m.version);

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
