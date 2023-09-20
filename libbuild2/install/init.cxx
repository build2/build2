// file      : libbuild2/install/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/install/init.hxx>

#include <libbutl/command.hxx> // command_substitute()

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
    // Process an install.<name>.* value replacing the <var>-substitutions
    // with their actual values. Note that for now we are only doing this for
    // dir_path (install.<name> variables).
    //
    // The semantics of <>-substitution is inspired by our command running
    // facility. In a nutshell, `<<` is an escape, unknown or unterminated
    // substitution is an error.
    //
    //
    template<typename T>
    static inline T
    proc_var (const dir_path*, scope&, const T& val, const variable&)
    {
      return val;
    }

    static inline dir_path
    proc_var (const dir_path* prv,
              scope& rs,
              const dir_path& val,
              const variable& var)
    {
      if (val.string ().find ('<') == string::npos)
        return val;

      // Note: watch out for the small std::function optimization.
      //
      struct data
      {
        const dir_path* prv;
        const dir_path& val;
        const variable& var;
      } d {prv, val, var};

      auto subst = [&d, &rs] (const string& n, string& r)
      {
        if (n == "project")
        {
          r += project (rs).string ();
        }
        else if (n == "version")
        {
          if (const auto* v = cast_null<string> (rs.vars[rs.ctx.var_version]))
            r += *v;
          else
            fail << "no version variable in project " << project (rs) <<
              info << "required in " << d.var << " value '" << d.val << "'";
        }
        else if (n == "private")
        {
          if (d.prv != nullptr && !d.prv->empty ())
            r += d.prv->string ();
        }
        else
          return false;

        return true;
      };

      dir_path r;
      for (auto i (val.begin ()); i != val.end (); ++i)
      {
        auto o (*i); // Original.

        size_t p (o.find ('<'));
        if (p == string::npos)
        {
          r.combine (o, i.separator ());
          continue;
        }

        string s; // Substituted.
        try
        {
          s = command_substitute (o, p, subst, '<', '>');
        }
        catch (const invalid_argument& e)
        {
          fail << "invalid " << var << " value '" << val << "': " << e;
        }

        // In case of <private> the result of the substitution may have
        // multiple path components.
        //
        if (path::traits_type::find_separator (s) == string::npos)
        {
          r.combine (s, i.separator ());
          continue;
        }

        dir_path d;
        try
        {
          d = dir_path (move (s));
        }
        catch (const invalid_path& e)
        {
          fail << "invalid path '" << e.path << "'";
        }

        // Use the substitution's separators except for the last one.
        //
        for (auto j (d.begin ()), e (d.end ()); j != e; )
        {
          auto c (*j);
          r.combine (c, ++j != e ? j.separator () : i.separator ());
        }
      }

      return r;
    }

    // Set an install.<name>.* value based on config.install.<name>.* or the
    // default. If none of config.install.* values were specified (spec is
    // false), then we do omitted/delayed configuration. Note that we still
    // need to set all the install.* values to defaults, as if we had the
    // default configuration.
    //
    // If override is true, then override values that came from outer
    // configurations. We had to do this for paths that contain the project
    // name but now we use the <project> substitution. Let's keep this
    // functionality for now in case we need it for something else.
    //
    // For global values we only set config.install.* variables. Non-global
    // values with NULL defaults are omitted.
    //
    template <typename T, typename CT>
    static void
    set_var (bool spec,
             const dir_path* prv,
             scope& rs,
             const char* name,
             const char* var,
             const CT* dv,
             bool override = false)
    {
      string vn;
      lookup l;

      bool global (*name == '\0');

      auto& vp (rs.var_pool (true /* default */)); // All qualified.

      if (spec)
      {
        vn = "config.install";
        if (!global)
        {
          vn += '.';
          vn += name;
        }
        vn += var;
        const variable& vr (vp.insert<CT> (move (vn)));

        using config::lookup_config;

        l = dv != nullptr
          ? lookup_config (rs, vr, *dv, 0 /* save_flags */, override)
          : (global
             ? lookup_config (rs, vr, nullptr)
             : lookup_config (rs, vr));
      }

      if (global)
        return;

      vn = "install.";
      vn += name;
      vn += var;
      const variable& vr (vp.insert<T> (move (vn)));

      value& v (rs.assign (vr));

      if (spec)
      {
        if (l)
          v = proc_var (prv, rs, cast<T> (l), vr); // Strip CT to T.
      }
      else
      {
        if (dv != nullptr)
          v = proc_var (prv, rs, *dv, vr);
      }
    }

    template <typename T>
    static void
    set_dir (bool s,                                  // specified
             const dir_path* p,                       // <private>
             scope& rs,                               // root scope
             const char* n,                           // var name
             const T& d,                              // path
             bool o = false,                          // override
             const string& fm = string (),            // file mode
             const string& dm = string (),            // dir mode
             const build2::path& c = build2::path ()) // command
    {
      using build2::path;

      bool global (*n == '\0');

      if (!global)
        set_var<dir_path> (s, p, rs, n, "",       d.empty ()  ? nullptr : &d, o);

      set_var<path>    (s, p, rs, n, ".cmd",      c.empty ()  ? nullptr : &c);
      set_var<strings> (s, p, rs, n, ".options",  (strings*) (nullptr));
      set_var<string>  (s, p, rs, n, ".mode",     fm.empty () ? nullptr : &fm);
      set_var<string>  (s, p, rs, n, ".dir_mode", dm.empty () ? nullptr : &dm);
      set_var<string>  (s, p, rs, n, ".sudo",     (string*) (nullptr));

      // This one doesn't have config.* value (only set in a buildfile).
      //
      if (!global)
        rs.var_pool (true).insert<bool> (string ("install.") + n + ".subdirs");
    }

    void
    functions (function_map&); // functions.cxx

    void
    boot (scope& rs, const location&, module_boot_extra&)
    {
      tracer trace ("install::boot");
      l5 ([&]{trace << "for " << rs;});

      context& ctx (rs.ctx);

      // Enter module variables (note that init() below enters some more).
      //
      // The install variable is a path, not dir_path, since it can be used
      // to both specify the target directory (to install with the same file
      // name) or target file (to install with a different name). And the
      // way we distinguish between the two is via the presence/absence of
      // the trailing directory separator.
      //
      // Plus it can have the special true/false values when acting as an
      // operation variable.
      //
      auto& ovar (rs.var_pool ().insert<path> ("install",
                                               variable_visibility::target));

      // Register the install function family if this is the first instance of
      // the install modules.
      //
      if (!function_family::defined (ctx.functions, "install"))
        functions (ctx.functions);

      // Register our operations.
      //
      rs.insert_operation (install_id,            op_install,            &ovar);
      rs.insert_operation (uninstall_id,          op_uninstall,          &ovar);
      rs.insert_operation (update_for_install_id, op_update_for_install, &ovar);
    }

    static const path cmd ("install");

    // Default config.install.* values.
    //
#define DIR(N, V) static const dir_path dir_##N (V)

    DIR (data_root,     dir_path ("root"));
    DIR (exec_root,     dir_path ("root"));

    DIR (sbin,          dir_path ("exec_root") /= "sbin");
    DIR (bin,           dir_path ("exec_root") /= "bin");
    DIR (lib,          (dir_path ("exec_root") /= "lib") /= "<private>");
    DIR (libexec,     ((dir_path ("exec_root") /= "libexec") /= "<private>") /= "<project>");
    DIR (pkgconfig,     dir_path ("lib")       /= "pkgconfig");

    DIR (etc,           dir_path ("data_root") /= "etc");
    DIR (include,      (dir_path ("data_root") /= "include") /= "<private>");
    DIR (include_arch,  dir_path ("include"));
    DIR (share,         dir_path ("data_root") /= "share");
    DIR (data,         (dir_path ("share")     /= "<private>") /= "<project>");
    DIR (buildfile,   ((dir_path ("share") /= "build2") /= "export") /= "<project>");

    DIR (doc,         ((dir_path ("share")     /= "doc") /= "<private>") /= "<project>");
    DIR (legal,         dir_path ("doc"));
    DIR (man,           dir_path ("share")     /= "man");
    DIR (man1,          dir_path ("man")       /= "man1");

#undef DIR

    static const group_rule group_rule_ (true /* see_through_only */);

    bool
    init (scope& rs,
          scope& bs,
          const location& l,
          bool first,
          bool,
          module_init_extra&)
    {
      tracer trace ("install::init");

      if (!first)
      {
        warn (l) << "multiple install module initializations";
        return true;
      }

      l5 ([&]{trace << "for " << rs;});

      // Enter module variables.
      //
      rs.var_pool ().insert<bool> ("for_install", variable_visibility::prereq);

      // The rest of the variables we enter are qualified so go straight
      // for the public variable pool.
      //
      auto& vp (rs.var_pool (true /* public */));

      // Note that the set_dir() calls below enter some more.
      //
      vp.insert<string> ("install.mode");
      vp.insert<bool>   ("install.subdirs");

      // Environment.
      //
      // Installation may involve executing the following programs:
      //
      // install
      //
      //   GNU coreutils install recognizes the SIMPLE_BACKUP_SUFFIX and
      //   VERSION_CONTROL variables but they only matter with --backup which
      //   we do not specify and assume unlikely to be specified via .options.
      //
      //   FreeBSD install recognizes STRIPBIN and DONTSTRIP variables that
      //   only matter with -s which we do not specify but which could be
      //   specified with .options. NetBSD and OpenBSD use STRIP (Mac OS man
      //   page doesn't list anything).
      //
      // sudo
      //
      //   While sudo has a bunch of SUDO_* variables, none of them appear to
      //   matter (either not used in the modes that we invoke sudo in or do
      //   not affect the result).
      //
      // ln, rm, rmdir
      //
      //   GNU coreutils ln recognizes the SIMPLE_BACKUP_SUFFIX and
      //   VERSION_CONTROL variables but they only matter with --backup which
      //   we do not specify.
      //
#if   defined(__FreeBSD__)
      config::save_environment (rs, {"STRIPBIN", "DONTSTRIP"});
#elif defined(__NetBSD__) || \
      defined(__OpenBSD__)
      config::save_environment (rs, "STRIP");
#endif

      // Register our rules.
      //
      {
        const auto& ar (alias_rule::instance);
        const auto& dr (fsdir_rule::instance);
        const auto& fr (file_rule::instance);
        const auto& gr (group_rule_);

        bs.insert_rule<alias> (perform_install_id,   "install.alias", ar);
        bs.insert_rule<alias> (perform_uninstall_id, "install.alias", ar);

        bs.insert_rule<fsdir> (perform_install_id,   "install.fsdir", dr);
        bs.insert_rule<fsdir> (perform_uninstall_id, "install.fsdir", dr);

        bs.insert_rule<file> (perform_install_id,   "install.file", fr);
        bs.insert_rule<file> (perform_uninstall_id, "install.file", fr);

        // Note: use mtime_target (instead of target) to take precedence over
        // the fallback file rules below.
        //
        // @@ We could fix this by checking the target type in file_rule,
        //    similar to build2::file_rule.
        //
        bs.insert_rule<mtime_target> (perform_install_id,   "install.group", gr);
        bs.insert_rule<mtime_target> (perform_uninstall_id, "install.group", gr);

        // Register the fallback file rule for the update-for-[un]install
        // operation, similar to update.
        //
        // @@ Hm, it's a bit fuzzy why we would be updating-for-install
        //    something outside of any project?
        //
        scope& gs (rs.global_scope ());

        gs.insert_rule<mtime_target> (perform_install_id,   "install.file", fr);
        gs.insert_rule<mtime_target> (perform_uninstall_id, "install.file", fr);
      }

      // Configuration.
      //
      // Note that we don't use any defaults for root -- the location
      // must be explicitly specified or the installer will complain
      // if and when we try to install.
      //
      {
        using build2::path;
        using config::lookup_config;
        using config::specified_config;

        // Note: ignore config.install.{scope,manifest} (see below).
        //
        bool s (specified_config (rs, "install", {"scope", "manifest"}));

        // Adjust module priority so that the (numerous) config.install.*
        // values are saved at the end of config.build.
        //
        if (s)
          config::save_module (rs, "install", INT32_MAX);

        // config.install.scope
        //
        // We do not install prerequisites (for example, shared libraries) of
        // targets (for example, executables) that belong to projects outside
        // of this scope. Valid values are:
        //
        //   project -- project scope
        //   bundle  -- bundle amalgamation
        //   strong  -- strong amalgamation
        //   weak    -- weak amalgamation
        //   global  -- all projects (default)
        //
        // Note: can only be specified as a global override.
        //
        {
          auto& v (vp.insert<string> ("config.install.scope"));

          // If specified, verify it is a global override.
          //
          if (lookup l = rs[v])
          {
            if (!l.belongs (rs.global_scope ()))
              fail << "config.install.scope must be a global override" <<
                info << "specify !config.install.scope=...";
          }

          config::unsave_variable (rs, v);
        }

        // config.install.manifest
        //
        // Installation manifest. Valid values are a file path or `-` to dump
        // the manifest to stdout.
        //
        // If specified during the install operation, then write the
        // information about all the filesystem entries being installed into
        // the manifest. If specified during uninstall, then remove the
        // filesystem entries according to the manifest as opposed to the
        // current build state. In particular, this functionality can be used
        // to avoid surprising (and potentially lengthy) updates during
        // uninstall that may happen because of changes to system-installed
        // dependencies (for example, the compiler or standard library).
        //
        // @@ TODO: manifest uninstall is still TODO.
        //
        // Note: there is a single manifest per operation and thus this
        // variable can only be specified as a global override. (While it
        // could be handy to save this varible in config.build in some
        // situations, supporting this will complicate the global override
        // case). Note that as a result the manifest file path may not be
        // specified in terms of the config.install.* values.
        //
        // Note also that the manifest is produced even in the dry-run mode.
        // However, in this case no directory creation is tracked.
        //
        // The format of the installation manifest is "JSON lines", that is,
        // each line is a JSON text (this makes it possible to reverse the
        // order of lines without loading the entire file into memory). For
        // example (indented lines indicate line continuations):
        //
        // {"type":"directory","path":"/tmp/install","mode":"755"}
        // {"type":"target","name":"/tmp/libhello/libs{hello}",
        //   "entries":[
        //     {"type":"file","path":"/tmp/install/lib/libhello-1.0.so","mode":"755"},
        //     {"type":"symlink","path":"/tmp/install/lib/libhello.so","target":"libhello-1.0.so"}]}
        //
        // Each line is a serialization of one of the following non-abstract
        // C++ structs:
        //
        // struct entry // abstract
        // {
        //   enum {directory, file, symlink, target} type;
        // };
        //
        // struct filesystem_entry: entry // abstract
        // {
        //   path path;
        // };
        //
        // struct directory_entry: filesystem_entry
        // {
        //   string mode;
        // };
        //
        // struct file_entry: filesystem_entry
        // {
        //   string mode;
        // };
        //
        // struct symlink_entry: filesystem_entry
        // {
        //   path target;
        // };
        //
        // struct target_entry: entry
        // {
        //   string name;
        //   vector<filesystem_entry*> entries;
        // };
        //
        // New entry types may be added later. Additional entry members may be
        // added later to existing entries after the existing members.
        //
        // If installation is relocatable (see config.install.relocatable) and
        // the installation manifest file path is inside config.install.root
        // (including chroot), then absolute filesystem_entry::path's are
        // saved as relative to the manifest file's directory (note that
        // symlink_entry::target cannot be absolute in relocatable
        // installation).
        //
        {
          auto& v (vp.insert<path> ("config.install.manifest"));

          // If specified, verify it is a global override.
          //
          if (lookup l = rs[v])
          {
            if (!l.belongs (rs.global_scope ()))
              fail << "config.install.manifest must be a global override" <<
                info << "specify !config.install.manifest=...";
          }

          config::unsave_variable (rs, v);
        }

        // Support for relocatable install.
        //
        // Note that it is false by default since supporting relocatable
        // installation may require extra effort and not all projects may
        // support it. A project that is known not to support it should assert
        // this fact in its root.build, for example:
        //
        // assert (!$install.relocatable) 'relocatable installation not supported'
        //
        {
          auto& var  (vp.insert<bool> (       "install.relocatable"));
          auto& cvar (vp.insert<bool> ("config.install.relocatable"));

          value& v (rs.assign (var));

          // Note: unlike other variables, for ease of assertion set it to
          // false if no config.install.* is specified.
          //
          v = s && cast_false<bool> (lookup_config (rs, cvar, false));
        }

        // Support for private install (aka poor man's Flatpack).
        //
        const dir_path* p;
        {
          auto& var  (vp.insert<dir_path> (       "install.private"));
          auto& cvar (vp.insert<dir_path> ("config.install.private"));

          value& v (rs.assign (var));

          if (s)
          {
            if (lookup l = lookup_config (rs, cvar, nullptr))
              v = cast<dir_path> (l);
          }

          if ((p = cast_null<dir_path> (v)) != nullptr)
          {
            if (p->absolute ())
              fail << "absolute directory " << *p << " in install.private";
          }
        }

        // Support for chroot'ed install (aka DESTDIR).
        //
        {
          auto& var  (vp.insert<dir_path>     (       "install.chroot"));
          auto& cvar (vp.insert<abs_dir_path> ("config.install.chroot"));

          value& v (rs.assign (var));

          if (s)
          {
            if (lookup l = lookup_config (rs, cvar, nullptr))
              v = cast<dir_path> (l); // Strip abs_dir_path.
          }
        }

        // config.install.filter
        //
        // Installation filterting. The value of this variable is a list of
        // key-value pairs that specify the filesystem entries to include or
        // exclude from the installation. For example, the following filters
        // will omit installing headers and static libraries (notice the
        // quoting of the wildcard).
        //
        // config.install.filter='include/@false "*.a"@false'
        //
        // The key in each pair is a file or directory path or a path wildcard
        // pattern. If a key is relative and contains a directory component or
        // is a directory, then it is treated relative to the corresponding
        // config.install.* location. Otherwise (simple path, normally a
        // pattern), it is matched against the leaf of any path. Note that if
        // an absolute path is specified, it should be without the
        // config.install.chroot prefix.
        //
        // The value in each pair is either true (include) or false (exclude).
        // The filters are evaluated in the order specified and the first
        // match that is found determines the outcome. If no match is found,
        // the default is to include. For a directory, while false means
        // exclude all the sub-paths inside this directory, true does not mean
        // that all the sub-paths will be included wholesale. Rather, the
        // matched component of the sub-path is treated as included with the
        // rest of the components matched against the following
        // sub-filters. For example:
        //
        // config.install.filter='
        //   include/x86_64-linux-gnu/@true
        //   include/x86_64-linux-gnu/details/@false
        //   include/@false'
        //
        // The true or false value may be followed by comma and the `symlink`
        // modifier to only apply to symlink filesystem entries. For example:
        //
        // config.install.filter='"*.so"@false,symlink'
        //
        // Note that this mechanism only affects what gets physically copied
        // to the installation directory without affecting what gets built for
        // install or the view of what gets installed at the buildfile level.
        // For example, given the `include/@false *.a@false` filters, static
        // libraries will still be built (unless arranged not to with
        // config.bin.lib) and the pkg-config files will still end up with -I
        // options pointing to the header installation directory. Note also
        // that this mechanism applies to both install and uninstall
        // operations.
        //
        // If you are familiar with the Debian or Fedora packaging, this
        // mechanism is somewhat similar to (and can be used for a similar
        // purpose as) the Debian's .install files and Fedora's %files spec
        // file sections that are used to split the installation into multiple
        // binary packages.
        //
        {
          auto& var  (vp.insert<filters> (       "install.filter"));
          auto& cvar (vp.insert<filters> ("config.install.filter"));

          value& v (rs.assign (var));

          if (s)
          {
            if (lookup l = lookup_config (rs, cvar, nullptr))
              v = cast<filters> (l);
          }
        }

        // Global config.install.* values.
        //
        set_dir (s, p, rs, "",             abs_dir_path (), false, "644", "755", cmd);

        set_dir (s, p, rs, "root",         abs_dir_path ());

        set_dir (s, p, rs, "data_root",    dir_data_root);
        set_dir (s, p, rs, "exec_root",    dir_exec_root, false, "755");

        set_dir (s, p, rs, "sbin",         dir_sbin);
        set_dir (s, p, rs, "bin",          dir_bin);
        set_dir (s, p, rs, "lib",          dir_lib);
        set_dir (s, p, rs, "libexec",      dir_libexec);
        set_dir (s, p, rs, "pkgconfig",    dir_pkgconfig, false, "644");

        set_dir (s, p, rs, "etc",          dir_etc);
        set_dir (s, p, rs, "include",      dir_include);
        set_dir (s, p, rs, "include_arch", dir_include_arch);
        set_dir (s, p, rs, "share",        dir_share);
        set_dir (s, p, rs, "data",         dir_data);
        set_dir (s, p, rs, "buildfile",    dir_buildfile);

        set_dir (s, p, rs, "doc",          dir_doc);
        set_dir (s, p, rs, "legal",        dir_legal);
        set_dir (s, p, rs, "man",          dir_man);
        set_dir (s, p, rs, "man1",         dir_man1);
      }

      // Configure "installability" for built-in target types.
      //
      // Note that for exe{} we also set explicit 755 mode in case it gets
      // installed somewhere else where the default is not 755 (for example to
      // libexec/, which on Debian has the 644 mode).
      //
      install_path<exe>   (bs, dir_path ("bin"));
      install_mode<exe>   (bs, "755");
      install_path<doc>   (bs, dir_path ("doc"));
      install_path<legal> (bs, dir_path ("legal"));
      install_path<man>   (bs, dir_path ("man"));
      install_path<man1>  (bs, dir_path ("man1"));

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
