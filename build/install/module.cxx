// file      : build/install/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/install/module>

#include <build/scope>
#include <build/target>
#include <build/rule>
#include <build/diagnostics>

#include <build/config/utility>

#include <build/install/operation>

using namespace std;
using namespace butl;

namespace build
{
  namespace install
  {
    // Set install.<name> values based on config.install.<name> values
    // or the defaults.
    //
    static void
    set_dir (scope& r, const char* name, const char* path, const char* mode)
    {
      string vn ("config.install.");
      vn += name;

      const list_value* pv (config::optional<list_value> (r, vn));

      vn += ".mode";
      const list_value* mv (config::optional<list_value> (r, vn));

      vn = "install.";
      vn += name;
      auto p (r.assign (vn));

      if (pv != nullptr && !pv->empty ())
        p = *pv;
      else if (path != nullptr)
        p = path;

      vn += ".mode";
      auto m (r.assign (vn));

      if (mv != nullptr && !mv->empty ())
        p = *mv;
      else if (mode != nullptr)
        p = mode;
    }

    extern "C" void
    install_init (scope& root,
                  scope& base,
                  const location& l,
                  unique_ptr<build::module>&,
                  bool first)
    {
      tracer trace ("install::init");

      if (&root != &base)
        fail (l) << "install module must be initialized in bootstrap.build";

      if (!first)
      {
        warn (l) << "multiple install module initializations";
        return;
      }

      const dir_path& out_root (root.path ());
      level4 ([&]{trace << "for " << out_root;});

      // Register the install operation.
      //
      operation_id install_id (root.operations.insert (install));

      {
        auto& rs (base.rules);

        // Register the standard alias rule for the install operation.
        //
        rs.insert<alias> (install_id, "alias", alias_rule::instance);
      }

      // Configuration.
      //
      // Note that we don't use any defaults -- the location must
      // be explicitly specified or the installer will complain if
      // and when we try to install.
      //
      if (first)
      {
        set_dir (root, "root", nullptr, nullptr);
        set_dir (root, "data_root", "root", "644");
        set_dir (root, "exec_root", "root", "755");

        set_dir (root, "bin", "exec_root/bin", nullptr);
        set_dir (root, "sbin", "exec_root/sbin", nullptr);
      }
    }
  }
}
