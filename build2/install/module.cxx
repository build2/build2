// file      : build2/install/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/install/module>

#include <build2/scope>
#include <build2/target>
#include <build2/rule>
#include <build2/operation>
#include <build2/diagnostics>

#include <build2/config/utility>

#include <build2/install/rule>
#include <build2/install/utility>
#include <build2/install/operation>

using namespace std;
using namespace butl;

namespace build2
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
        const variable& vr (var_pool.find<T> (move (vn)));

        cv = dv != nullptr
          ? &config::required (r, vr, *dv, override).first.get ()
          : &config::optional (r, vr);
      }

      vn = "install.";
      vn += name;
      vn += var;
      const variable& vr (var_pool.find<T> (move (vn)));

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
    set_dir (bool s,                         // specified
             scope& r,                       // root scope
             const char* n,                  // var name
             const string& ps,               // path (as string)
             const string& fm = string (),   // file mode
             const string& dm = string (),   // dir mode
             const string& c = string (),    // command
             bool o = false)                 // override
    {
      dir_path p (ps);
      set_var          (s, r, n, "",          p.empty ()  ? nullptr : &p, o);
      set_var          (s, r, n, ".mode",     fm.empty () ? nullptr : &fm);
      set_var          (s, r, n, ".dir_mode", dm.empty () ? nullptr : &dm);
      set_var<string>  (s, r, n, ".sudo",     nullptr);
      set_var          (s, r, n, ".cmd",      c.empty ()  ? nullptr : &c);
      set_var<strings> (s, r, n, ".options",  nullptr);
    }

    static alias_rule alias_;
    static file_rule file_;

    extern "C" void
    install_boot (scope& r, const location&, unique_ptr<module>&)
    {
      tracer trace ("install::boot");

      l5 ([&]{trace << "for " << r.out_path ();});

      // Register the install operation.
      //
      r.operations.insert (install_id, install);
    }

    extern "C" bool
    install_init (scope& r,
                  scope& b,
                  const location& l,
                  unique_ptr<module>&,
                  bool first,
                  bool)
    {
      tracer trace ("install::init");

      if (!first)
      {
        warn (l) << "multiple install module initializations";
        return true;
      }

      const dir_path& out_root (r.out_path ());
      l5 ([&]{trace << "for " << out_root;});

      // Enter module variables.
      //
      // Note that the set_dir() calls below enter some more.
      //
      if (first)
      {
        auto& v (var_pool);

        v.find<dir_path> ("install");
      }

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
        const string& n (cast<string> (*r["project"]));

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
