// file      : build/install/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/install/module>

#include <build/scope>
#include <build/target>
#include <build/rule>
#include <build/operation>
#include <build/diagnostics>

#include <build/config/utility>

#include <build/install/rule>
#include <build/install/utility>
#include <build/install/operation>

using namespace std;
using namespace butl;

namespace build
{
  namespace install
  {
    // Set install.<name>.* values based on config.install.<name>.* ones
    // or the defaults. If none of config.install.* values were specified,
    // then we do omitted/delayed configuration. Note that we still need
    // to set all the install.* values to defaults, as if we had the
    // default configuration.
    //
    static void
    set_dir (bool spec,
             scope& r,
             const char* name,
             const char* path,
             const char* mode = nullptr,
             const char* dir_mode = nullptr,
             const char* cmd = nullptr,
             const char* options = nullptr)
    {
      auto set = [spec, &r, name] (const char* var, const char* dv)
        {
          string vn;
          const list_value* lv (nullptr);

          if (spec)
          {
            vn = "config.install.";
            vn += name;
            vn += var;

            lv = dv != nullptr
              ? &config::required (r, vn, list_value (dv)).first
              : config::optional<list_value> (r, vn);
          }

          vn = "install.";
          vn += name;
          vn += var;
          auto v (r.assign (vn));

          if (spec)
          {
            if (lv != nullptr && !lv->empty ())
              v = *lv;
          }
          else
          {
            if (dv != nullptr)
              v = dv;
          }
        };

      set ("", path);
      set (".mode", mode);
      set (".dir_mode", dir_mode);
      set (".cmd", cmd);
      set (".options", options);
    }

    static rule rule_;

    extern "C" void
    install_init (scope& r,
                  scope& b,
                  const location& l,
                  unique_ptr<build::module>&,
                  bool first)
    {
      tracer trace ("install::init");

      if (&r != &b)
        fail (l) << "install module must be initialized in bootstrap.build";

      if (!first)
      {
        warn (l) << "multiple install module initializations";
        return;
      }

      const dir_path& out_root (r.path ());
      level4 ([&]{trace << "for " << out_root;});

      // Register the install operation.
      //
      assert (r.operations.insert (install) == install_id);

      {
        auto& rs (b.rules);

        // Register the standard alias rule for the install operation.
        //
        rs.insert<alias> (install_id, "alias", alias_rule::instance);

        // Register our file installer rule.
        //
        rs.insert<file> (install_id, "install", rule_);
      }

      // Configuration.
      //
      // Note that we don't use any defaults for root -- the location
      // must be explicitly specified or the installer will complain
      // if and when we try to install.
      //
      if (first)
      {
        bool s (config::specified (r, "config.install"));
        const string& n (r["project"].as<const string&> ());

        set_dir (s, r, "root",      nullptr, nullptr, "755", "install");
        set_dir (s, r, "data_root", "root",  "644");
        set_dir (s, r, "exec_root", "root",  "755");

        set_dir (s, r, "sbin",      "exec_root/sbin");
        set_dir (s, r, "bin",       "exec_root/bin");
        set_dir (s, r, "lib",       "exec_root/lib");
        set_dir (s, r, "libexec",  ("exec_root/libexec/" + n).c_str ());

        set_dir (s, r, "data",     ("data_root/share/" + n).c_str ());
        set_dir (s, r, "include",   "data_root/include");

        set_dir (s, r, "doc",      ("data_root/share/doc/" + n).c_str ());
        set_dir (s, r, "man",       "data_root/share/man");

        set_dir (s, r, "man1",      "man/man1");
      }

      // Configure "installability" for built-in target types.
      //
      path<doc> (b, "doc");  // Install into install.doc.
      path<man> (b, "man");  // Install into install.man.
      path<man> (b, "man1"); // Install into install.man1.
    }
  }
}
