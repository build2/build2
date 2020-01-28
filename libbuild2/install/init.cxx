// file      : libbuild2/install/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/install/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/rule.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/operation.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/install/rule.hxx>
#include <libbuild2/install/utility.hxx>
#include <libbuild2/install/operation.hxx>

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
             scope& rs,
             const char* name,
             const char* var,
             const CT* dv,
             bool override = false)
    {
      string vn;
      lookup l;

      bool global (*name == '\0');

      if (spec)
      {
        // Note: overridable.
        //
        vn = "config.install";
        if (!global)
        {
          vn += '.';
          vn += name;
        }
        vn += var;
        const variable& vr (rs.var_pool ().insert<CT> (move (vn), true));

        l = dv != nullptr
          ? config::required (rs, vr, *dv, override).first
          : (global
             ? config::optional (rs, vr)
             : config::omitted (rs, vr).first);
      }

      if (global)
        return;

      // Note: not overridable.
      //
      vn = "install.";
      vn += name;
      vn += var;
      const variable& vr (rs.var_pool ().insert<T> (move (vn)));

      value& v (rs.assign (vr));

      if (spec)
      {
        if (l)
          v = cast<T> (l); // Strip CT to T.
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
             scope& rs,                               // root scope
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
        set_var<dir_path> (s, rs, n, "",        p.empty ()  ? nullptr : &p, o);

      set_var<path>     (s, rs, n, ".cmd",      c.empty ()  ? nullptr : &c);
      set_var<strings>  (s, rs, n, ".options",  (strings*) (nullptr));
      set_var<string>   (s, rs, n, ".mode",     fm.empty () ? nullptr : &fm);
      set_var<string>   (s, rs, n, ".dir_mode", dm.empty () ? nullptr : &dm);
      set_var<string>   (s, rs, n, ".sudo",     (string*) (nullptr));

      // This one doesn't have config.* value (only set in a buildfile).
      //
      if (!global)
        rs.var_pool ().insert<bool> (string ("install.") + n + ".subdirs");
    }

    void
    functions (function_map&); // functions.cxx

    bool
    boot (scope& rs, const location&, unique_ptr<module_base>&)
    {
      tracer trace ("install::boot");
      l5 ([&]{trace << "for " << rs;});

      context& ctx (rs.ctx);

      // Register the install function family if this is the first instance of
      // the install modules.
      //
      if (!function_family::defined (ctx.functions, "install"))
        functions (ctx.functions);

      // Register our operations.
      //
      rs.insert_operation (install_id,            op_install);
      rs.insert_operation (uninstall_id,          op_uninstall);
      rs.insert_operation (update_for_install_id, op_update_for_install);

      return false;
    }

    static const path cmd ("install");

    static const dir_path dir_root ("root");

    static const dir_path dir_sbin      (dir_path ("exec_root") /= "sbin");
    static const dir_path dir_bin       (dir_path ("exec_root") /= "bin");
    static const dir_path dir_lib       (dir_path ("exec_root") /= "lib");
    static const dir_path dir_libexec   (dir_path ("exec_root") /= "libexec");
    static const dir_path dir_pkgconfig (dir_path ("lib") /= "pkgconfig");

    static const dir_path dir_data    (dir_path ("data_root") /= "share");
    static const dir_path dir_include (dir_path ("data_root") /= "include");

    static const dir_path dir_doc  (dir_path (dir_data) /= "doc");
    static const dir_path dir_man  (dir_path (dir_data) /= "man");
    static const dir_path dir_man1 (dir_path ("man") /= "man1");

    static const group_rule group_rule_ (true /* see_through_only */);

    bool
    init (scope& rs,
          scope& bs,
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

      l5 ([&]{trace << "for " << rs;});

      assert (config_hints.empty ()); // We don't known any hints.

      // Enter module variables.
      //
      auto& vp (rs.var_pool ());

      // Note that the set_dir() calls below enter some more.
      //
      {
        // Note: not overridable.
        //
        // The install variable is a path, not dir_path, since it can be used
        // to both specify the target directory (to install with the same file
        // name) or target file (to install with a different name). And the
        // way we distinguish between the two is via the presence/absence of
        // the trailing directory separator.
        //
        vp.insert<path>   ("install",         variable_visibility::target);
        vp.insert<bool>   ("for_install",     variable_visibility::prereq);
        vp.insert<string> ("install.mode",    variable_visibility::project);
        vp.insert<bool>   ("install.subdirs", variable_visibility::project);
      }

      // Register our rules.
      //
      {
        const auto& ar (alias_rule::instance);
        const auto& dr (fsdir_rule::instance);
        const auto& fr (file_rule::instance);
        const auto& gr (group_rule_);

        bs.insert_rule<alias> (perform_install_id,   "install.alias", ar);
        bs.insert_rule<alias> (perform_uninstall_id, "uninstall.alias", ar);

        bs.insert_rule<fsdir> (perform_install_id,   "install.fsdir", dr);
        bs.insert_rule<fsdir> (perform_uninstall_id, "install.fsdir", dr);

        bs.insert_rule<file> (perform_install_id,   "install.file", fr);
        bs.insert_rule<file> (perform_uninstall_id, "uninstall.file", fr);

        bs.insert_rule<target> (perform_install_id,   "install.file", gr);
        bs.insert_rule<target> (perform_uninstall_id, "uninstall.file", gr);
     }

      // Configuration.
      //
      // Note that we don't use any defaults for root -- the location
      // must be explicitly specified or the installer will complain
      // if and when we try to install.
      //
      {
        using build2::path;

        bool s (config::specified (rs, "install"));

        // Adjust module priority so that the (numerous) config.install.*
        // values are saved at the end of config.build.
        //
        if (s)
          config::save_module (rs, "install", INT32_MAX);

        const string& n (project (rs).string ());

        // Global config.install.* values.
        //
        set_dir (s, rs, "",          abs_dir_path (), false, "644", "755", cmd);

        set_dir (s, rs, "root",      abs_dir_path ());

        set_dir (s, rs, "data_root", dir_root);
        set_dir (s, rs, "exec_root", dir_root, false, "755");

        set_dir (s, rs, "sbin",      dir_sbin);
        set_dir (s, rs, "bin",       dir_bin);
        set_dir (s, rs, "lib",       dir_lib);
        set_dir (s, rs, "libexec",   dir_path (dir_libexec) /= n, true);
        set_dir (s, rs, "pkgconfig", dir_pkgconfig, false, "644");

        set_dir (s, rs, "data",      dir_path (dir_data) /= n, true);
        set_dir (s, rs, "include",   dir_include);

        set_dir (s, rs, "doc",       dir_path (dir_doc) /= n, true);
        set_dir (s, rs, "man",       dir_man);
        set_dir (s, rs, "man1",      dir_man1);

        // Support for chroot'ed install (aka DESTDIR).
        //
        {
          auto& var  (vp.insert<dir_path>     (       "install.chroot"));
          auto& cvar (vp.insert<abs_dir_path> ("config.install.chroot", true));

          value& v (rs.assign (var));

          if (s)
          {
            if (lookup l = config::optional (rs, cvar))
              v = cast<dir_path> (l); // Strip abs_dir_path.
          }
        }
      }

      // Configure "installability" for built-in target types.
      //
      install_path<exe>  (bs, dir_path ("bin"));  // Install into install.bin.
      install_path<doc>  (bs, dir_path ("doc"));  // Install into install.doc.
      install_path<man>  (bs, dir_path ("man"));  // Install into install.man.
      install_path<man1> (bs, dir_path ("man1")); // Install into install.man1.

      return true;
    }

    static const module_functions mod_functions[] =
    {
      {"install", &boot,   &init},
      {nullptr,   nullptr, nullptr}
    };

    const module_functions*
    build2_install_load ()
    {
      return mod_functions;
    }
  }
}
