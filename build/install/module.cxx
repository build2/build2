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
    // If override is true, then override values that came from outer
    // configurations. We have to do this for paths that contain the
    // package name.
    //
    template <typename T>
    static void
    set_var (bool spec,
             scope& r,
             const char* name,
             const char* var,
             const T* dv,
             bool override = false)
    {
      string vn;
      const value* cv (nullptr);

      if (spec)
      {
        vn = "config.install.";
        vn += name;
        vn += var;
        const variable& vr (
          var_pool.find (move (vn), &value_traits<T>::value_type));

        cv = dv != nullptr
          ? &config::required (r, vr, *dv, override).first.get ()
          : &config::optional (r, vr);
      }

      vn = "install.";
      vn += name;
      vn += var;
      const variable& vr (
        var_pool.find (move (vn), &value_traits<T>::value_type));

      value& v (r.assign (vr));

      if (spec)
      {
        if (*cv && !cv->empty ())
          v = *cv;
      }
      else
      {
        if (dv != nullptr)
          v = *dv;
      }
    }

    static void
    set_dir (bool s,
             scope& r,
             const char* name,
             const string& path,
             const string& fmode = string (),
             const string& dmode = string (),
             const string& cmd = string (),
             bool ovr = false)
    {
      dir_path dpath (path);
      set_var (s, r, name, "",          dpath.empty () ? nullptr : &dpath, ovr);
      set_var (s, r, name, ".mode",     fmode.empty () ? nullptr : &fmode);
      set_var (s, r, name, ".dir_mode", dmode.empty () ? nullptr : &dmode);
      set_var (s, r, name, ".cmd",      cmd.empty ()   ? nullptr : &cmd);
      set_var<strings> (s, r, name, ".options", nullptr);
    }

    static alias_rule alias_;
    static file_rule file_;

    extern "C" bool
    install_init (scope& r,
                  scope& b,
                  const location& l,
                  unique_ptr<module>&,
                  bool first,
                  bool)
    {
      tracer trace ("install::init");

      if (&r != &b)
        fail (l) << "install module must be initialized in bootstrap.build";

      if (!first)
      {
        warn (l) << "multiple install module initializations";
        return true;
      }

      const dir_path& out_root (r.out_path ());
      level5 ([&]{trace << "for " << out_root;});

      // Enter module variables.
      //
      // Note that the set_dir() calls below enter some more.
      //
      if (first)
      {
        auto& v (var_pool);

        v.find ("install", dir_path_type);
      }

      // Register the install operation.
      //
      r.operations.insert (install_id, install);

      // Register our alias and file installer rule.
      //
      b.rules.insert<alias> (perform_install_id, "install.alias", alias_);
      b.rules.insert<file> (perform_install_id, "install.file", file_);

      // Configuration.
      //
      // Note that we don't use any defaults for root -- the location
      // must be explicitly specified or the installer will complain
      // if and when we try to install.
      //
      if (first)
      {
        bool s (config::specified (r, "config.install"));
        const string& n (as<string> (*r["project"]));

        set_dir (s, r, "root",      "",      "", "755", "install");
        set_dir (s, r, "data_root", "root",  "644");
        set_dir (s, r, "exec_root", "root",  "755");

        set_dir (s, r, "sbin",    "exec_root/sbin");
        set_dir (s, r, "bin",     "exec_root/bin");
        set_dir (s, r, "lib",     "exec_root/lib");
        set_dir (s, r, "libexec", "exec_root/libexec/" + n, "", "", "", true);

        set_dir (s, r, "data",    "data_root/share/" + n, "", "", "", true);
        set_dir (s, r, "include", "data_root/include");

        set_dir (s, r, "doc",     "data_root/share/doc/" + n, "", "", "", true);
        set_dir (s, r, "man",     "data_root/share/man");

        set_dir (s, r, "man1",    "man/man1");
      }

      // Configure "installability" for built-in target types.
      //
      path<doc>  (b, dir_path ("doc"));  // Install into install.doc.
      path<man>  (b, dir_path ("man"));  // Install into install.man.
      path<man1> (b, dir_path ("man1")); // Install into install.man1.

      return true;
    }
  }
}
