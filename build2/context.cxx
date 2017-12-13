// file      : build2/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/context.hxx>

#include <sstream>
#include <exception> // uncaught_exception[s]()

#include <build2/rule.hxx>
#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/diagnostics.hxx>

#include <libbutl/ft/exception.hxx> // uncaught_exceptions

// For command line variable parsing.
//
#include <build2/token.hxx>
#include <build2/lexer.hxx>
#include <build2/parser.hxx>

#include <build2/config/operation.hxx> // config::preprocess_create().

using namespace std;
using namespace butl;

namespace build2
{
  scheduler sched;

  run_phase phase;
  phase_mutex phase_mutex::instance;

  size_t load_generation;

#ifdef __cpp_thread_local
  thread_local
#else
  __thread
#endif
  phase_lock* phase_lock::instance;

  bool phase_mutex::
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
        phase = p;
        r = !fail_;
      }
      else if (phase != p)
      {
        sched.deactivate ();
        for (; phase != p; v->wait (l)) ;
        r = !fail_;
        l.unlock (); // Important: activate() can block.
        sched.activate ();
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

  void phase_mutex::
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

        if      (lc_ != 0) {phase = run_phase::load;    v = &lv_;}
        else if (mc_ != 0) {phase = run_phase::match;   v = &mv_;}
        else if (ec_ != 0) {phase = run_phase::execute; v = &ev_;}
        else               {phase = run_phase::load;    v = nullptr;}

        if (v != nullptr)
        {
          l.unlock ();
          v->notify_all ();
        }
      }
    }
  }

  bool phase_mutex::
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
        phase = n;
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
        sched.deactivate ();
        for (; phase != n; v->wait (l)) ;
        r = !fail_;
        l.unlock (); // Important: activate() can block.
        sched.activate ();
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
  phase_lock::
  phase_lock (run_phase p)
      : p (p)
  {
    if (phase_lock* l = instance)
      assert (l->p == p);
    else
    {
      if (!phase_mutex::instance.lock (p))
      {
        phase_mutex::instance.unlock (p);
        throw failed ();
      }

      instance = this;

      //text << this_thread::get_id () << " phase acquire " << p;
    }
  }

  phase_lock::
  ~phase_lock ()
  {
    if (instance == this)
    {
      instance = nullptr;
      phase_mutex::instance.unlock (p);

      //text << this_thread::get_id () << " phase release " << p;
    }
  }

  // phase_unlock
  //
  phase_unlock::
  phase_unlock (bool u)
      : l (u ? phase_lock::instance : nullptr)
  {
    if (u)
    {
      phase_lock::instance = nullptr;
      phase_mutex::instance.unlock (l->p);

      //text << this_thread::get_id () << " phase unlock  " << l->p;
    }
  }

  phase_unlock::
  ~phase_unlock () noexcept (false)
  {
    if (l != nullptr)
    {
      bool r (phase_mutex::instance.lock (l->p));
      phase_lock::instance = l;

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
  phase_switch (run_phase n)
      : o (phase), n (n)
  {
    if (!phase_mutex::instance.relock (o, n))
    {
      phase_mutex::instance.relock (n, o);
      throw failed ();
    }

    phase_lock::instance->p = n;

    if (n == run_phase::load) // Note: load lock is exclusive.
      load_generation++;

    //text << this_thread::get_id () << " phase switch  " << o << " " << n;
  }

  phase_switch::
  ~phase_switch () noexcept (false)
  {
    // If we are coming off a failed load phase, mark the phase_mutex as
    // failed to terminate all other threads since the build state may no
    // longer be valid.
    //
    if (n == run_phase::load && uncaught_exception ())
      phase_mutex::instance.fail_ = true;

    bool r (phase_mutex::instance.relock (n, o));
    phase_lock::instance->p = o;

    // Similar logic to ~phase_unlock().
    //
    if (!r && !uncaught_exception ())
      throw failed ();

    //text << this_thread::get_id () << " phase restore " << n << " " << o;
  }

  const variable* var_src_root;
  const variable* var_out_root;
  const variable* var_src_base;
  const variable* var_out_base;

  const variable* var_project;
  const variable* var_amalgamation;
  const variable* var_subprojects;
  const variable* var_version;

  const variable* var_project_url;
  const variable* var_project_summary;

  const variable* var_import_target;

  const variable* var_clean;

  const char var_extension[10] = "extension";

  const string* current_mname;
  const string* current_oname;

  const meta_operation_info* current_mif;
  const operation_info* current_inner_oif;
  const operation_info* current_outer_oif;
  size_t current_on;
  execution_mode current_mode;

  atomic_count dependency_count;
  atomic_count target_count;
  atomic_count skip_count;

  bool keep_going = false;

  variable_overrides
  reset (const strings& cmd_vars)
  {
    tracer trace ("reset");

    // @@ Need to unload modules when we dynamically load them.
    //

    l6 ([&]{trace << "resetting build state";});

    auto& vp (variable_pool::instance);
    auto& sm (scope_map::instance);

    variable_overrides vos;

    targets.clear ();
    sm.clear ();
    vp.clear ();

    // Reset meta/operation tables. Note that the order should match the id
    // constants in <build2/operation.hxx>.
    //
    meta_operation_table.clear ();
    meta_operation_table.insert ("noop");
    meta_operation_table.insert ("perform");
    meta_operation_table.insert ("configure");
    meta_operation_table.insert ("disfigure");
    meta_operation_table.insert (
      meta_operation_data ("create", &config::preprocess_create));
    meta_operation_table.insert ("dist");
    meta_operation_table.insert ("info");

    operation_table.clear ();
    operation_table.insert ("default");
    operation_table.insert ("update");
    operation_table.insert ("clean");
    operation_table.insert ("test");
    operation_table.insert ("install");
    operation_table.insert ("uninstall");

    // Create global scope. Note that the empty path is a prefix for any other
    // path. See the comment in <libbutl/prefix-map.mxx> for details.
    //
    auto make_global_scope = [&sm] () -> scope&
    {
      auto i (sm.insert (dir_path (), false));
      scope& r (i->second);
      r.out_path_ = &i->first;
      global_scope = scope::global_ = &r;
      return r;
    };

    scope& gs (make_global_scope ());

    // Setup the global scope before parsing any variable overrides since they
    // may reference these things.
    //

    // Target extension.
    //
    vp.insert<string> (var_extension, variable_visibility::target);

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

      auto set = [&vp, &gs] (const char* var, auto val)
      {
        using T = decltype (val);
        gs.assign (vp.insert<T> (var)) = move (val);
      };

      set ("build.version", v.string_project ());

      set ("build.version.number", v.version);
      set ("build.version.id",     v.string_project_id ());

      set ("build.version.major", uint64_t (v.major ()));
      set ("build.version.minor", uint64_t (v.minor ()));
      set ("build.version.patch", uint64_t (v.patch ()));

      set ("build.version.alpha",              v.alpha ()); // bool
      set ("build.version.beta",               v.beta ());  // bool
      set ("build.version.pre_release",        v.alpha () || v.beta ());
      set ("build.version.pre_release_string", v.string_pre_release ());
      set ("build.version.pre_release_number", uint64_t (v.pre_release ()));

      set ("build.version.snapshot",        v.snapshot ()); // bool
      set ("build.version.snapshot_sn",     v.snapshot_sn); // uint64
      set ("build.version.snapshot_id",     v.snapshot_id); // string
      set ("build.version.snapshot_string", v.string_snapshot ());
    }

    // Enter the host information. Rather than jumping through hoops like
    // config.guess, for now we are just going to use the compiler target we
    // were built with. While it is not as precise (for example, a binary
    // built for i686 might be running on x86_64), it is good enough of an
    // approximation/fallback since most of the time we are interested in just
    // the target class (e.g., linux, windows, macosx).
    //
    {
      // Did the user ask us to use config.guess?
      //
      string orig (
        ops.config_guess_specified ()
        ? run<string> (ops.config_guess (), [] (string& l) {return move (l);})
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
      target_type_map& t (gs.target_types);

      t.insert<file>  ();
      t.insert<alias> ();
      t.insert<dir>   ();
      t.insert<fsdir> ();
      t.insert<in>    ();
      t.insert<exe>   ();
      t.insert<doc>   ();
      t.insert<man>   ();
      t.insert<man1>  ();
    }

    // Parse and enter the command line variables. We do it before entering
    // any other variables so that all the variables that are overriden are
    // marked as such first. Then, as we enter variables, we can verify that
    // the override is alowed.
    //
    for (const string& s: cmd_vars)
    {
      istringstream is (s);
      is.exceptions (istringstream::failbit | istringstream::badbit);

      // Similar to buildspec we do "effective escaping" and only for ['"\$(]
      // (basically what's necessary inside a double-quoted literal plus the
      // single quote).
      //
      lexer l (is, path ("<cmdline>"), 1 /* line */, "\'\"\\$(");

      // The first token should be a word, either the variable name or the
      // scope qualification.
      //
      token t (l.next ());
      token_type tt (l.next ().type);

      dir_path dir;
      if (t.type == token_type::word && tt == token_type::colon)
      {
        if (!path::traits::is_separator (t.value.back ()))
          fail << "expected directory (with trailing slash) instead of "
               << "'" << t.value << "'";

        dir = dir_path (move (t.value));

        if (dir.relative ())
          dir.complete ();

        dir.normalize ();

        t = l.next ();
        tt = l.next ().type;
      }

      // This should be the variable name followed by =, +=, or =+.
      //
      if (t.type != token_type::word || t.value.empty () ||
          (tt != token_type::assign &&
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
      string n (t.value, c == '!' || c == '%' || c == '/' ? 1 : 0);

      if (c == '!' && !dir.empty ())
        fail << "scope-qualified global override of variable " << n;

      variable_visibility v (c == '/' ? variable_visibility::scope   :
                             c == '%' ? variable_visibility::project :
                             variable_visibility::normal);

      const variable& var (vp.insert (n, true)); // Allow overrides.
      const char* k (tt == token_type::assign ? ".__override" :
                     tt == token_type::append ? ".__suffix" : ".__prefix");

      // We might already have a variable for this kind of override.
      //
      const variable* o (&var); // Step behind.
      for (; o->override != nullptr; o = o->override.get ())
      {
        if (o->override->visibility == v &&
            o->override->name.rfind (k) != string::npos)
          break;
      }

      // Add it if not found.
      //
      if (o->override == nullptr)
        const_cast<variable*> (o)->override.reset (
          new variable {n + k, nullptr, nullptr, v});

      o = o->override.get ();

      // Currently we expand project overrides in the global scope to keep
      // things simple. Pass original variable for diagnostics. Use current
      // working directory as pattern base.
      //
      parser p;
      pair<value, token> r (p.parse_variable_value (l, gs, &work, var));

      if (r.second.type != token_type::eos)
        fail << "unexpected " << r.second << " in variable assignment "
             << "'" << s << "'";

      // Make sure the value is not typed.
      //
      if (r.first.type != nullptr)
        fail << "typed override of variable " << n;

      // Global and scope overrides we can enter directly. Project ones will
      // be entered by the caller for each amalgamation/project.
      //
      if (c == '!' || !dir.empty ())
      {
        scope& s (c == '!' ? gs : sm.insert (dir, false)->second);
        auto p (s.vars.insert (*o));

        if (!p.second)
        {
          if (c == '!')
            fail << "multiple global overrides of variable " << n;
          else
            fail << "multiple overrides of variable " << n
                 << " in scope " << dir;
        }

        value& v (p.first);
        v = move (r.first);
      }
      else
        vos.emplace_back (variable_override {var, *o, move (r.first)});
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

    // module.cxx:load_module().
    //
    vp.insert_pattern<bool> (
      "**.loaded", false, variable_visibility::project);
    vp.insert_pattern<bool> (
      "**.configured", false, variable_visibility::project);

    var_src_root = &vp.insert<dir_path> ("src_root");
    var_out_root = &vp.insert<dir_path> ("out_root");
    var_src_base = &vp.insert<dir_path> ("src_base");
    var_out_base = &vp.insert<dir_path> ("out_base");

    // Note that subprojects is not typed since the value requires
    // pre-processing (see file.cxx).
    //
    {
      auto pv (variable_visibility::project);

      var_project      = &vp.insert<string>   ("project",      pv);
      var_amalgamation = &vp.insert<dir_path> ("amalgamation", pv);
      var_subprojects  = &vp.insert           ("subprojects",  pv);
      var_version      = &vp.insert<string>   ("version",      pv);

      var_project_url     = &vp.insert<string> ("project.url",     pv);
      var_project_summary = &vp.insert<string> ("project.summary", pv);

      var_import_target = &vp.insert<name> ("import.target");

      var_clean = &vp.insert<bool> ("clean", variable_visibility::target);
    }

    // Register builtin rules.
    //
    {
      rule_map& r (gs.rules);

      r.insert<alias> (perform_id, 0, "alias", alias_rule::instance);

      r.insert<fsdir> (perform_update_id, "fsdir", fsdir_rule::instance);
      r.insert<fsdir> (perform_clean_id, "fsdir", fsdir_rule::instance);

      r.insert<mtime_target> (perform_update_id, "file", file_rule::instance);
      r.insert<mtime_target> (perform_clean_id, "file", file_rule::instance);
    }

    return vos;
  }

  dir_path
  src_out (const dir_path& out, const scope& r)
  {
    assert (r.root ());
    return src_out (out, r.out_path (), r.src_path ());
  }

  dir_path
  out_src (const dir_path& src, const scope& r)
  {
    assert (r.root ());
    return out_src (src, r.out_path (), r.src_path ());
  }

  dir_path
  src_out (const dir_path& o,
           const dir_path& out_root, const dir_path& src_root)
  {
    assert (o.sub (out_root));
    return src_root / o.leaf (out_root);
  }

  dir_path
  out_src (const dir_path& s,
           const dir_path& out_root, const dir_path& src_root)
  {
    assert (s.sub (src_root));
    return out_root / s.leaf (src_root);
  }

  // diag_do(), etc.
  //
  string
  diag_do (const action&)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    string r;

    // perform(update(x))   -> "update x"
    // configure(update(x)) -> "configure updating x"
    //
    if (m.name_do.empty ())
      r = io.name_do;
    else
    {
      r = m.name_do;

      if (!io.name_doing.empty ())
      {
        r += ' ';
        r += io.name_doing;
      }
    }

    if (oo != nullptr)
    {
      r += " (for ";
      r += oo->name;
      r += ')';
    }

    return r;
  }

  void
  diag_do (ostream& os, const action& a, const target& t)
  {
    os << diag_do (a) << ' ' << t;
  }

  string
  diag_doing (const action&)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    string r;

    // perform(update(x))   -> "updating x"
    // configure(update(x)) -> "configuring updating x"
    //
    if (!m.name_doing.empty ())
      r = m.name_doing;

    if (!io.name_doing.empty ())
    {
      if (!r.empty ()) r += ' ';
      r += io.name_doing;
    }

    if (oo != nullptr)
    {
      r += " (for ";
      r += oo->name;
      r += ')';
    }

    return r;
  }

  void
  diag_doing (ostream& os, const action& a, const target& t)
  {
    os << diag_doing (a) << ' ' << t;
  }

  string
  diag_did (const action&)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    string r;

    // perform(update(x))   -> "updated x"
    // configure(update(x)) -> "configured updating x"
    //
    if (!m.name_did.empty ())
    {
      r = m.name_did;

      if (!io.name_doing.empty ())
      {
        r += ' ';
        r += io.name_doing;
      }
    }
    else
      r += io.name_did;

    if (oo != nullptr)
    {
      r += " (for ";
      r += oo->name;
      r += ')';
    }

    return r;
  }

  void
  diag_did (ostream& os, const action& a, const target& t)
  {
    os << diag_did (a) << ' ' << t;
  }

  void
  diag_done (ostream& os, const action&, const target& t)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    // perform(update(x))   -> "x is up to date"
    // configure(update(x)) -> "updating x is configured"
    //
    if (m.name_done.empty ())
    {
      os << t;

      if (!io.name_done.empty ())
        os << ' ' << io.name_done;

      if (oo != nullptr)
        os << " (for " << oo->name << ')';
    }
    else
    {
      if (!io.name_doing.empty ())
        os << io.name_doing << ' ';

      if (oo != nullptr)
        os << "(for " << oo->name << ") ";

      os << t << ' ' << m.name_done;
    }
  }
}
