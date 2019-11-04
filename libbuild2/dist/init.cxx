// file      : libbuild2/dist/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/dist/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/file.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/dist/rule.hxx>
#include <libbuild2/dist/module.hxx>
#include <libbuild2/dist/operation.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace dist
  {
    static const rule rule_;

    bool
    boot (scope& rs, const location&, unique_ptr<module_base>& mod)
    {
      tracer trace ("dist::boot");

      l5 ([&]{trace << "for " << rs;});

      // Register the meta-operation.
      //
      rs.insert_meta_operation (dist_id, mo_dist);

      // Enter module variables. Do it during boot in case they get assigned
      // in bootstrap.build (which is customary for, e.g., dist.package).
      //
      auto& vp (rs.ctx.var_pool.rw (rs));

      // Note: some overridable, some not.
      //
      // config.dist.archives is a list of archive extensions (e.g., zip,
      // tar.gz) that can be optionally prefixed with a directory. If it is
      // relative, then it is prefixed with config.dist.root. Otherwise, the
      // archive is written to the absolute location.
      //
      // config.dist.checksums is a list of archive checksum extensions (e.g.,
      // sha1, sha256) that can also be optionally prefixed with a directory
      // with the same semantics as config.dist.archives. If the directory is
      // absent, then the checksum file is written into the same directory as
      // the corresponding archive.
      //
      vp.insert<abs_dir_path> ("config.dist.root",      true);
      vp.insert<paths>        ("config.dist.archives",  true);
      vp.insert<paths>        ("config.dist.checksums", true);
      vp.insert<path>         ("config.dist.cmd",       true);

      // Allow distribution of uncommitted projects. This is enforced by the
      // version module.
      //
      vp.insert<bool> ("config.dist.uncommitted", true);

      vp.insert<dir_path>     ("dist.root");
      vp.insert<process_path> ("dist.cmd");
      vp.insert<paths>        ("dist.archives");
      vp.insert<paths>        ("dist.checksums");
      vp.insert<paths>        ("dist.uncommitted");

      vp.insert<bool> ("dist", variable_visibility::target); // Flag.

      // Project's package name.
      //
      auto& v_d_p (
        vp.insert<string> ("dist.package", variable_visibility::project));

      // Create the module.
      //
      mod.reset (new module (v_d_p));

      return false;
    }

    bool
    init (scope& rs,
          scope&,
          const location& l,
          unique_ptr<module_base>&,
          bool first,
          bool,
          const variable_map& config_hints)
    {
      tracer trace ("dist::init");

      if (!first)
      {
        warn (l) << "multiple dist module initializations";
        return true;
      }

      l5 ([&]{trace << "for " << rs;});

      assert (config_hints.empty ()); // We don't known any hints.

      // Register our wildcard rule. Do it explicitly for the alias to prevent
      // something like insert<target>(dist_id, test_id) taking precedence.
      //
      rs.rules.insert<target> (dist_id, 0, "dist", rule_);
      rs.rules.insert<alias> (dist_id, 0, "dist.alias", rule_); //@@ outer?

      // Configuration.
      //
      // Note that we don't use any defaults for root -- the location
      // must be explicitly specified or we will complain if and when
      // we try to dist.
      //
      bool s (config::specified (rs, "dist"));

      // Adjust module priority so that the config.dist.* values are saved at
      // the end of config.build.
      //
      if (s)
        config::save_module (rs, "dist", INT32_MAX);

      // dist.root
      //
      {
        value& v (rs.assign ("dist.root"));

        if (s)
        {
          if (lookup l = config::optional (rs, "config.dist.root"))
            v = cast<dir_path> (l); // Strip abs_dir_path.
        }
      }

      // dist.cmd
      //
      {
        value& v (rs.assign<process_path> ("dist.cmd"));

        if (s)
        {
          if (lookup l = config::required (rs,
                                           "config.dist.cmd",
                                           path ("install")).first)
            v = run_search (cast<path> (l), true);
        }
      }

      // dist.archives
      // dist.checksums
      //
      {
        value& a (rs.assign ("dist.archives"));
        value& c (rs.assign ("dist.checksums"));

        if (s)
        {
          if (lookup l = config::optional (rs, "config.dist.archives"))
            a = *l;

          if (lookup l = config::optional (rs, "config.dist.checksums"))
          {
            c = *l;

            if (!c.empty () && (!a || a.empty ()))
              fail << "config.dist.checksums specified without "
                   << "config.dist.archives";

          }
        }
      }

      // dist.uncommitted
      //
      // Omit it from the configuration unless specified.
      //
      config::omitted (rs, "config.dist.uncommitted");

      return true;
    }

    static const module_functions mod_functions[] =
    {
      {"dist",  &boot,   &init},
      {nullptr, nullptr, nullptr}
    };

    const module_functions*
    build2_dist_load ()
    {
      return mod_functions;
    }
  }
}
