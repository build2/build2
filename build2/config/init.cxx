// file      : build2/config/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/config/init.hxx>

#include <build2/file.hxx>
#include <build2/rule.hxx>
#include <build2/scope.hxx>
#include <build2/context.hxx>
#include <build2/filesystem.hxx>  // exists()
#include <build2/diagnostics.hxx>

#include <build2/config/module.hxx>
#include <build2/config/utility.hxx>
#include <build2/config/operation.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace config
  {
    void
    boot (scope& rs, const location& loc, unique_ptr<module_base>& mod)
    {
      tracer trace ("config::boot");

      const dir_path& out_root (rs.out_path ());
      l5 ([&]{trace << "for " << out_root;});

      const string& mname (*current_mname);
      const string& oname (*current_oname);

      // Only create the module if we are configuring or creating. This is a
      // bit tricky since the build2 core may not yet know if this is the
      // case. But we know.
      //
      if ((                   mname == "configure" || mname == "create") ||
          (mname.empty () && (oname == "configure" || oname == "create")))
      {
        unique_ptr<module> m (new module);

        // Adjust priority for the import pseudo-module so that
        // config.import.* values come first in config.build.
        //
        m->save_module ("import", INT32_MIN);

        mod = move (m);
      }

      // Register meta-operations. Note that we don't register create_id
      // since it will be pre-processed into configure.
      //
      rs.meta_operations.insert (configure_id, mo_configure);
      rs.meta_operations.insert (disfigure_id, mo_disfigure);

      auto& vp (var_pool.rw (rs));

      // utility.cxx:unconfigured() (note: not overridable).
      //
      vp.insert_pattern<bool> (
        "config.*.configured", false, variable_visibility::normal);

      // Load config.build if one exists.
      //
      // Note that we have to do this during bootstrap since the order in
      // which the modules will be initialized is unspecified. So it is
      // possible that some module which needs the configuration will get
      // called first.
      //
      const variable& c_v (vp.insert<uint64_t> ("config.version", false));

      // Don't load it if we are disfiguring. The same situation as with
      // module loading above.
      //
      if (mname != "disfigure" && (!mname.empty () || oname != "disfigure"))
      {
        path f (out_root / config_file);

        if (exists (f))
        {
          // Check the config version. We assume that old versions cannot
          // understand new configs and new versions are incompatible with old
          // configs.
          //
          // We extract the value manually instead of loading and then
          // checking in order to be able to fixup/migrate the file which we
          // may want to do in the future.
          //
          {
            // Assume missing version is 0.
            //
            auto p (extract_variable (rs, f, c_v));
            uint64_t v (p.second ? cast<uint64_t> (p.first) : 0);

            if (v != module::version)
              fail (loc) << "incompatible config file " << f <<
                info << "config file version   " << v
                         << (p.second ? "" : " (missing)") <<
                info << "config module version " << module::version <<
                info << "consider reconfiguring " << project (rs) << '@'
                         << out_root;
          }

          source (rs, rs, f);
        }
      }
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
      tracer trace ("config::init");

      if (!first)
      {
        warn (l) << "multiple config module initializations";
        return true;
      }

      l5 ([&]{trace << "for " << rs.out_path ();});

      assert (config_hints.empty ()); // We don't known any hints.

      // Register alias and fallback rule for the configure meta-operation.
      //
      {
        // We need this rule for out-of-any-project dependencies (e.g.,
        // libraries imported from /usr/lib). Registring it on the global
        // scope smells a bit but seems harmless.
        //
        rs.global ().rules.insert<mtime_target> (
          configure_id, 0, "config.file", file_rule::instance);

        auto& r (rs.rules);

        r.insert<target> (configure_id, 0, "config", fallback_rule::instance);
        r.insert<file> (configure_id, 0, "config.file", fallback_rule::instance);
        r.insert<alias> (configure_id, 0, "config.alias", alias_rule::instance);
      }

      return true;
    }
  }
}
