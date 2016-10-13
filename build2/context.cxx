// file      : build2/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/context>

#include <sstream>

#include <butl/triplet>

#include <build2/rule>
#include <build2/scope>
#include <build2/target>
#include <build2/version>
#include <build2/diagnostics>

// For command line variable parsing.
//
#include <build2/token>
#include <build2/lexer>
#include <build2/parser>

using namespace std;
using namespace butl;

namespace build2
{
  options ops;

  string_pool extension_pool;
  string_pool project_name_pool;

  const string* current_mname;
  const string* current_oname;

  const meta_operation_info* current_mif;
  const operation_info* current_inner_oif;
  const operation_info* current_outer_oif;

  execution_mode current_mode;

  uint64_t dependency_count;

  variable_override_cache var_override_cache;

  variable_overrides
  reset (const strings& cmd_vars)
  {
    tracer trace ("reset");

    // @@ Need to unload modules when we dynamically load them.
    //

    l6 ([&]{trace << "resetting build state";});

    variable_overrides vos;

    var_override_cache.clear ();

    targets.clear ();
    scopes.clear ();
    var_pool.clear ();

    extension_pool.clear ();
    project_name_pool.clear ();

    // Reset meta/operation tables. Note that the order should match the id
    // constants in <build2/operation>.
    //
    meta_operation_table.clear ();
    meta_operation_table.insert ("perform");
    meta_operation_table.insert ("configure");
    meta_operation_table.insert ("disfigure");
    meta_operation_table.insert ("dist");

    operation_table.clear ();
    operation_table.insert ("default");
    operation_table.insert ("update");
    operation_table.insert ("clean");
    operation_table.insert ("test");
    operation_table.insert ("install");
    operation_table.insert ("uninstall");

    // Create global scope. For Win32 this is not a "real" root path.
    // On POSIX, however, this is a real path. See the comment in
    // <build2/path-map> for details.
    //
    {
      auto i (scopes.insert (dir_path ("/"), false));
      global_scope = &i->second;
      global_scope->out_path_ = &i->first;
    }

    scope& gs (*global_scope);


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
      lexer l (is, path ("<cmdline>"), "\'\"\\$(");

      // The first token should be a name, either the variable name or the
      // scope qualification.
      //
      token t (l.next ());
      token_type tt (l.next ().type);

      dir_path dir;
      if (t.type == token_type::name && tt == token_type::colon)
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
      if (t.type != token_type::name || t.value.empty () ||
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

      const variable& var (var_pool[n]);
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
        o->override.reset (new variable {n + k, nullptr, nullptr, v});

      o = o->override.get ();

      // Currently we expand project overrides in the global scope to keep
      // things simple. Pass original variable for diagnostics.
      //
      parser p;
      pair<value, token> r (p.parse_variable_value (l, gs, var));

      if (r.second.type != token_type::eos)
        fail << "unexpected " << r.second << " in variable assignment "
             << "'" << s << "'";

      // Make sure the value is not typed.
      //
      if (r.first.type != nullptr)
        fail << "typed override of variable " << n;

      // Global and scope overrides we can enter directly. Project ones will
      // be entered by the caller for for each amalgamation/project.
      //
      if (c == '!' || !dir.empty ())
      {
        scope& s (c == '!' ? gs : scopes.insert (dir, false)->second);
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

    // Enter builtin variables.
    //
    {
      auto& v (var_pool);

      v.insert<dir_path> ("src_root");
      v.insert<dir_path> ("out_root");
      v.insert<dir_path> ("src_base");
      v.insert<dir_path> ("out_base");

      v.insert<string> ("project");
      v.insert<dir_path> ("amalgamation");

      // Not typed since the value requires pre-processing (see file.cxx).
      //
      v.insert ("subprojects");

      v.insert<string> ("extension", variable_visibility::target);
    }

    gs.assign<dir_path> ("build.work") = work;
    gs.assign<dir_path> ("build.home") = home;

    // Enter the version.
    //
    {
      gs.assign<uint64_t> ("build.version") = uint64_t (BUILD2_VERSION);
      gs.assign<string> ("build.version.string") = BUILD2_VERSION_STR;

      // AABBCCDD
      //
      auto comp = [] (unsigned int d) -> uint64_t
      {
        return (BUILD2_VERSION / d) % 100;
      };

      gs.assign<uint64_t> ("build.version.release") = comp (1);
      gs.assign<uint64_t> ("build.version.patch")   = comp (100);
      gs.assign<uint64_t> ("build.version.minor")   = comp (10000);
      gs.assign<uint64_t> ("build.version.major")   = comp (1000000);
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
        string canon;
        triplet t (orig, canon);

        l5 ([&]{trace << "canonical host: '" << canon << "'; "
                      << "class: " << t.class_;});

        // Enter as build.host.{cpu,vendor,system,version,class}.
        //
        gs.assign<string> ("build.host") = move (canon);
        gs.assign<string> ("build.host.cpu") = move (t.cpu);
        gs.assign<string> ("build.host.vendor") = move (t.vendor);
        gs.assign<string> ("build.host.system") = move (t.system);
        gs.assign<string> ("build.host.version") = move (t.version);
        gs.assign<string> ("build.host.class") = move (t.class_);
      }
      catch (const invalid_argument& e)
      {
        fail << "unable to parse build host '" << orig << "': " << e.what () <<
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
      t.insert<doc>   ();
      t.insert<man>   ();
      t.insert<man1>  ();
    }

    // Register builtin rules.
    //
    {
      rule_map& r (gs.rules);

      r.insert<alias> (perform_id, 0, "alias", alias_rule::instance);

      r.insert<fsdir> (perform_update_id, "fsdir", fsdir_rule::instance);
      r.insert<fsdir> (perform_clean_id, "fsdir", fsdir_rule::instance);

      r.insert<file> (perform_update_id, "file", file_rule::instance);
      r.insert<file> (perform_clean_id, "file", file_rule::instance);
    }

    return vos;
  }

  dir_path
  src_out (const dir_path& out, scope& r)
  {
    assert (r.root ());
    return src_out (out, r.out_path (), r.src_path ());
  }

  dir_path
  out_src (const dir_path& src, scope& r)
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
  void
  diag_do (ostream& os, const action&, const target& t)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    // perform(update(x))   -> "update x"
    // configure(update(x)) -> "configure updating x"
    //
    if (m.name_do.empty ())
      os << io.name_do << ' ';
    else
    {
      os << m.name_do << ' ';

      if (!io.name_doing.empty ())
        os << io.name_doing << ' ';
    }

    if (oo != nullptr)
      os << "(for " << oo->name << ") ";

    os << t;
  }

  void
  diag_doing (ostream& os, const action&, const target& t)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    // perform(update(x))   -> "updating x"
    // configure(update(x)) -> "configuring updating x"
    //
    if (!m.name_doing.empty ())
      os << m.name_doing << ' ';

    if (!io.name_doing.empty ())
      os << io.name_doing << ' ';

    if (oo != nullptr)
      os << "(for " << oo->name << ") ";

    os << t;
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
        os << " " << io.name_done;

      if (oo != nullptr)
        os << "(for " << oo->name << ") ";
    }
    else
    {
      if (!io.name_doing.empty ())
        os << io.name_doing << ' ';

      if (oo != nullptr)
        os << "(for " << oo->name << ") ";

      os << t << " " << m.name_done;
    }
  }
}
