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
    template <typename T>
    static void
    set_var (bool spec,
             scope& r,
             const char* name,
             const char* var,
             const T* dv)
    {
      string vn;
      const value* cv (nullptr);

      if (spec)
      {
        vn = "config.install.";
        vn += name;
        vn += var;
        const variable& vr (
          variable_pool.find (move (vn), &value_traits<T>::value_type));

        cv = dv != nullptr
          ? &config::required (r, vr, *dv).first.get ()
          : &config::optional (r, vr);
      }

      vn = "install.";
      vn += name;
      vn += var;
      const variable& vr (
        variable_pool.find (move (vn), &value_traits<T>::value_type));

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
             const dir_path& path,
             const string& fmode = string (),
             const string& dmode = string (),
             const string& cmd = string ())
    {
      set_var (s, r, name, "",          path.empty ()  ? nullptr : &path);
      set_var (s, r, name, ".mode",     fmode.empty () ? nullptr : &fmode);
      set_var (s, r, name, ".dir_mode", dmode.empty () ? nullptr : &dmode);
      set_var (s, r, name, ".cmd",      cmd.empty ()   ? nullptr : &cmd);
      set_var<strings> (s, r, name, ".options", nullptr);
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

      const dir_path& out_root (r.out_path ());
      level5 ([&]{trace << "for " << out_root;});

      // Register the install operation.
      //
      r.operations.insert (install_id, install);

      // Register our file installer rule.
      //
      b.rules.insert<file> (perform_id, install_id, "install", rule_);

      // Enter module variables.
      //
      // Note that the set_dir() calls below enter some more.
      //
      if (first)
      {
        variable_pool.find ("install", dir_path_type);
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
        const string& n (as<string> (*r["project"]));

        set_dir (s, r, "root",      dir_path (),        "", "755", "install");
        set_dir (s, r, "data_root", dir_path ("root"),  "644");
        set_dir (s, r, "exec_root", dir_path ("root"),  "755");

        set_dir (s, r, "sbin",      dir_path ("exec_root/sbin"));
        set_dir (s, r, "bin",       dir_path ("exec_root/bin"));
        set_dir (s, r, "lib",       dir_path ("exec_root/lib"));
        set_dir (s, r, "libexec",   dir_path ("exec_root/libexec/" + n));

        set_dir (s, r, "data",      dir_path ("data_root/share/" + n));
        set_dir (s, r, "include",   dir_path ("data_root/include"));

        set_dir (s, r, "doc",       dir_path ("data_root/share/doc/" + n));
        set_dir (s, r, "man",       dir_path ("data_root/share/man"));

        set_dir (s, r, "man1",      dir_path ("man/man1"));
      }

      // Configure "installability" for built-in target types.
      //
      path<doc>  (b, dir_path ("doc"));  // Install into install.doc.
      path<man>  (b, dir_path ("man"));  // Install into install.man.
      path<man1> (b, dir_path ("man1")); // Install into install.man1.
    }
  }
}
