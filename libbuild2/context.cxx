// file      : libbuild2/context.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/context.hxx>

#include <sstream>
#include <exception> // uncaught_exception[s]()

#include <libbuild2/rule.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbutl/ft/exception.hxx> // uncaught_exceptions

// For command line variable parsing.
//
#include <libbuild2/token.hxx>
#include <libbuild2/lexer.hxx>
#include <libbuild2/parser.hxx>

#include <libbuild2/config/utility.hxx> // config_preprocess_create

using namespace std;
using namespace butl;

namespace build2
{
  // Create global scope. Note that the empty path is a prefix for any other
  // path. See the comment in <libbutl/prefix-map.hxx> for details.
  //
  static inline scope&
  create_global_scope (scope_map& m)
  {
    auto i (m.insert_out (dir_path ()));
    scope& r (*i->second.front ());
    r.out_path_ = &i->first;
    return r;
  };

  struct context::data
  {
    scope_map scopes;
    target_set targets;
    variable_pool var_pool;
    variable_patterns var_patterns;
    variable_overrides var_overrides;
    function_map functions;

    target_type_map global_target_types;
    variable_override_cache global_override_cache;
    strings global_var_overrides;

    data (context& c)
        : scopes (c),
          targets (c),
          var_pool (&c /* shared */, nullptr /* outer */, &var_patterns),
          var_patterns (&c /* shared */, &var_pool) {}
  };

  void context::
  reserve (reserves res)
  {
    assert (phase == run_phase::load);

    if (res.targets != 0)
      data_->targets.map_.reserve (res.targets);

    if (res.variables != 0)
      data_->var_pool.map_.reserve (res.variables);
  }

  pair<char, variable_override> context::
  parse_variable_override (const string& s, size_t i, bool buildspec)
  {
    istringstream is (s);
    is.exceptions (istringstream::failbit | istringstream::badbit);

    // Similar to buildspec we do "effective escaping" of the special `'"\$(`
    // characters (basically what's escapable inside a double-quoted literal
    // plus the single quote; note, however, that we exclude line
    // continuations and `)` since they would make directory paths on Windows
    // unusable).
    //
    path_name in ("<cmdline>");
    lexer l (is, in, 1 /* line */, "\'\"\\$(");

    // At the buildfile level the scope-specific variable should be separated
    // from the directory with a whitespace, for example:
    //
    // ./ foo=$bar
    //
    // However, requiring this for command line variables would be too
    // inconvinient so we support both.
    //
    // We also have the optional visibility modifier as a first character of
    // the variable name:
    //
    // ! - global
    // % - project
    // / - scope
    //
    // The last one clashes a bit with the directory prefix:
    //
    // ./ /foo=bar
    // .//foo=bar
    //
    // But that's probably ok (the need for a scope-qualified override with
    // scope visibility should be pretty rare). Note also that to set the
    // value on the global scope we use !.
    //
    // And so the first token should be a word which can be either a variable
    // name (potentially with the directory qualification) or just the
    // directory, in which case it should be followed by another word
    // (unqualified variable name). To avoid treating any of the visibility
    // modifiers as special we use the cmdvar mode.
    //
    l.mode (lexer_mode::cmdvar);
    token t (l.next ());

    optional<dir_path> dir;
    if (t.type == token_type::word)
    {
      string& v (t.value);
      size_t p (path::traits_type::rfind_separator (v));

      if (p != string::npos && p != 0) // If first then visibility.
      {
        if (p == v.size () - 1)
        {
          // Separate directory.
          //
          dir = dir_path (move (v));
          t = l.next ();

          // Target-specific overrides are not yet supported (and probably
          // never will be; the beast is already complex enough).
          //
          if (t.type == token_type::colon)
          {
            diag_record dr (fail);

            dr << "'" << s << "' is a target-specific override";

            if (buildspec)
              dr << info << "use double '--' to treat this argument as "
                 << "buildspec";
          }
        }
        else
        {
          // Combined directory.
          //
          // If double separator (visibility marker), then keep the first in
          // name.
          //
          if (p != 0 && path::traits_type::is_separator (v[p - 1]))
            --p;

          dir = dir_path (t.value, 0, p + 1); // Include the separator.
          t.value.erase (0, p + 1);           // Erase the separator.
        }

        if (dir->relative ())
        {
          // Handle the special relative to base scope case (.../).
          //
          auto i (dir->begin ());

          if (*i == "...")
            dir = dir_path (++i, dir->end ()); // Note: can become empty.
          else
            dir->complete (); // Relative to CWD.
        }

        if (dir->absolute ())
          dir->normalize ();
      }
    }

    token_type tt (l.next ().type);

    // The token should be the variable name followed by =, +=, or =+.
    //
    if (t.type != token_type::word || t.value.empty () ||
        (tt != token_type::assign  &&
         tt != token_type::prepend &&
         tt != token_type::append))
    {
      diag_record dr (fail);

      dr << "expected variable assignment instead of '" << s << "'";

      if (buildspec)
        dr << info << "use double '--' to treat this argument as buildspec";
    }

    // Take care of the visibility. Note that here we rely on the fact that
    // none of these characters are lexer's name separators.
    //
    char c (t.value[0]);

    if (path::traits_type::is_separator (c))
      c = '/'; // Normalize.

    string n (t.value, c == '!' || c == '%' || c == '/' ? 1 : 0);

    // Make sure it is qualified.
    //
    // We can support overridable public unqualified variables (which must
    // all be pre-entered by the end of this constructor) but we will need
    // to detect their names here in an ad hoc manner (we cannot enter them
    // before this logic because of the "untyped override" requirement).
    //
    // Note: issue the same diagnostics as in variable_pool::update().
    //
    if (n.find ('.') == string::npos)
      fail << "variable " << n << " cannot be overridden";

    if (c == '!' && dir)
      fail << "scope-qualified global override of variable " << n;

    // Pre-enter the main variable. Note that we rely on all the overridable
    // variables with global visibility to be known (either entered or
    // handled via a pattern) at this stage.
    //
    variable_pool& vp (data_->var_pool);
    variable& var (
      const_cast<variable&> (vp.insert (n, true /* overridable */)));

    const variable* o;
    {
      variable_visibility v (c == '/' ? variable_visibility::scope   :
                             c == '%' ? variable_visibility::project :
                             variable_visibility::global);

      const char* k (tt == token_type::assign ? "__override" :
                     tt == token_type::append ? "__suffix" : "__prefix");

      unique_ptr<variable> p (
        new variable {
          n + '.' + to_string (i + 1) + '.' + k,
          &vp     /* owner */,
          nullptr /* aliases   */,
          nullptr /* type      */,
          nullptr /* overrides */,
          v});

      // Back link.
      //
      p->aliases = p.get ();
      if (var.overrides != nullptr)
        swap (p->aliases,
              const_cast<variable*> (var.overrides.get ())->aliases);

      // Forward link.
      //
      p->overrides = move (var.overrides);
      var.overrides = move (p);

      o = var.overrides.get ();
    }

    // Currently we expand project overrides in the global scope to keep
    // things simple. Pass original variable for diagnostics. Use current
    // working directory as pattern base.
    //
    scope& gs (global_scope.rw ());

    parser p (*this);
    pair<value, token> r (p.parse_variable_value (l, gs, &work, var));

    if (r.second.type != token_type::eos)
      fail << "unexpected " << r.second << " in variable assignment "
           << "'" << s << "'";

    // Make sure the value is not typed.
    //
    if (r.first.type != nullptr)
      fail << "typed override of variable " << n;

    return make_pair (
      c,
      variable_override {var, *o, move (dir), move (r.first)});
  }

  context::
  context (scheduler& s,
           global_mutexes& ms,
           file_cache& fc,
           optional<match_only_level> mo,
           bool nem,
           bool dr,
           bool ndb,
           bool kg,
           const strings& cmd_vars,
           reserves res,
           optional<context*> mc,
           const module_libraries_lock* ml,
           const function<var_override_function>& var_ovr_func)
      : data_ (new data (*this)),
        sched (&s),
        mutexes (&ms),
        fcache (&fc),
        match_only (mo),
        no_external_modules (nem),
        dry_run_option (dr),
        no_diag_buffer (ndb),
        keep_going (kg),
        phase_mutex (*this),
        scopes (data_->scopes),
        targets (data_->targets),
        var_pool (data_->var_pool),
        var_patterns (data_->var_patterns),
        var_overrides (data_->var_overrides),
        functions (data_->functions),
        global_scope (create_global_scope (data_->scopes)),
        global_target_types (data_->global_target_types),
        global_override_cache (data_->global_override_cache),
        global_var_overrides (data_->global_var_overrides),
        modules_lock (ml),
        module_context (mc ? *mc : nullptr),
        module_context_storage (mc
                                ? optional<unique_ptr<context>> (nullptr)
                                : nullopt)
  {
    // NOTE: see also the bare minimum version below if adding anything here.

    tracer trace ("context");

    l6 ([&]{trace << "initializing build state";});

    reserve (res);

    scope_map& sm (data_->scopes);
    variable_pool& vp (data_->var_pool);
    variable_patterns& vpats (data_->var_patterns);

    insert_builtin_functions (functions);

    // Initialize the meta/operation tables. Note that the order should match
    // the id constants in <libbuild2/operation.hxx>.
    //
    meta_operation_table.insert ("noop");
    meta_operation_table.insert ("perform");
    meta_operation_table.insert ("configure"); // bpkg assumes no process.
    meta_operation_table.insert ("disfigure");

    if (config_preprocess_create != nullptr)
      meta_operation_table.insert (
        meta_operation_data ("create", config_preprocess_create));

    meta_operation_table.insert ("dist");
    meta_operation_table.insert ("info");

    operation_table.clear ();
    operation_table.insert ("default");
    operation_table.insert ("update");
    operation_table.insert ("clean");
    operation_table.insert ("test");
    operation_table.insert ("update-for-test");
    operation_table.insert ("install");
    operation_table.insert ("uninstall");
    operation_table.insert ("update-for-install");

    // Setup the global scope before parsing any variable overrides since they
    // may reference these things.
    //
    scope& gs (global_scope.rw ());
    {
      const auto v_g (variable_visibility::global);

      // Any variable assigned on the global scope should natually have the
      // global visibility.
      //
      auto set = [&gs, &vp] (const char* var, auto val) -> const value&
      {
        using T = decltype (val);
        value& v (gs.assign (vp.insert<T> (var, variable_visibility::global)));
        v = move (val);
        return v;
      };

      // Build system mode.
      //
      // This value signals any special mode the build system may be running
      // in. The two core modes are `no-external-modules` (bootstrapping of
      // external modules is disabled) and `normal` (normal build system
      // execution). Build system drivers may invent additional modes (for
      // example, the bpkg `skeleton` mode that is used to evaluate depends
      // clauses).
      //
      set ("build.mode",
           no_external_modules ? "no-external-modules" : "normal");

      set ("build.work", work);
      set ("build.home", home);

      // Build system driver process path.
      //
      set ("build.path",
           process_path (nullptr, // Will be filled by value assignment.
                         path (argv0.recall_string ()),
                         path (argv0.effect)));

      // Build system import path for modules. We only set it for the
      // development build.
      //
      var_import_build2 = &vp.insert<abs_dir_path> ("import.build2", v_g);

      if (!build_installed)
      {
#ifdef BUILD2_IMPORT_PATH
        gs.assign (var_import_build2) = abs_dir_path (BUILD2_IMPORT_PATH);
#endif
      }

      // Build system verbosity level.
      //
      set ("build.verbosity", uint64_t (verb));

      // Build system diagnostics progress and color.
      //
      // Note that these can be true, false, or NULL if neither requested nor
      // suppressed explicitly.
      //
      {
        value& v (gs.assign (vp.insert<bool> ("build.progress", v_g)));
        if (diag_progress_option)
          v = *diag_progress_option;
      }

      {
        value& v (gs.assign (vp.insert<bool> ("build.diag_color", v_g)));
        if (diag_color_option)
          v = *diag_color_option;
      }

      // These are the "effective" values that incorporate a suitable default
      // if neither requested nor suppressed explicitly.
      //
      set ("build.show_progress",   show_progress (verb_never));
      set ("build.show_diag_color", show_diag_color ());

      // Build system version (similar to what we do in the version module
      // except here we don't include package epoch/revision).
      //
      const standard_version& v (build_version);

      // Note: here we assume epoch will always be 1 and therefore omit the
      //       project_ prefix in a few places.
      //
      set ("build.version", v.string_project ());

      set ("build.version.number", v.version);
      set ("build.version.id",     v.string_project_id ());

      set ("build.version.major", uint64_t (v.major ()));
      set ("build.version.minor", uint64_t (v.minor ()));
      set ("build.version.patch", uint64_t (v.patch ()));

      optional<uint16_t> a (v.alpha ());
      optional<uint16_t> b (v.beta ());

      set ("build.version.alpha",              a.has_value ());
      set ("build.version.beta",               b.has_value ());
      set ("build.version.pre_release",        v.pre_release ().has_value ());
      set ("build.version.pre_release_string", v.string_pre_release ());
      set ("build.version.pre_release_number", uint64_t (a ? *a : b ? *b : 0));

      set ("build.version.snapshot",        v.snapshot ()); // bool
      set ("build.version.snapshot_sn",     v.snapshot_sn); // uint64
      set ("build.version.snapshot_id",     v.snapshot_id); // string
      set ("build.version.snapshot_string", v.string_snapshot ());

      // Build system interface version. In particular, it is embedded into
      // build system modules as load_suffix.
      //
      set ("build.version.interface", build_version_interface);

      // Allow detection (for example, in tests) whether this is a staged
      // toolchain.
      //
      // Note that it is either staged or public, without queued, since we do
      // not re-package things during the queued-to-public transition.
      //
      set ("build.version.stage", LIBBUILD2_STAGE);

      // Enter the host information. Rather than jumping through hoops like
      // config.guess, for now we are just going to use the compiler target we
      // were built with. While it is not as precise (for example, a binary
      // built for i686 might be running on x86_64), it is good enough of an
      // approximation/fallback since most of the time we are interested in
      // just the target class (e.g., linux, windows, macos).
      //

      // Did the user ask us to use config.guess?
      //
      string orig (config_guess
                   ? run<string> (*this,
                                  3,
                                  *config_guess,
                                  [](string& l, bool) {return move (l);})
                   : BUILD2_HOST_TRIPLET);

      l5 ([&]{trace << "original host: '" << orig << "'";});

      try
      {
        target_triplet t (orig);

        l5 ([&]{trace << "canonical host: '" << t.string () << "'; "
                      << "class: " << t.class_;});

        // Also enter as build.host.{cpu,vendor,system,version,class} for
        // convenience of access.
        //
        set ("build.host.cpu",     t.cpu);
        set ("build.host.vendor",  t.vendor);
        set ("build.host.system",  t.system);
        set ("build.host.version", t.version);
        set ("build.host.class",   t.class_);

        build_host = &set ("build.host", move (t)).as<target_triplet> ();
      }
      catch (const invalid_argument& e)
      {
        fail << "unable to parse build host '" << orig << "': " << e <<
          info << "consider using the --config-guess option";
      }

      var_build_meta_operation =
        &vp.insert<string> ("build.meta_operation", v_g);
    }

    // Register builtin target types.
    //
    {
      target_type_map& t (data_->global_target_types);

      // These are abstract.
      //
      t.insert<target>       ();
      t.insert<mtime_target> ();
      t.insert<path_target>  ();

      t.insert<file>         ();
      t.insert<group>        ();
      t.insert<alias>        ();
      t.insert<dir>          ();
      t.insert<fsdir>        ();
      t.insert<exe>          ();
      t.insert<doc>          ();
      t.insert<legal>        ();
      t.insert<man>          ();
      t.insert<man1>         ();

      {
        auto& tt (t.insert<manifest> ());
        t.insert_file ("manifest", tt);
      }

      {
        auto& tt (t.insert<buildfile> ());
        t.insert_file ("buildfile", tt);
      }
    }

    // Enter builtin variable patterns.
    //
    // Note that we must do global visibility prior to entering overrides
    // below but they cannot be typed. So it's a careful dance.
    //
    const auto v_g (variable_visibility::global);

    // All config.** variables are overridable with global visibility.
    //
    // For the config.**.configured semantics, see config::unconfigured().
    //
    // Note that some config.config.* variables have project visibility thus
    // the match argument is false.
    //
    vpats.insert ("config.**", nullopt, true, v_g, true, false);

    // Parse and enter the command line variables. We do it before entering
    // any other variables so that all the variables that are overriden are
    // marked as such first. Then, as we enter variables, we can verify that
    // the override is alowed.
    //
    {
      size_t i (0);
      for (; i != cmd_vars.size (); ++i)
      {
        const string& s (cmd_vars[i]);

        pair<char, variable_override> p (
          parse_variable_override (s, i, true /* buildspec */));

        char c (p.first);
        variable_override& vo (p.second);

        // Global and absolute scope overrides we can enter directly. Project
        // and relative scope ones will be entered later for each project.
        //
        if (c == '!' || (vo.dir && vo.dir->absolute ()))
        {
          scope& s (c == '!' ? gs : *sm.insert_out (*vo.dir)->second.front ());

          auto p (s.vars.insert (vo.ovr));
          assert (p.second); // Variable name is unique.

          value& v (p.first);
          v = move (vo.val);
        }
        else
          data_->var_overrides.push_back (move (vo));

        // Save global overrides for nested contexts.
        //
        if (c == '!')
          data_->global_var_overrides.push_back (s);
      }

      // Parse any ad hoc project-wide overrides.
      //
      if (var_ovr_func != nullptr)
        var_ovr_func (*this, i);
    }

    // Enter remaining variable patterns and builtin variables.
    //
    const auto v_p (variable_visibility::project);
    const auto v_t (variable_visibility::target);
    const auto v_q (variable_visibility::prereq);

    vpats.insert<bool> ("config.**.configured", false, v_p);

    // file.cxx:import()
    //
    // Note: the order is important (see variable_patterns::insert()).
    //
    // Note that if any are overriden, they are "pre-typed" by the config.**
    // pattern above and we just "add" the types.
    //
    vpats.insert<abs_dir_path> ("config.import.*",  true, v_g, true);
    vpats.insert<path>         ("config.import.**", true, v_g, true);

    // module.cxx:boot/init_module().
    //
    // Note that we also have the config.<module>.configured variable (see
    // above).
    //
    vpats.insert<bool> ("**.booted",     false /* overridable */, v_p);
    vpats.insert<bool> ("**.loaded",     false,                   v_p);
    vpats.insert<bool> ("**.configured", false,                   v_p);

    var_src_root = &vp.insert<dir_path> ("src_root");
    var_out_root = &vp.insert<dir_path> ("out_root");
    var_src_base = &vp.insert<dir_path> ("src_base");
    var_out_base = &vp.insert<dir_path> ("out_base");

    var_forwarded = &vp.insert<bool> ("forwarded");

    // Note that subprojects is not typed since the value requires
    // pre-processing (see file.cxx).
    //
    var_project      = &vp.insert<project_name> ("project");
    var_amalgamation = &vp.insert<dir_path>     ("amalgamation");
    var_subprojects  = &vp.insert               ("subprojects"); // Untyped.
    var_version      = &vp.insert<string>       ("version");

    var_project_url     = &vp.insert<string> ("project.url");
    var_project_summary = &vp.insert<string> ("project.summary");

    var_import_target   = &vp.insert<name>     ("import.target");
    var_import_metadata = &vp.insert<uint64_t> ("import.metadata");

    var_export_metadata = &vp.insert ("export.metadata", v_t); // Untyped.

    var_extension = &vp.insert<string> ("extension", v_t);
    var_update    = &vp.insert<string> ("update",    v_q);
    var_clean     = &vp.insert<bool>   ("clean",     v_t);
    var_backlink  = &vp.insert         ("backlink",  v_t); // Untyped.
    var_include   = &vp.insert<string> ("include",   v_q);

    // Backlink executables and (generated) documentation by default.
    //
    gs.target_vars[exe::static_type]["*"].assign (var_backlink) =
      names {name ("true")};
    gs.target_vars[doc::static_type]["*"].assign (var_backlink) =
      names {name ("true")};

    // Register builtin rules.
    //
    {
      rule_map& r (gs.rules); // Note: global scope!

      r.insert<alias> (perform_id, 0, "build.alias", alias_rule::instance);

      r.insert<fsdir> (perform_update_id, "build.fsdir", fsdir_rule::instance);
      r.insert<fsdir> (perform_clean_id,  "build.fsdir", fsdir_rule::instance);

      r.insert<mtime_target> (perform_update_id, "build.file", file_rule::instance);
      r.insert<mtime_target> (perform_clean_id,  "build.file", file_rule::instance);
    }

    // End of initialization.
    //
    load_generation = 1;
  }

  context::
  context ()
      : data_ (new data (*this)),
        sched (nullptr),
        mutexes (nullptr),
        fcache (nullptr),
        match_only (nullopt),
        no_external_modules (true),
        dry_run_option (false),
        no_diag_buffer (false),
        keep_going (false),
        phase_mutex (*this),
        scopes (data_->scopes),
        targets (data_->targets),
        var_pool (data_->var_pool),
        var_patterns (data_->var_patterns),
        var_overrides (data_->var_overrides),
        functions (data_->functions),
        global_scope (create_global_scope (data_->scopes)),
        global_target_types (data_->global_target_types),
        global_override_cache (data_->global_override_cache),
        global_var_overrides (data_->global_var_overrides),
        modules_lock (nullptr),
        module_context (nullptr)
  {
    variable_pool& vp (data_->var_pool);

    var_src_root = &vp.insert<dir_path> ("src_root");
    var_out_root = &vp.insert<dir_path> ("out_root");

    var_project      = &vp.insert<project_name> ("project");
    var_amalgamation = &vp.insert<dir_path>     ("amalgamation");

    load_generation = 1;
  }

  context::
  ~context ()
  {
    // Cannot be inline since context::data is undefined.
  }

  void context::
  enter_project_overrides (scope& rs,
                           const dir_path& out_base,
                           const variable_overrides& ovrs,
                           scope* as)
  {
    // The mildly tricky part here is to distinguish the situation where we
    // are bootstrapping the same project multiple times. The first override
    // that we set cannot already exist (because the override variable names
    // are unique) so if it is already set, then it can only mean this project
    // is already bootstrapped.
    //
    // This is further complicated by the project vs amalgamation logic (we
    // may have already done the amalgamation but not the project).  So we
    // split it into two passes.
    //
    auto& sm (scopes.rw ());

    for (const variable_override& o: ovrs)
    {
      if (o.ovr.visibility != variable_visibility::global)
        continue;

      // If we have a directory, enter the scope, similar to how we do
      // it in the context ctor.
      //
      scope& s (
        o.dir
        ? *sm.insert_out ((out_base / *o.dir).normalize ())->second.front ()
        : *(as != nullptr ? as : (as = rs.weak_scope ())));

      auto p (s.vars.insert (o.ovr));

      if (!p.second)
        break;

      value& v (p.first);
      v = o.val;
    }

    for (const variable_override& o: ovrs)
    {
      // Ours is either project (%foo) or scope (/foo).
      //
      if (o.ovr.visibility == variable_visibility::global)
        continue;

      scope& s (
        o.dir
        ? *sm.insert_out ((out_base / *o.dir).normalize ())->second.front ()
        : rs);

      auto p (s.vars.insert (o.ovr));

      if (!p.second)
        break;

      value& v (p.first);
      v = o.val;
    }
  }

  void context::
  current_meta_operation (const meta_operation_info& mif)
  {
    if (current_mname != mif.name)
    {
      current_mname = mif.name;
      global_scope.rw ().assign (var_build_meta_operation) = mif.name;
    }

    current_mif = &mif;
    current_mdata = current_data_ptr (nullptr, null_current_data_deleter);
    current_on = 0; // Reset.
  }

  void context::
  current_operation (const operation_info& inner_oif,
                     const operation_info* outer_oif,
                     bool diag_noise)
  {
    const auto& oif (outer_oif == nullptr ? inner_oif : *outer_oif);

    current_oname = oif.name;
    current_inner_oif = &inner_oif;
    current_outer_oif = outer_oif;
    current_inner_odata = current_data_ptr (nullptr, null_current_data_deleter);
    current_outer_odata = current_data_ptr (nullptr, null_current_data_deleter);
    current_on++;
    current_mode = inner_oif.mode;
    current_diag_noise = diag_noise;

    // Reset counters (serial execution).
    //
    dependency_count.store (0, memory_order_relaxed);
    target_count.store (0, memory_order_relaxed);
    skip_count.store (0, memory_order_relaxed);
    resolve_count.store (0, memory_order_relaxed);

    // Clear accumulated targets with post hoc prerequisites.
    //
    current_posthoc_targets.clear ();
  }

  bool run_phase_mutex::
  lock (run_phase n)
  {
    bool r;

    {
      mlock l (m_);
      bool u (lc_ == 0 && mc_ == 0 && ec_ == 0); // Unlocked.

      // Increment the counter.
      //
      condition_variable* v (nullptr);
      switch (n)
      {
      case run_phase::load:    lc_++; v = &lv_; break;
      case run_phase::match:   mc_++; v = &mv_; break;
      case run_phase::execute: ec_++; v = &ev_; break;
      }

      // If unlocked, switch directly to the new phase. Otherwise wait for the
      // phase switch. Note that in the unlocked case we don't need to notify
      // since there is nobody waiting (all counters are zero).
      //
      if (u)
      {
        ctx_.phase = n;
        r = !fail_;
      }
      else if (ctx_.phase != n)
      {
        ++contention; // Protected by m_.

        ctx_.sched->deactivate (false /* external */);
        for (; ctx_.phase != n; v->wait (l)) ;
        r = !fail_;
        l.unlock (); // Important: activate() can block.
        ctx_.sched->activate (false /* external */);
      }
      else
        r = !fail_;
    }

    // In case of load, acquire the exclusive access mutex.
    //
    if (n == run_phase::load)
    {
      if (!lm_.try_lock ())
      {
        ctx_.sched->deactivate (false /* external */);
        lm_.lock ();
        ctx_.sched->activate (false /* external */);

        ++contention_load; // Protected by lm_.
      }
      r = !fail_; // Re-query.
    }

    return r;
  }

  void run_phase_mutex::
  unlock (run_phase o)
  {
    // In case of load, release the exclusive access mutex.
    //
    if (o == run_phase::load)
      lm_.unlock ();

    {
      mlock l (m_);

      // Decrement the counter and see if this phase has become unlocked.
      //
      bool u (false);
      switch (o)
      {
      case run_phase::load:    u = (--lc_ == 0); break;
      case run_phase::match:   u = (--mc_ == 0); break;
      case run_phase::execute: u = (--ec_ == 0); break;
      }

      // If the phase became unlocked, pick a new phase and notify the
      // waiters. Note that we notify all load waiters so that they can all
      // serialize behind the second-level mutex.
      //
      if (u)
      {
        run_phase n;
        condition_variable* v;
        if      (lc_ != 0) {n = run_phase::load;    v = &lv_;}
        else if (mc_ != 0) {n = run_phase::match;   v = &mv_;}
        else if (ec_ != 0) {n = run_phase::execute; v = &ev_;}
        else               {n = run_phase::load;    v = nullptr;}

        ctx_.phase = n;

        // Enter/leave scheduler sub-phase. See also the other half in
        // relock().
        //
        if (o == run_phase::match && n == run_phase::execute)
          ctx_.sched->push_phase ();
        else if (o == run_phase::execute && n == run_phase::match)
          ctx_.sched->pop_phase ();

        if (v != nullptr)
        {
          l.unlock ();
          v->notify_all ();
        }
      }
    }
  }

  optional<bool> run_phase_mutex::
  relock (run_phase o, run_phase n)
  {
    // Pretty much a fused unlock/lock implementation except that we always
    // switch into the new phase.
    //
    assert (o != n);

    bool r;
    bool s (true); // True switch.

    if (o == run_phase::load)
      lm_.unlock ();

    {
      mlock l (m_);
      bool u (false);

      switch (o)
      {
      case run_phase::load:    u = (--lc_ == 0); break;
      case run_phase::match:   u = (--mc_ == 0); break;
      case run_phase::execute: u = (--ec_ == 0); break;
      }

      // Set if will be waiting or notifying others.
      //
      condition_variable* v (nullptr);
      switch (n)
      {
      case run_phase::load:    v = lc_++ != 0 || !u ? &lv_ : nullptr; break;
      case run_phase::match:   v = mc_++ != 0 || !u ? &mv_ : nullptr; break;
      case run_phase::execute: v = ec_++ != 0 || !u ? &ev_ : nullptr; break;
      }

      if (u)
      {
        ctx_.phase = n;
        r = !fail_;

        // Enter/leave scheduler sub-phase. See also the other half in
        // unlock().
        //
        if (o == run_phase::match && n == run_phase::execute)
          ctx_.sched->push_phase ();
        else if (o == run_phase::execute && n == run_phase::match)
          ctx_.sched->pop_phase ();

        // Notify others that could be waiting for this phase.
        //
        if (v != nullptr)
        {
          l.unlock ();
          v->notify_all ();
        }
      }
      else // phase != n
      {
        ++contention; // Protected by m_.

        ctx_.sched->deactivate (false /* external */);
        for (; ctx_.phase != n; v->wait (l)) ;
        r = !fail_;
        l.unlock (); // Important: activate() can block.
        ctx_.sched->activate (false /* external */);
      }
    }

    if (n == run_phase::load)
    {
      if (!lm_.try_lock ())
      {
        // If we failed to acquire the load mutex, then we know there is (or
        // was) someone before us in the load phase. And it's impossible to
        // switch to a different phase between our calls to try_lock() above
        // and lock() below because of our +1 in lc_.
        //
        s = false;

        ctx_.sched->deactivate (false /* external */);
        lm_.lock ();
        ctx_.sched->activate (false /* external */);

        ++contention_load; // Protected by lm_.
      }
      r = !fail_; // Re-query.
    }

    return r ? optional<bool> (s) : nullopt;
  }

  // C++17 deprecated uncaught_exception() so use uncaught_exceptions() if
  // available.
  //
  static inline bool
  uncaught_exception ()
  {
#ifdef __cpp_lib_uncaught_exceptions
    return std::uncaught_exceptions () != 0;
#else
    return std::uncaught_exception ();
#endif
  }

  // phase_lock
  //
  static
#ifdef __cpp_thread_local
  thread_local
#else
  __thread
#endif
  phase_lock* phase_lock_instance;

  phase_lock::
  phase_lock (context& c, run_phase p)
      : ctx (c), phase (p)
  {
    phase_lock* pl (phase_lock_instance);

    // This is tricky: we might be switching to another context.
    //
    if (pl != nullptr && &pl->ctx == &ctx)
      assert (pl->phase == phase);
    else
    {
      if (!ctx.phase_mutex.lock (phase))
      {
        ctx.phase_mutex.unlock (phase);
        throw failed ();
      }

      prev = pl;
      phase_lock_instance = this;

      //text << this_thread::get_id () << " phase acquire " << phase;
    }
  }

  phase_lock::
  ~phase_lock ()
  {
    if (phase_lock_instance == this)
    {
      phase_lock_instance = prev;
      ctx.phase_mutex.unlock (phase);

      //text << this_thread::get_id () << " phase release " << phase;
    }
  }

  // phase_unlock
  //
  phase_unlock::
  phase_unlock (context* c, bool d)
      : ctx (c), lock_ (nullptr)
  {
    if (ctx != nullptr && !d)
      unlock ();
  }

  void phase_unlock::
  unlock ()
  {
    if (ctx != nullptr && lock_ == nullptr)
    {
      lock_ = phase_lock_instance;
      assert (&lock_->ctx == ctx);

      phase_lock_instance = nullptr; // Note: not lock->prev.
      ctx->phase_mutex.unlock (lock_->phase);

      //text << this_thread::get_id () << " phase unlock  " << lock_->phase;
    }
  }

  void phase_unlock::
  lock ()
  {
    if (lock_ != nullptr)
    {
      bool r (ctx->phase_mutex.lock (lock_->phase));
      phase_lock_instance = lock_;

      // Fail unless we are already failing. Note that we keep the phase
      // locked since there will be phase_lock down the stack to unlock it.
      //
      if (!r && !uncaught_exception ())
        throw failed ();

      //text << this_thread::get_id () << " phase lock    " << lock_->phase;
    }
  }

  phase_unlock::
  ~phase_unlock () noexcept (false)
  {
    lock ();
  }

  // phase_switch
  //
  phase_switch::
  phase_switch (context& ctx, run_phase n)
      : old_phase (ctx.phase), new_phase (n)
  {
    phase_lock* pl (phase_lock_instance);
    assert (&pl->ctx == &ctx);

    optional<bool> r (ctx.phase_mutex.relock (old_phase, new_phase));
    if (!r)
    {
      ctx.phase_mutex.relock (new_phase, old_phase);
      throw failed ();
    }

    pl->phase = new_phase;

    if (new_phase == run_phase::load) // Note: load lock is exclusive.
    {
      ctx.load_generation++;

      // Invalidate cached target base_scope values if we are switching from a
      // non-load phase (we don't cache during load which means load->load
      // switch doesn't have anything to invalidate).
      //
      // @@ This is still quite expensive on project like Boost with a large
      //    number of files (targets) and a large number of load phase
      //    switches (due to directory buildfiles).
      //
      // Thinking some more on this, we shouldn't need to do this since such
      // loads can (or at least should) only perform "island appends" see
      // comment on context::phase for details.
      //
#if 0
      if (*r)
      {
        for (const unique_ptr<target>& t: ctx.targets)
          t->base_scope_.store (nullptr, memory_order_relaxed);
      }
#endif
    }

    //text << this_thread::get_id () << " phase switch  "
    //     << old_phase << " " << new_phase;
  }

#if 0
  // NOTE: see push/pop_phase() logic if trying to enable this. Also
  //       the load stuff above.
  //
  phase_switch::
  phase_switch (phase_unlock&& u, phase_lock&& l)
      : old_phase (u.l->phase), new_phase (l.phase)
  {
    phase_lock_instance = u.l; // Disarms phase_lock
    u.l = nullptr;             // Disarms phase_unlock
  }
#endif

  phase_switch::
  ~phase_switch () noexcept (false)
  {
    phase_lock* pl (phase_lock_instance);
    run_phase_mutex& pm (pl->ctx.phase_mutex);

    // If we are coming off a failed load phase, mark the phase_mutex as
    // failed to terminate all other threads since the build state may no
    // longer be valid.
    //
    if (new_phase == run_phase::load && uncaught_exception ())
    {
      mlock l (pm.m_);
      pm.fail_ = true;
    }

    bool r (pm.relock (new_phase, old_phase));
    pl->phase = old_phase;

    // Similar logic to ~phase_unlock().
    //
    if (!r && !uncaught_exception ())
      throw failed ();

    //text << this_thread::get_id () << " phase restore "
    //     << new_phase << " " << old_phase;
  }
}
