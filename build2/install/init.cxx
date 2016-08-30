// file      : build2/install/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/install/init>

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
    // For global values we only set config.install.* variables. Non-global
    // values with NULL defaults are omitted.
    //
    template <typename T, typename CT>
    static void
    set_var (bool spec,
             scope& r,
             const char* name,
             const char* var,
             const CT* dv,
             bool override = false)
    {
      string vn;
      const value* cv (nullptr);

      bool global (*name == '\0');

      if (spec)
      {
        vn = "config.install";
        if (!global)
        {
          vn += '.';
          vn += name;
        }
        vn += var;
        const variable& vr (var_pool.insert<CT> (move (vn), true));

        cv = dv != nullptr
          ? &config::required (r, vr, *dv, override).first.get ()
          : (global
             ? &config::optional (r, vr)
             : config::omitted (r, vr).first);
      }

      if (global)
        return;

      vn = "install.";
      vn += name;
      vn += var;
      const variable& vr (var_pool.insert<T> (move (vn))); // Not overridable.

      value& v (r.assign (vr));

      if (spec)
      {
        if (cv != nullptr && *cv)
          v = cast<T> (*cv); // Strip CT to T.
      }
      else
      {
        if (dv != nullptr)
          v = *dv;
      }
    }

    template <typename T>
    static void
    set_dir (bool s,                                  // specified
             scope& r,                                // root scope
             const char* n,                           // var name
             const T& p,                              // path
             bool o = false,                          // override
             const string& fm = string (),            // file mode
             const string& dm = string (),            // dir mode
             const build2::path& c = build2::path ()) // command
    {
      using build2::path;

      bool global (*n == '\0');

      if (!global)
        set_var<dir_path> (s, r, n, "",        p.empty ()  ? nullptr : &p, o);

      set_var<path>     (s, r, n, ".cmd",      c.empty ()  ? nullptr : &c);
      set_var<strings>  (s, r, n, ".options",  (strings*) (nullptr));
      set_var<string>   (s, r, n, ".mode",     fm.empty () ? nullptr : &fm);
      set_var<string>   (s, r, n, ".dir_mode", dm.empty () ? nullptr : &dm);
      set_var<string>   (s, r, n, ".sudo",     (string*) (nullptr));

      // This one doesn't have config.* value (only set in a buildfile).
      //
      if (!global)
        var_pool.insert<bool> (string ("install.") + n + ".subdirs");
    }

    static alias_rule alias_;
    static file_rule file_;

    void
    boot (scope& r, const location&, unique_ptr<module_base>&)
    {
      tracer trace ("install::boot");

      l5 ([&]{trace << "for " << r.out_path ();});

      // Register the install and uninstall operations.
      //
      r.operations.insert (install_id, install);
      r.operations.insert (uninstall_id, uninstall);
    }

    static const path cmd ("install");

    static const dir_path dir_root ("root");

    static const dir_path dir_sbin    (dir_path ("exec_root") /= "sbin");
    static const dir_path dir_bin     (dir_path ("exec_root") /= "bin");
    static const dir_path dir_lib     (dir_path ("exec_root") /= "lib");
    static const dir_path dir_libexec (dir_path ("exec_root") /= "libexec");

    static const dir_path dir_data    (dir_path ("data_root") /= "share");
    static const dir_path dir_include (dir_path ("data_root") /= "include");

    static const dir_path dir_doc  (dir_path (dir_data) /= "doc");
    static const dir_path dir_man  (dir_path (dir_data) /= "man");
    static const dir_path dir_man1 (dir_path ("man") /= "man1");

    bool
    init (scope& r,
          scope& b,
          const location& l,
          unique_ptr<module_base>&,
          bool first,
          bool,
          const variable_map& config_hints)
    {
      tracer trace ("install::init");

      if (!first)
      {
        warn (l) << "multiple install module initializations";
        return true;
      }

      const dir_path& out_root (r.out_path ());
      l5 ([&]{trace << "for " << out_root;});

      assert (config_hints.empty ()); // We don't known any hints.

      // Enter module variables.
      //
      // Note that the set_dir() calls below enter some more.
      //
      {
        auto& v (var_pool);

        // Note: not overridable.
        //
        // The install variable is a path, not dir_path, since it can be used
        // to both specify the target directory (to install with the same file
        // name) or target file (to install with a different name). And the
        // way we distinguish between the two is via the presence/absence of
        // the trailing directory separator.
        //
        v.insert<path> ("install");
        v.insert<string> ("install.mode");
      }

      // Register our alias and file rules.
      //
      b.rules.insert<alias> (perform_install_id,   "install.alias", alias_);
      b.rules.insert<alias> (perform_uninstall_id, "uninstall.alias", alias_);

      b.rules.insert<file> (perform_install_id,   "install.file", file_);
      b.rules.insert<file> (perform_uninstall_id, "uinstall.file", file_);

      // Configuration.
      //
      // Note that we don't use any defaults for root -- the location
      // must be explicitly specified or the installer will complain
      // if and when we try to install.
      //
      {
        using build2::path;

        bool s (config::specified (r, "config.install"));

        // Adjust module priority so that the (numerous) config.install.*
        // values are saved at the end of config.build.
        //
        if (s)
          config::save_module (r, "install", INT32_MAX);

        const string& n (cast<string> (r["project"]));

        // Global config.install.* values.
        //
        set_dir (s, r, "",          abs_dir_path (), false, "644", "755", cmd);

        set_dir (s, r, "root",      abs_dir_path ());

        set_dir (s, r, "data_root", dir_root);
        set_dir (s, r, "exec_root", dir_root, false, "755");

        set_dir (s, r, "sbin",      dir_sbin);
        set_dir (s, r, "bin",       dir_bin);
        set_dir (s, r, "lib",       dir_lib);
        set_dir (s, r, "libexec",   dir_path (dir_libexec) /= n, true);

        set_dir (s, r, "data",      dir_path (dir_data) /= n, true);
        set_dir (s, r, "include",   dir_include);

        set_dir (s, r, "doc",       dir_path (dir_doc) /= n, true);
        set_dir (s, r, "man",       dir_man);
        set_dir (s, r, "man1",      dir_man1);
      }

      // Configure "installability" for built-in target types.
      //
      install_path<doc>  (b, dir_path ("doc"));  // Install into install.doc.
      install_path<man>  (b, dir_path ("man"));  // Install into install.man.
      install_path<man1> (b, dir_path ("man1")); // Install into install.man1.

      return true;
    }
  }
}
