// file      : libbuild2/cc/init.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/init.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/config/utility.hxx>

#include <libbuild2/cc/module.hxx>
#include <libbuild2/cc/target.hxx>
#include <libbuild2/cc/utility.hxx>
#include <libbuild2/cc/compiledb.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    // Scope operation callback that cleans up module sidebuilds.
    //
    static target_state
    clean_module_sidebuilds (const scope& rs)
    {
      context& ctx (rs.ctx);

      const dir_path& out_root (rs.out_path ());

      dir_path d (out_root /
                  rs.root_extra->build_dir /
                  module_build_modules_dir);

      if (exists (d))
      {
        if (rmdir_r (ctx, d))
        {
          // Clean up cc/build/ if it became empty.
          //
          d = out_root / rs.root_extra->build_dir / module_build_dir;
          if (empty (d))
          {
            rmdir (ctx, d, 2);

            // Clean up cc/ if it became empty.
            //
            d = out_root / rs.root_extra->build_dir / module_dir;
            if (empty (d))
            {
              rmdir (ctx, d, 2);

              // And build/ if it also became empty (e.g., in case of a build
              // with a transient configuration).
              //
              d = out_root / rs.root_extra->build_dir;
              if (empty (d))
                rmdir (ctx, d, 2);
            }
          }

          return target_state::changed;
        }
      }

      return target_state::unchanged;
    }

    // Scope operation callback that cleans up compilation databases.
    //
    static target_state
    clean_compiledb (const scope& rs)
    {
      context& ctx (rs.ctx);

      target_state r (target_state::unchanged);

      for (const unique_ptr<compiledb>& db: compiledbs)
      {
        const path& p (db->path);

        if (p.empty () ||
            ctx.scopes.find_out (p.directory ()).root_scope () != &rs)
          continue;

        if (rmfile (ctx, p))
          r = target_state::changed;
      }

      return r;
    }

    // Scope operation callback for cleaning module sidebuilds and compilation
    // databases.
    //
    static target_state
    clean_callback (action, const scope& rs, const dir&)
    {
      target_state r (clean_module_sidebuilds (rs));

      if (!compiledbs.empty ())
        r |= clean_compiledb (rs);

      return r;
    }

    // Detect if just <name> in the <name>[@<path>] form is actually <path>.
    // We assume it is <path> and not <name> if it contains a directory
    // component or is the special directory name (`.`/`..`) . If that's the
    // case, return canonicalized name representing <path>. See the call site
    // in core_config_init() below for background.
    //
    static optional<name>
    compiledb_name_to_path (const name& n)
    {
      if (n.directory ())
        return n;

      if (n.file ())
      {
        if (!n.dir.empty () ||
            path_traits::find_separator (n.value) != string::npos)
        {
          name r (n);
          r.canonicalize ();
          return r;
        }
        else if (n.value == "." || n.value == "..")
        {
          return name (dir_path (n.value));
        }
      }

      return nullopt;
    }

    // Custom save function that completes relative paths in the
    // config.cc.compiledb and config.cc.compiledb.name values.
    //
    static pair<names_view, const char*>
    save_compiledb_name (const scope&,
                         const value& v,
                         const value*,
                         names& storage)
    {
      const names& ns (v.as<names> ()); // Value is untyped.

      // Detect and handle the case where just <name> is actually <path>.
      //
      if (ns.size () == 1)
      {
        const name& n (ns.back ());

        if (optional<name> otn = compiledb_name_to_path (n))
        {
          name& tn (*otn);

          if (tn.dir.relative ())
            tn.dir.complete ();

          tn.dir.normalize ();

          storage.push_back (move (tn));
          return make_pair (names_view (storage), "=");
        }
      }

      if (find_if (ns.begin (), ns.end (),
                   [] (const name& n) {return n.pair;}) == ns.end ())
      {
        return make_pair (names_view (ns), "=");
      }

      storage = ns;
      for (auto i (storage.begin ()); i != storage.end (); ++i)
      {
        if (i->pair)
        {
          name& n (*++i);

          if (!n.directory ())
            n.canonicalize ();

          if (n.dir.relative ())
            n.dir.complete ();

          n.dir.normalize ();
        }
      }

      return make_pair (names_view (storage), "=");
    }

    bool
    core_vars_init (scope& rs,
                    scope&,
                    const location& loc,
                    bool first,
                    bool,
                    module_init_extra&)
    {
      tracer trace ("cc::core_vars_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      // Load bin.vars (we need its config.bin.target/pattern for hints).
      //
      load_module (rs, rs, "bin.vars", loc);

      // Enter variables.
      //
      // All the variables we enter are qualified so go straight for the
      // public variable pool.
      //
      auto& vp (rs.var_pool (true /* public */));

      auto v_t (variable_visibility::target);

      // NOTE: remember to update documentation if changing anything here.
      //
      vp.insert<strings> ("config.cc.poptions");
      vp.insert<strings> ("config.cc.coptions");
      vp.insert<strings> ("config.cc.loptions");
      vp.insert<strings> ("config.cc.aoptions");
      vp.insert<strings> ("config.cc.libs");

      vp.insert<string> ("config.cc.internal.scope");

      vp.insert<bool> ("config.cc.reprocess"); // See cc.preprocess below.

      vp.insert<abs_dir_path> ("config.cc.pkgconfig.sysroot");

      // Compilation database.
      //
      // See the manual for the semantics.
      //
      // config.cc.compiledb               --  <name>[@<path>]|<path> (untyped)
      // config.cc.compiledb.name          --  <name>[@<path>]... (untyped)
      // config.cc.compiledb.filter        --  [<name>@]<bool>...
      // config.cc.compiledb.filter.input  --  [<name>@]<target-type>...
      // config.cc.compiledb.filter.output --  [<name>@]<target-type>...
      //
      vp.insert                        ("config.cc.compiledb");
      vp.insert                        ("config.cc.compiledb.name");
      vp.insert<compiledb_name_filter> ("config.cc.compiledb.filter");
      vp.insert<compiledb_type_filter> ("config.cc.compiledb.filter.input");
      vp.insert<compiledb_type_filter> ("config.cc.compiledb.filter.output");

      vp.insert<strings> ("cc.poptions");
      vp.insert<strings> ("cc.coptions");
      vp.insert<strings> ("cc.loptions");
      vp.insert<strings> ("cc.aoptions");
      vp.insert<strings> ("cc.libs");

      vp.insert<string>  ("cc.internal.scope");
      vp.insert<strings> ("cc.internal.libs");

      vp.insert<strings> ("cc.export.poptions");
      vp.insert<strings> ("cc.export.coptions");
      vp.insert<strings> ("cc.export.loptions");
      vp.insert          ("cc.export.libs");
      vp.insert          ("cc.export.impl_libs");

      // Header (-I) and library (-L) search paths to use in the generated .pc
      // files instead of the default install.{include,lib}. Relative paths
      // are resolved as install paths.
      //
      vp.insert<dir_paths> ("cc.pkgconfig.include");
      vp.insert<dir_paths> ("cc.pkgconfig.lib");

      // {c,cxx}.predefs rule settings.
      //
      // Note that these variables are aliased as {c,cxx}.predefs.*.
      //
      // The cc.predefs.poptions variable controls whether the *.poptions are
      // included on the command line (in which case any macro definitions
      // they may contain will end up in the output). It is false by default
      // for pure predefs (i.e., when we preprocess an empty translation unit)
      // and is required if we are preprocessing a user-specified file (since
      // command line macros may affect what's in the user's file).
      //
      // The cc.predefs.default variable specifies the default macro value to
      // use in the buildfile{} and json{} output for macros that are not
      // defined to any value (e.g., just `#define FOO`). If not specified,
      // then 1 is used (which is what macros specified on the command line as
      // -DFOO end up being defined to by the C/C++ compilers).
      //
      // The cc.predefs.macros variable specifies the macros to extract for
      // the buildfile{} and json{} output. Additionally, optional mapping to
      // variable/member name (buildfile/json) can be specified as the second
      // half of a pair for each macro. For example:
      //
      // cc.predefs.macros = BYTE_ORDER __SIZEOF_SIZE_T__@SIZEOF_SIZE_T
      //
      // Note that for the buildfile{} output specifying cc.predefs.macros
      // is mandatory (since undefined macros need to be set to null).
      //
      // Note: it would have been better to enter these only if the
      // {c,cxx}.predefs is loaded but that would require inventing yet another
      // cc submodule.
      //
      vp.insert<bool>                          ("cc.predefs.poptions");
      vp.insert<string>                        ("cc.predefs.default");
      vp.insert<map<string, optional<string>>> ("cc.predefs.macros");

      // Hint variables (not overridable).
      //
      vp.insert<string>         ("config.cc.id",      false);
      vp.insert<string>         ("config.cc.hinter",  false); // Hinting module.
      vp.insert<string>         ("config.cc.pattern", false);
      vp.insert<strings>        ("config.cc.mode",    false);
      vp.insert<target_triplet> ("config.cc.target",  false);

      // Compiler runtime and C standard library.
      //
      vp.insert<string> ("cc.runtime");
      vp.insert<string> ("cc.stdlib");

      // Library target type in the <lang>[,<type>...] form where <lang> is
      // "c" (C library), "cxx" (C++ library), or "cc" (C-common library but
      // the specific language is not known). Currently recognized <type>
      // values are "binless" (library is binless) and "recursively-binless"
      // (library and all its prerequisite libraries are binless). Note that
      // another indication of a binless library is an empty path, which could
      // be easier/faster to check. Note also that there should be no
      // whitespaces of any kind and <lang> is always first.
      //
      // This value should be set on the library target as a rule-specific
      // variable by the matching rule. It is also saved in the generated
      // pkg-config files. Currently <lang> is used to decide which *.libs to
      // use during static linking. The "cc" language is used in the import
      // installed logic.
      //
      // Note that this variable cannot be set via the target type/pattern-
      // specific mechanism (see process_libraries()).
      //
      vp.insert<string> ("cc.type", v_t);

      // If set and is true, then this (imported) library has been found in a
      // system library search directory.
      //
      vp.insert<bool> ("cc.system", v_t);

      // C++ module name. Set on the bmi*{} target as a rule-specific variable
      // by the matching rule. Can also be set by the user (normally via the
      // x.module_name alias) on the x_mod{} source.
      //
      vp.insert<string> ("cc.module_name", v_t);

      // Importable header marker (normally set via the x.importable alias).
      //
      // Note that while at first it might seem like a good idea to allow
      // setting it on a scope, that will cause translation of inline/template
      // includes which is something we definitely don't want.
      //
      vp.insert<bool> ("cc.importable", v_t);

      // Ability to disable using preprocessed output for compilation.
      //
      vp.insert<bool> ("cc.reprocess");

      // Execute serially with regards to any other recipe. This is primarily
      // useful when compiling large translation units or linking large
      // binaries that require so much memory that doing that in parallel with
      // other compilation/linking jobs is likely to summon the OOM killer.
      //
      vp.insert<bool> ("cc.serialize");

      return true;
    }

    bool
    core_guess_init (scope& rs,
                     scope&,
                     const location& loc,
                     bool first,
                     bool,
                     module_init_extra& extra)
    {
      tracer trace ("cc::core_guess_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      auto& h (extra.hints);

      // Load cc.core.vars.
      //
      load_module (rs, rs, "cc.core.vars", loc);

      // config.cc.{id,hinter}
      //
      // These values must be hinted.
      //
      {
        rs.assign<string> ("cc.id") = cast<string> (h["config.cc.id"]);
        rs.assign<string> ("cc.hinter") = cast<string> (h["config.cc.hinter"]);
      }

      // config.cc.target
      //
      // This value must be hinted.
      //
      {
        const auto& t (cast<target_triplet> (h["config.cc.target"]));

        // Also enter as cc.target.{cpu,vendor,system,version,class} for
        // convenience of access.
        //
        rs.assign<string> ("cc.target.cpu")     = t.cpu;
        rs.assign<string> ("cc.target.vendor")  = t.vendor;
        rs.assign<string> ("cc.target.system")  = t.system;
        rs.assign<string> ("cc.target.version") = t.version;
        rs.assign<string> ("cc.target.class")   = t.class_;

        rs.assign<target_triplet> ("cc.target") = t;
      }

      // config.cc.pattern
      //
      // This value could be hinted. Note that the hints may not be the same.
      //
      {
        rs.assign<string> ("cc.pattern") =
          cast_empty<string> (h["config.cc.pattern"]);
      }

      // config.cc.mode
      //
      // This value could be hinted. Note that the hints may not be the same.
      //
      {
        rs.assign<strings> ("cc.mode") =
          cast_empty<strings> (h["config.cc.mode"]);
      }

      // cc.runtime
      // cc.stdlib
      //
      rs.assign ("cc.runtime") = cast<string> (h["cc.runtime"]);
      rs.assign ("cc.stdlib") = cast<string> (h["cc.stdlib"]);

      return true;
    }

    bool
    core_config_init (scope& rs,
                      scope&,
                      const location& loc,
                      bool first,
                      bool,
                      module_init_extra& extra)
    {
      tracer trace ("cc::core_config_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      context& ctx (rs.ctx);

      // Load cc.core.guess.
      //
      load_module (rs, rs, "cc.core.guess", loc);

      // Configuration.
      //
      using config::lookup_config;

      // Adjust module priority (compiler).
      //
      config::save_module (rs, "cc", 250);

      // Note that we are not having a config report since it will just
      // duplicate what has already been printed by the hinting module.

      // config.cc.{p,c,l}options
      // config.cc.libs
      //
      // @@ Same nonsense as in module.
      //
      rs.assign ("cc.poptions") += cast_null<strings> (
        lookup_config (rs, "config.cc.poptions", nullptr));

      rs.assign ("cc.coptions") += cast_null<strings> (
        lookup_config (rs, "config.cc.coptions", nullptr));

      rs.assign ("cc.loptions") += cast_null<strings> (
        lookup_config (rs, "config.cc.loptions", nullptr));

      rs.assign ("cc.aoptions") += cast_null<strings> (
        lookup_config (rs, "config.cc.aoptions", nullptr));

      rs.assign ("cc.libs") += cast_null<strings> (
        lookup_config (rs, "config.cc.libs", nullptr));

      // config.cc.internal.scope
      //
      // Note: save omitted.
      //
      if (lookup l = lookup_config (rs, "config.cc.internal.scope"))
      {
        if (cast<string> (l) == "current")
          fail << "'current' value in config.cc.internal.scope";

        // This is necessary in case we are acting as bundle amalgamation.
        //
        rs.assign ("cc.internal.scope") = *l;
      }

      // config.cc.reprocess
      //
      // Note: save omitted.
      //
      if (lookup l = lookup_config (rs, "config.cc.reprocess"))
        rs.assign ("cc.reprocess") = *l;

      // config.cc.pkgconfig.sysroot
      //
      // Let's look it up instead of just marking for saving to make sure the
      // path is valid.
      //
      // Note: save omitted.
      //
      lookup_config (rs, "config.cc.pkgconfig.sysroot");

      // Load the bin.config module.
      //
      if (!cast_false<bool> (rs["bin.config.loaded"]))
      {
        // Prepare configuration hints (pretend it belongs to root scope).
        //
        variable_map h (rs);

        // Note that all these variables have already been registered.
        //
        h.assign ("config.bin.target") =
          cast<target_triplet> (rs["cc.target"]).representation ();

        if (auto l = extra.hints["config.bin.pattern"])
          h.assign ("config.bin.pattern") = cast<string> (l);

        init_module (rs, rs, "bin.config", loc, false /* optional */, h);
      }

      // Verify bin's target matches ours (we do it even if we loaded it
      // ourselves since the target can come from the configuration and not
      // our hint).
      //
      {
        const auto& ct (cast<target_triplet> (rs["cc.target"]));
        const auto& bt (cast<target_triplet> (rs["bin.target"]));

        if (bt != ct)
        {
          const auto& h (cast<string> (rs["cc.hinter"]));

          fail (loc) << h << " and bin module target mismatch" <<
            info << h << " target is " << ct <<
            info << "bin target is " << bt;
        }
      }

      // Load bin.* modules we may need (see core_init() below).
      //
      const string& tsys (cast<string> (rs["cc.target.system"]));

      load_module (rs, rs, "bin.ar.config", loc);

      if (tsys == "win32-msvc")
      {
        load_module (rs, rs, "bin.ld.config", loc);
        load_module (rs, rs, "bin.def", loc);
      }

      if (tsys == "mingw32")
        load_module (rs, rs, "bin.rc.config", loc);

      // Find the innermost outer core_module, if any.
      //
      const core_module* om (nullptr);
      for (const scope* s (&rs);
           (s = s->parent_scope ()->root_scope ()) != nullptr; )
      {
        if ((om = s->find_module<core_module> (core_module::name)) != nullptr)
          break;
      }

      auto& m (extra.set_module (new core_module (om)));

      // config.cc.compiledb.*
      //
      {
        // For config.cc.compiledb and config.cc.compiledb.name we only
        // consider a value in this root scope (if it's inherited from the
        // outer scope, then that's where it will be handled). One special
        // case is when it's specified on a scope that doesn't load the cc
        // module (including, ultimately, the global scope for a global
        // override). We handle it by assuming the value belongs to the
        // outermost amalgamation that loads the cc module.
        //
        // Note: cache the result.
        //
        auto find_outermost =
          [&rs, o = optional<pair<scope*, core_module*>> ()] () mutable
        {
          if (!o)
          {
            o = pair<scope*, core_module*> (&rs, nullptr);
            for (scope* s (&rs);
                 (s = s->parent_scope ()->root_scope ()) != nullptr; )
            {
              if (auto* m = s->find_module<core_module> (core_module::name))
              {
                o->first = s;
                o->second = m;
              }
            }
          }

          return *o;
        };

        auto belongs = [&rs, &find_outermost] (const lookup& l)
        {
          return l.belongs (rs) || find_outermost ().first == &rs;
        };

        // Add compilation databases specified in ns as <name>[@<path>] pairs,
        // appending their names to cdb_names. If <path> is absent, then place
        // the database into the base directory. Return the last added name.
        //
        auto add_cdbs = [&ctx,
                         &loc,
                         &trace] (strings& cdb_names,
                                  const names& ns,
                                  const dir_path& base) -> const string&
        {
          // Check that names and paths match. Return false if this entry
          // already exist.
          //
          // Note that before we also checked that the same paths are not used
          // across contexts. But, actually, there doesn't seem to be anything
          // wrong with that and this can actually be useful, for example,
          // when developing build system modules.
          //
          auto check = [&loc] (const string& n, const path& p)
          {
            for (const unique_ptr<compiledb>& db: compiledbs)
            {
              bool nm (db->name == n);
              bool pm (db->path == p);

              if (nm != pm)
                fail (loc) << "inconsistent compilation database names/paths" <<
                  info << p << " is called " << n <<
                  info << db->path << " is called " << db->name;

              if (nm)
                return false;
            }

            return true;
          };

          const string* r (&empty_string);

          bool reg (false);
          size_t j (compiledbs.size ()); // First newly added database.
          for (auto i (ns.begin ()); i != ns.end (); ++i)
          {
            // Each element has the <name>[@<path>] form.
            //
            // The special `-` <name> signifies stdout.
            //
            // If <path> is absent, then the file is called <name>.json and
            // placed into the output directory of the amalgamation or project
            // root scope (passed as the base argument).
            //
            // If <path> is (syntactically) a directory, then the file path is
            // <path>/<name>.json.
            //
            if (!i->simple () || i->empty ())
              fail (loc) << "invalid compilation database name '" << *i << "'";

            // Don't allow names that have (or are) directory components.
            //
            if (compiledb_name_to_path (*i))
              fail (loc) << "directory component in compilation database name '"
                         << *i << "'";

            string n (i->value);

            path p;
            if (i->pair)
            {
              ++i;

              if (n == "-")
                fail (loc) << "compilation database path specified for stdout "
                           << "name";
              try
              {
                if (i->directory ())
                  p = i->dir / n + ".json";
                else if (i->file ())
                {
                  if (i->dir.empty ())
                    p = path (i->value);
                  else
                    p = i->dir / i->value;
                }
                else
                  throw invalid_path ("");

                if (p.relative ())
                  p.complete ();

                p.normalize ();
              }
              catch (const invalid_path&)
              {
                fail (loc) << "invalid compilation database path '" << *i
                           << "'";
              }
            }
            else if (n != "-")
            {
              p = base / n + ".json";
            }

            if (check (n, p))
            {
              reg = compiledbs.empty (); // First time.

#ifdef BUILD2_BOOTSTRAP
              fail (loc) << "compilation database requested during bootstrap";
#else
              if (n == "-")
                compiledbs.push_back (
                  unique_ptr<compiledb> (
                    new compiledb_stdout (n)));
              else
                compiledbs.push_back (
                  unique_ptr<compiledb> (
                    new compiledb_file (n, move (p))));
#endif
            }

            // We may end up with duplicates via the config.cc.compiledb
            // logic.
            //
            auto k (find (cdb_names.begin (), cdb_names.end (), n));

            if (k == cdb_names.end ())
            {
              cdb_names.push_back (move (n));
              r = &cdb_names.back ();
            }
            else
              r = &*k;
          }

          // Register context operation callback for compiledb generation.
          //
          // We have two complications here:
          //
          // 1. We could be performing all this from the load phase that
          //    interrupted the match phase, which means the point where the
          //    pre callback would have been called is already gone (but the
          //    post callback will still be called). This will happen if we,
          //    say, import a project that has a compilation database from a
          //    project that doesn't.
          //
          //    (Note that if you think that this can be solved by simply
          //    always registering the callbacks, regardless of whether we
          //    have any databases or not, consider a slightly different
          //    scenario where we import a project that loads the cc module
          //    from a project that does not).
          //
          //    What we are going to do in this case is simply call the pre
          //    callback manually.
          //
          // 2. We could again be performing all this from the load phase that
          //    interrupted the match phase, but this time the pre callback
          //    has already been called, which means there will be no pre()
          //    call for the newly added database(s). This will happen if we,
          //    say, import a project that has a compilation database from a
          //    project that also has one.
          //
          //    Again, what we are going to do in this case is simply call the
          //    pre callback for the new database(s) manually.
          //
          if (reg)
            ctx.operation_callbacks.emplace (
              perform_update_id,
              context::operation_callback {&compiledb_pre, &compiledb_post});

          if (!ctx.phase_mutex.unlocked ()) // Interrupting load.
          {
            action a (ctx.current_action ());

            if (a.inner_action () == perform_update_id)
            {
              if (reg) // Case #1.
              {
                l6 ([&]{trace << "direct compiledb_pre for context " << &ctx;});
                compiledb_pre (ctx, a, action_targets {});
              }
              else     // Case #2.
              {
                size_t n (compiledbs.size ());

                if (j != n)
                {
                  l6 ([&]{trace << "additional compiledb for context " << &ctx;});

                  for (; j != n; ++j)
                    compiledbs[j]->pre (ctx);
                }
              }
            }
          }

          return *r;
        };

        lookup l;

        // config.cc.compiledb
        //
        // The semantics of this value is as follows:
        //
        // Location:    outermost amalgamation that loads the cc module.
        // Name filter: enable from this scope unless specified explicitly.
        // Type filter: enable from this scope unless specified explicitly.
        //
        // Note: save omitted.
        //
        optional<string> enable_filter;

        l = lookup_config (rs, "config.cc.compiledb", 0, &save_compiledb_name);
        if (l && belongs (l))
        {
          l6 ([&]{trace << "config.cc.compiledb specified on " << rs;});

          const names& ns (cast<names> (l));

          // Make sure it's one name/path.
          //
          size_t n (ns.size ());
          if (n == 0 || n != (ns.front ().pair ? 2 : 1))
            fail (loc) << "invalid compilation database name '" << ns << "'";

          // Detect and translate just <name> which is actually <path> to the
          // <name>@<path> form:
          //
          // - The <name> part is the name of the directory where the database
          //   file will reside (typically project/repository or package
          //   name).
          //
          // - If <path> is a directory, then the database name is
          //   compile_commands.json.
          //
          names tns;
          if (n == 1)
          {
            const name& n (ns.front ());

            if (optional<name> otn = compiledb_name_to_path (n))
            {
              name& tn (*otn);

              // Note: the add_cdbs() call below completes and normalizes the
              // path but we need to do it earlier in order to be able to
              // derive the name (the last component can be `.`/`..`).
              //
              if (tn.dir.relative ())
                tn.dir.complete ();

              tn.dir.normalize ();

              if (!exists (tn.dir))
                fail (loc) << "compilation database directory " << tn.dir
                           << " does not exist";

              if (tn.value.empty ())
                tn.value = "compile_commands.json";

              tns.push_back (name (tn.dir.leaf ().string ()));
              tns.back ().pair = '@';
              tns.push_back (move (tn));
            }
          }

          // We inject the database directly into the outer amalgamation's
          // module, as-if config.cc.compiledb.name was specified in its
          // scope. Unless there isn't one, in which case it's us.
          //
          pair<scope*, core_module*> p (find_outermost ());

          // Save the name for the name filter below.
          //
          enable_filter = add_cdbs (
            (p.second != nullptr ? *p.second : m).cdb_names_,
            tns.empty () ? ns : tns,
            p.first->out_path ());
        }

        // config.cc.compiledb.name
        //
        // Note: save omitted.
        //
        l = lookup_config (rs,
                           "config.cc.compiledb.name",
                           0,
                           &save_compiledb_name);
        if (l && belongs (l))
        {
          l6 ([&]{trace << "config.cc.compiledb.name specified on " << rs;});

          add_cdbs (m.cdb_names_, cast<names> (l), rs.out_path ());
        }

        // config.cc.compiledb.filter
        //
        // Note: save omitted.
        //
        l = lookup_config (rs, "config.cc.compiledb.filter");
        if (l && belongs (l)) // Custom.
        {
          m.cdb_filter_ = &cast<compiledb_name_filter> (l);
        }
        else if (enable_filter) // Override.
        {
          // Inherit outer filter.
          //
          if (om != nullptr && om->cdb_filter_ != nullptr)
            m.cdb_filter_storage_ = *om->cdb_filter_;

          m.cdb_filter_storage_.emplace_back (*enable_filter, true);
          m.cdb_filter_ = &m.cdb_filter_storage_;
        }
        else if (om != nullptr) // Inherit.
        {
          m.cdb_filter_ = om->cdb_filter_;
        }

        // config.cc.compiledb.filter.input
        // config.cc.compiledb.filter.output
        //
        // Note that filtering happens before we take into account the change
        // status, which means for larger projects there would be a lot of
        // targets to filter even during the incremental update. So it feels
        // it would have been better to pre-lookup the target types. However,
        // the targets that would normally be used are registered by other
        // modules (bin, c/cxx) and which haven't been loaded yet. So instead
        // we try to optimize the lookup for the commonly used targets.
        //
        // Note: save omitted.
        //
        l = lookup_config (rs, "config.cc.compiledb.filter.input");
        if (l && belongs (l)) // Custom.
        {
          m.cdb_filter_input_ = &cast<compiledb_type_filter> (l);
        }
        else if (enable_filter) // Override.
        {
          // Inherit outer filter.
          //
          if (om != nullptr && om->cdb_filter_input_ != nullptr)
          {
            m.cdb_filter_input_storage_ = *om->cdb_filter_input_;
            m.cdb_filter_input_storage_.emplace_back (*enable_filter, "target");
            m.cdb_filter_input_ = &m.cdb_filter_input_storage_;
          }
          else
            m.cdb_filter_input_ = nullptr; // Enable all.
        }
        else if (om != nullptr) // Inherit.
        {
          m.cdb_filter_input_ = om->cdb_filter_input_;
        }

        l = lookup_config (rs, "config.cc.compiledb.filter.output");
        if (l && belongs (l)) // Custom.
        {
          m.cdb_filter_output_ = &cast<compiledb_type_filter> (l);
        }
        else if (enable_filter) // Override.
        {
          // Inherit outer filter.
          //
          if (om != nullptr && om->cdb_filter_output_ != nullptr)
          {
            m.cdb_filter_output_storage_ = *om->cdb_filter_output_;
            m.cdb_filter_output_storage_.emplace_back (*enable_filter, "target");
            m.cdb_filter_output_ = &m.cdb_filter_output_storage_;
          }
          else
            m.cdb_filter_output_ = nullptr; // Enable all.
        }
        else if (om != nullptr) // Inherit.
        {
          m.cdb_filter_output_ = om->cdb_filter_output_;
        }
      }

      // Register scope operation callback for cleaning module sidebuilds and
      // compilation databases.
      //
      // It feels natural to clean this stuff up as a post operation but that
      // prevents the (otherwise-empty) out root directory to be cleaned up
      // (via the standard fsdir{} chain).
      //
      rs.operation_callbacks.emplace (
        perform_clean_id,
        scope::operation_callback {&clean_callback, nullptr /*post*/});

      return true;
    }

    bool
    core_init (scope& rs,
               scope&,
               const location& loc,
               bool first,
               bool,
               module_init_extra& extra)
    {
      tracer trace ("cc::core_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      const string& tsys (cast<string> (rs["cc.target.system"]));

      // Load cc.core.config.
      //
      load_module (rs, rs, "cc.core.config", loc, extra.hints);

      // Load the bin module.
      //
      load_module (rs, rs, "bin", loc);

      // Load the bin.ar module.
      //
      load_module (rs, rs, "bin.ar", loc);

      // For this target we link things directly with link.exe so load the
      // bin.ld module.
      //
      if (tsys == "win32-msvc")
        load_module (rs, rs, "bin.ld", loc);

      // If our target is MinGW, then we will need the resource compiler
      // (windres) in order to embed manifests into executables.
      //
      if (tsys == "mingw32")
        load_module (rs, rs, "bin.rc", loc);

      return true;
    }

    // The cc module is an "alias" for c and cxx. Its intended use is to make
    // sure that the C/C++ configuration is captured in an amalgamation rather
    // than subprojects.
    //
    static inline bool
    init_alias (tracer& trace,
                scope& rs,
                scope& bs,
                const char* m,
                const char* c,
                const char* c_loaded,
                const char* cxx,
                const char* cxx_loaded,
                const location& loc,
                const variable_map& hints)
    {
      l5 ([&]{trace << "for " << bs;});

      // We only support root loading (which means there can only be one).
      //
      if (rs != bs)
        fail (loc) << m << " module must be loaded in project root";

      // We want to order the loading to match what user specified on the
      // command line (config.c or config.cxx). This way the first loaded
      // module (with user-specified config.*) will hint the compiler to the
      // second.
      //
      bool lc (!cast_false<bool> (rs[c_loaded]));
      bool lp (!cast_false<bool> (rs[cxx_loaded]));

      // If none of them are already loaded, load c first only if config.c
      // is specified.
      //
      if (lc && lp && rs["config.c"])
      {
        init_module (rs, rs, c,   loc, false /* optional */, hints);
        init_module (rs, rs, cxx, loc, false /* optional */, hints);
      }
      else
      {
        if (lp) init_module (rs, rs, cxx, loc, false, hints);
        if (lc) init_module (rs, rs, c,   loc, false, hints);
      }

      return true;
    }

    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& loc,
                 bool,
                 bool,
                 module_init_extra& extra)
    {
      tracer trace ("cc::config_init");
      return init_alias (trace, rs, bs,
                         "cc.config",
                         "c.config",   "c.config.loaded",
                         "cxx.config", "cxx.config.loaded",
                         loc, extra.hints);
    }

    bool
    init (scope& rs,
          scope& bs,
          const location& loc,
          bool,
          bool,
          module_init_extra& extra)
    {
      tracer trace ("cc::init");
      return init_alias (trace, rs, bs,
                         "cc",
                         "c",   "c.loaded",
                         "cxx", "cxx.loaded",
                         loc, extra.hints);
    }

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"cc.core.vars",   nullptr, core_vars_init},
      {"cc.core.guess",  nullptr, core_guess_init},
      {"cc.core.config", nullptr, core_config_init},
      {"cc.core",        nullptr, core_init},
      {"cc.config",      nullptr, config_init},
      {"cc",             nullptr, init},
      {nullptr,          nullptr, nullptr}
    };

    const module_functions*
    build2_cc_load ()
    {
      return mod_functions;
    }
  }
}
