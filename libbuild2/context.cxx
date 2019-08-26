// file      : libbuild2/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
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

using namespace std;
using namespace butl;

namespace build2
{
  // Create global scope. Note that the empty path is a prefix for any other
  // path. See the comment in <libbutl/prefix-map.mxx> for details.
  //
  static inline scope&
  create_global_scope (scope_map& m)
  {
    auto i (m.insert (dir_path ()));
    scope& r (i->second);
    r.out_path_ = &i->first;
    return r;
  };

  struct context::data
  {
    scope_map scopes;
    target_set targets;
    variable_pool var_pool;
    variable_overrides var_overrides;
    function_map functions;

    target_type_map global_target_types;
    variable_override_cache global_override_cache;

    data (context& c): scopes (c), targets (c), var_pool (&c /* global */) {}
  };

  context::
  context (scheduler& s, const strings& cmd_vars, bool dr, bool kg)
      : data_ (new data (*this)),
        sched (s),
        dry_run_option (dr),
        keep_going (kg),
        phase_mutex (*this),
        scopes (data_->scopes),
        targets (data_->targets),
        var_pool (data_->var_pool),
        var_overrides (data_->var_overrides),
        functions (data_->functions),
        global_scope (create_global_scope (data_->scopes)),
        global_target_types (data_->global_target_types),
        global_override_cache (data_->global_override_cache)
  {
    tracer trace ("context");

    l6 ([&]{trace << "initializing build state";});

    scope_map& sm (data_->scopes);
    variable_pool& vp (data_->var_pool);

    register_builtin_functions (functions);

    // Initialize the meta/operation tables. Note that the order should match
    // the id constants in <libbuild2/operation.hxx>.
    //
    meta_operation_table.insert ("noop");
    meta_operation_table.insert ("perform");
    meta_operation_table.insert ("configure");
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

    gs.assign<dir_path> ("build.work") = work;
    gs.assign<dir_path> ("build.home") = home;

    // Build system driver process path.
    //
    gs.assign<process_path> ("build.path") =
      process_path (nullptr, // Will be filled by value assignment.
                    path (argv0.recall_string ()),
                    path (argv0.effect));

    // Build system verbosity level.
    //
    gs.assign<uint64_t> ("build.verbosity") = verb;

    // Build system version (similar to what we do in the version module
    // except here we don't include package epoch/revision).
    //
    {
      const standard_version& v (build_version);

      auto set = [&gs, &vp] (const char* var, auto val)
      {
        using T = decltype (val);
        gs.assign (vp.insert<T> (var)) = move (val);
      };

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
    }

    // Enter the host information. Rather than jumping through hoops like
    // config.guess, for now we are just going to use the compiler target we
    // were built with. While it is not as precise (for example, a binary
    // built for i686 might be running on x86_64), it is good enough of an
    // approximation/fallback since most of the time we are interested in just
    // the target class (e.g., linux, windows, macos).
    //
    {
      // Did the user ask us to use config.guess?
      //
      string orig (config_guess
                   ? run<string> (3,
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
        gs.assign<string> ("build.host.cpu")     = t.cpu;
        gs.assign<string> ("build.host.vendor")  = t.vendor;
        gs.assign<string> ("build.host.system")  = t.system;
        gs.assign<string> ("build.host.version") = t.version;
        gs.assign<string> ("build.host.class")   = t.class_;

        gs.assign<target_triplet> ("build.host") = move (t);
      }
      catch (const invalid_argument& e)
      {
        fail << "unable to parse build host '" << orig << "': " << e <<
          info << "consider using the --config-guess option";
      }
    }

    // Register builtin target types.
    //
    {
      target_type_map& t (data_->global_target_types);

      t.insert<file>  ();
      t.insert<alias> ();
      t.insert<dir>   ();
      t.insert<fsdir> ();
      t.insert<exe>   ();
      t.insert<doc>   ();
      t.insert<man>   ();
      t.insert<man1>  ();

      {
        auto& tt (t.insert<manifest> ());
        t.insert_file ("manifest", tt);
      }

      {
        auto& tt (t.insert<buildfile> ());
        t.insert_file ("buildfile", tt);
      }
    }

    // Parse and enter the command line variables. We do it before entering
    // any other variables so that all the variables that are overriden are
    // marked as such first. Then, as we enter variables, we can verify that
    // the override is alowed.
    //
    for (size_t i (0); i != cmd_vars.size (); ++i)
    {
      const string& s (cmd_vars[i]);

      istringstream is (s);
      is.exceptions (istringstream::failbit | istringstream::badbit);

      // Similar to buildspec we do "effective escaping" and only for ['"\$(]
      // (basically what's necessary inside a double-quoted literal plus the
      // single quote).
      //
      lexer l (is, path ("<cmdline>"), 1 /* line */, "\'\"\\$(");

      // At the buildfile level the scope-specific variable should be
      // separated from the directory with a whitespace, for example:
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
      // And so the first token should be a word which can be either a
      // variable name (potentially with the directory qualification) or just
      // the directory, in which case it should be followed by another word
      // (unqualified variable name).
      //
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
              fail << "'" << s << "' is a target-specific override" <<
                info << "use double '--' to treat this argument as buildspec";
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
        fail << "expected variable assignment instead of '" << s << "'" <<
          info << "use double '--' to treat this argument as buildspec";
      }

      // Take care of the visibility. Note that here we rely on the fact that
      // none of these characters are lexer's name separators.
      //
      char c (t.value[0]);

      if (path::traits_type::is_separator (c))
        c = '/'; // Normalize.

      string n (t.value, c == '!' || c == '%' || c == '/' ? 1 : 0);

      if (c == '!' && dir)
        fail << "scope-qualified global override of variable " << n;

      variable& var (const_cast<variable&> (
                       vp.insert (n, true /* overridable */)));

      const variable* o;
      {
        variable_visibility v (c == '/' ? variable_visibility::scope   :
                               c == '%' ? variable_visibility::project :
                               variable_visibility::normal);

        const char* k (tt == token_type::assign ? "__override" :
                       tt == token_type::append ? "__suffix" : "__prefix");

        unique_ptr<variable> p (
          new variable {
            n + '.' + to_string (i + 1) + '.' + k,
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
      parser p (*this);
      pair<value, token> r (p.parse_variable_value (l, gs, &work, var));

      if (r.second.type != token_type::eos)
        fail << "unexpected " << r.second << " in variable assignment "
             << "'" << s << "'";

      // Make sure the value is not typed.
      //
      if (r.first.type != nullptr)
        fail << "typed override of variable " << n;

      // Global and absolute scope overrides we can enter directly. Project
      // and relative scope ones will be entered later for each project.
      //
      if (c == '!' || (dir && dir->absolute ()))
      {
        scope& s (c == '!' ? gs : sm.insert (*dir)->second);

        auto p (s.vars.insert (*o));
        assert (p.second); // Variable name is unique.

        value& v (p.first);
        v = move (r.first);
      }
      else
        data_->var_overrides.push_back (
          variable_override {var, *o, move (dir), move (r.first)});
    }

    // Enter builtin variables and patterns.
    //

    // All config. variables are by default overridable.
    //
    vp.insert_pattern ("config.**", nullopt, true, nullopt, true, false);

    // file.cxx:import() (note that order is important; see insert_pattern()).
    //
    vp.insert_pattern<abs_dir_path> (
      "config.import.*", true, variable_visibility::normal, true);
    vp.insert_pattern<path> (
      "config.import.**", true, variable_visibility::normal, true);

    // module.cxx:boot/init_module().
    //
    {
      auto v_p (variable_visibility::project);

      vp.insert_pattern<bool> ("**.booted", false, v_p);
      vp.insert_pattern<bool> ("**.loaded", false, v_p);
      vp.insert_pattern<bool> ("**.configured", false, v_p);
    }

    {
      auto v_p (variable_visibility::project);
      auto v_t (variable_visibility::target);
      auto v_q (variable_visibility::prereq);

      var_src_root = &vp.insert<dir_path> ("src_root");
      var_out_root = &vp.insert<dir_path> ("out_root");
      var_src_base = &vp.insert<dir_path> ("src_base");
      var_out_base = &vp.insert<dir_path> ("out_base");

      var_forwarded = &vp.insert<bool> ("forwarded", v_p);

      // Note that subprojects is not typed since the value requires
      // pre-processing (see file.cxx).
      //
      var_project      = &vp.insert<project_name> ("project",      v_p);
      var_amalgamation = &vp.insert<dir_path>     ("amalgamation", v_p);
      var_subprojects  = &vp.insert               ("subprojects",  v_p);
      var_version      = &vp.insert<string>       ("version",      v_p);

      var_project_url     = &vp.insert<string> ("project.url",     v_p);
      var_project_summary = &vp.insert<string> ("project.summary", v_p);

      var_import_target = &vp.insert<name> ("import.target");

      var_extension = &vp.insert<string> ("extension", v_t);
      var_clean     = &vp.insert<bool>   ("clean",    v_t);
      var_backlink  = &vp.insert<string> ("backlink", v_t);
      var_include   = &vp.insert<string> ("include",  v_q);

      // Backlink executables and (generated) documentation by default.
      //
      gs.target_vars[exe::static_type]["*"].assign (var_backlink) = "true";
      gs.target_vars[doc::static_type]["*"].assign (var_backlink) = "true";

      var_build_meta_operation = &vp.insert<string> ("build.meta_operation");
    }

    // Register builtin rules.
    //
    {
      rule_map& r (gs.rules); // Note: global scope!

      //@@ outer
      r.insert<alias> (perform_id, 0, "alias", alias_rule::instance);

      r.insert<fsdir> (perform_update_id, "fsdir", fsdir_rule::instance);
      r.insert<fsdir> (perform_clean_id, "fsdir", fsdir_rule::instance);

      r.insert<mtime_target> (perform_update_id, "file", file_rule::instance);
      r.insert<mtime_target> (perform_clean_id, "file", file_rule::instance);
    }
  }

  context::
  ~context ()
  {
    // Cannot be inline since context::data is undefined.
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
    current_on = 0; // Reset.
  }

  void context::
  current_operation (const operation_info& inner_oif,
                     const operation_info* outer_oif,
                     bool diag_noise)
  {
    current_oname = (outer_oif == nullptr ? inner_oif : *outer_oif).name;
    current_inner_oif = &inner_oif;
    current_outer_oif = outer_oif;
    current_on++;
    current_mode = inner_oif.mode;
    current_diag_noise = diag_noise;

    // Reset counters (serial execution).
    //
    dependency_count.store (0, memory_order_relaxed);
    target_count.store (0, memory_order_relaxed);
    skip_count.store (0, memory_order_relaxed);
  }

  bool run_phase_mutex::
  lock (run_phase p)
  {
    bool r;

    {
      mlock l (m_);
      bool u (lc_ == 0 && mc_ == 0 && ec_ == 0); // Unlocked.

      // Increment the counter.
      //
      condition_variable* v (nullptr);
      switch (p)
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
        ctx_.phase = p;
        r = !fail_;
      }
      else if (ctx_.phase != p)
      {
        ctx_.sched.deactivate (false /* external */);
        for (; ctx_.phase != p; v->wait (l)) ;
        r = !fail_;
        l.unlock (); // Important: activate() can block.
        ctx_.sched.activate (false /* external */);
      }
      else
        r = !fail_;
    }

    // In case of load, acquire the exclusive access mutex.
    //
    if (p == run_phase::load)
    {
      lm_.lock ();
      r = !fail_; // Re-query.
    }

    return r;
  }

  void run_phase_mutex::
  unlock (run_phase p)
  {
    // In case of load, release the exclusive access mutex.
    //
    if (p == run_phase::load)
      lm_.unlock ();

    {
      mlock l (m_);

      // Decrement the counter and see if this phase has become unlocked.
      //
      bool u (false);
      switch (p)
      {
      case run_phase::load:    u = (--lc_ == 0); break;
      case run_phase::match:   u = (--mc_ == 0); break;
      case run_phase::execute: u = (--ec_ == 0); break;
      }

      // If the phase is unlocked, pick a new phase and notify the waiters.
      // Note that we notify all load waiters so that they can all serialize
      // behind the second-level mutex.
      //
      if (u)
      {
        condition_variable* v;

        if      (lc_ != 0) {ctx_.phase = run_phase::load;    v = &lv_;}
        else if (mc_ != 0) {ctx_.phase = run_phase::match;   v = &mv_;}
        else if (ec_ != 0) {ctx_.phase = run_phase::execute; v = &ev_;}
        else               {ctx_.phase = run_phase::load;    v = nullptr;}

        if (v != nullptr)
        {
          l.unlock ();
          v->notify_all ();
        }
      }
    }
  }

  bool run_phase_mutex::
  relock (run_phase o, run_phase n)
  {
    // Pretty much a fused unlock/lock implementation except that we always
    // switch into the new phase.
    //
    assert (o != n);

    bool r;

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
        ctx_.sched.deactivate (false /* external */);
        for (; ctx_.phase != n; v->wait (l)) ;
        r = !fail_;
        l.unlock (); // Important: activate() can block.
        ctx_.sched.activate (false /* external */);
      }
    }

    if (n == run_phase::load)
    {
      lm_.lock ();
      r = !fail_; // Re-query.
    }

    return r;
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

      //text << this_thread::get_id () << " phase release " << p;
    }
  }

  // phase_unlock
  //
  phase_unlock::
  phase_unlock (context& ctx, bool u)
      : l (u ? phase_lock_instance : nullptr)
  {
    if (u)
    {
      assert (&l->ctx == &ctx);

      phase_lock_instance = nullptr; // Note: not l->prev.
      ctx.phase_mutex.unlock (l->phase);

      //text << this_thread::get_id () << " phase unlock  " << l->p;
    }
  }

  phase_unlock::
  ~phase_unlock () noexcept (false)
  {
    if (l != nullptr)
    {
      bool r (l->ctx.phase_mutex.lock (l->phase));
      phase_lock_instance = l;

      // Fail unless we are already failing. Note that we keep the phase
      // locked since there will be phase_lock down the stack to unlock it.
      //
      if (!r && !uncaught_exception ())
        throw failed ();

      //text << this_thread::get_id () << " phase lock    " << l->p;
    }
  }

  // phase_switch
  //
  phase_switch::
  phase_switch (context& ctx, run_phase n)
      : old_phase (ctx.phase), new_phase (n)
  {
    phase_lock* pl (phase_lock_instance);
    assert (&pl->ctx == &ctx);

    if (!ctx.phase_mutex.relock (old_phase, new_phase))
    {
      ctx.phase_mutex.relock (new_phase, old_phase);
      throw failed ();
    }

    pl->phase = new_phase;

    if (new_phase == run_phase::load) // Note: load lock is exclusive.
      ctx.load_generation++;

    //text << this_thread::get_id () << " phase switch  " << o << " " << n;
  }

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

    //text << this_thread::get_id () << " phase restore " << n << " " << o;
  }

  void (*config_save_variable) (scope&, const variable&, uint64_t);

  const string& (*config_preprocess_create) (context&,
                                             values&,
                                             vector_view<opspec>&,
                                             bool,
                                             const location&);
}
