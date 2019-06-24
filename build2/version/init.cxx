// file      : build2/version/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/version/init.hxx>

#include <libbutl/manifest-parser.mxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <build2/config/utility.hxx>

#include <build2/dist/module.hxx>

#include <build2/version/rule.hxx>
#include <build2/version/module.hxx>
#include <build2/version/utility.hxx>
#include <build2/version/snapshot.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace version
  {
    static const path manifest_file ("manifest");

    static const in_rule in_rule_;
    static const manifest_install_rule manifest_install_rule_;

    bool
    boot (scope& rs, const location& l, unique_ptr<module_base>& mod)
    {
      tracer trace ("version::boot");
      l5 ([&]{trace << "for " << rs;});

      // Extract the version from the manifest file. As well as summary and
      // url while at it.
      //
      // Also, as a sanity check, verify the package name matches the build
      // system project name.
      //
      string sum;
      string url;

      standard_version v;
      dependencies ds;
      {
        path f (rs.src_path () / manifest_file);

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
            if (nv.name == "name")
            {
              auto& pn (cast<project_name> (rs.vars[var_project]));

              if (nv.value != pn.string ())
              {
                path bf (rs.src_path () / rs.root_extra->bootstrap_file);
                location ml (&f, nv.value_line, nv.value_column);
                location bl (&bf);

                fail (ml) << "package name " << nv.value << " does not match "
                          << "build system project name " << pn <<
                  info (bl) << "build system project name specified here";
              }
            }
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

                p = d.find_first_of (" \t=<>[(~^");
                string n (d, 0, p);
                string c (p != string::npos ? string (d, p) : string ());

                trim (n);
                trim (c);

                try
                {
                  package_name pn (move (n));
                  string v (pn.variable ());

                  ds.emplace (move (v), dependency {move (pn), move (c)});
                }
                catch (const invalid_argument& e)
                {
                  fail (l) << "invalid package name for dependency "
                           << d << ": " << e;
                }
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
      // snapshot number and id (e.g., commit date and id from git).
      //
      bool committed (true);
      bool rewritten (false);
      if (v.snapshot () && v.snapshot_sn == standard_version::latest_sn)
      {
        snapshot ss (extract_snapshot (rs));

        if (!ss.empty ())
        {
          v.snapshot_sn = ss.sn;
          v.snapshot_id = move (ss.id);
          committed = ss.committed;
          rewritten = true;
        }
        else
          committed = false;
      }

      // If there is a dependency on the build system itself, check it (so
      // there is no need for explicit using build@X.Y.Z).
      //
      {
        auto i (ds.find ("build2"));

        if (i != ds.end () && !i->second.constraint.empty ())
        try
        {
          check_build_version (
            standard_version_constraint (i->second.constraint, v), l);
        }
        catch (const invalid_argument& e)
        {
          fail (l) << "invalid version constraint for dependency build2 "
                   << i->second.constraint << ": " << e;
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

      optional<uint16_t> a (v.alpha ());
      optional<uint16_t> b (v.beta ());

      set ("version.alpha",              a.has_value ());
      set ("version.beta",               b.has_value ());
      set ("version.pre_release",        v.pre_release ().has_value ());
      set ("version.pre_release_string", v.string_pre_release ());
      set ("version.pre_release_number", uint64_t (a ? *a : b ? *b : 0));

      set ("version.snapshot",           v.snapshot ()); // bool
      set ("version.snapshot_sn",        v.snapshot_sn); // uint64
      set ("version.snapshot_id",        v.snapshot_id); // string
      set ("version.snapshot_string",    v.string_snapshot ());
      set ("version.snapshot_committed", committed);     // bool

      set ("version.revision", uint64_t (v.revision));

      // Create the module.
      //
      mod.reset (new module (cast<project_name> (rs.vars[var_project]),
                             move (v),
                             committed,
                             rewritten,
                             move (ds)));

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

      // Load in.base (in.* varibales, in{} target type).
      //
      if (!cast_false<bool> (rs["in.base.loaded"]))
        load_module (rs, rs, "in.base", l);

      module& m (static_cast<module&> (*mod));
      const standard_version& v (m.version);

      // If the dist module is used, set its dist.package and register the
      // post-processing callback.
      //
      if (auto* dm = rs.lookup_module<dist::module> (dist::module::name))
      {
        // Make sure dist is init'ed, not just boot'ed.
        //
        if (!cast_false<bool> (rs["dist.loaded"]))
          load_module (rs, rs, "dist", l);

        m.dist_uncommitted = cast_false<bool> (rs["config.dist.uncommitted"]);

        // Don't touch if dist.package was set by the user.
        //
        value& val (rs.assign (dm->var_dist_package));

        if (!val)
        {
          string p (cast<project_name> (rs.vars[var_project]).string ());
          p += '-';
          p += v.string ();
          val = move (p);

          // Only register the post-processing callback if this is a rewritten
          // snapshot.
          //
          if (m.rewritten)
            dm->register_callback (dir_path (".") / manifest_file,
                                   &dist_callback,
                                   &m);
        }
      }

      // Register rules.
      //
      {
        auto& r (rs.rules);

        r.insert<file> (perform_update_id,   "version.in", in_rule_);
        r.insert<file> (perform_clean_id,    "version.in", in_rule_);
        r.insert<file> (configure_update_id, "version.in", in_rule_);

        if (cast_false<bool> (rs["install.booted"]))
        {
          r.insert<manifest> (
            perform_install_id, "version.manifest", manifest_install_rule_);
        }
      }

      return true;
    }

    static void
    dist_callback (const path& f, const scope& rs, void* data)
    {
      module& m (*static_cast<module*> (data));

      // Complain if this is an uncommitted snapshot.
      //
      if (!m.committed && !m.dist_uncommitted)
        fail << "distribution of uncommitted project " << rs.src_path () <<
          info << "specify config.dist.uncommitted=true to force";

      // The plan is simple: fixing up the version in a temporary file then
      // move it to the original.
      //
      try
      {
        auto_rmfile t (fixup_manifest (f,
                                       path::temp_path ("manifest"),
                                       m.version));

        mvfile (t.path, f, (cpflags::overwrite_content |
                            cpflags::overwrite_permissions));
        t.cancel ();
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
