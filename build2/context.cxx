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
  dir_path work;
  dir_path home;
  options ops;

  string_pool extension_pool;
  string_pool project_name_pool;

  const meta_operation_info* current_mif;
  const operation_info* current_inner_oif;
  const operation_info* current_outer_oif;
  execution_mode current_mode;
  uint64_t dependency_count;

  variable_overrides
  reset (const strings& cmd_vars)
  {
    tracer trace ("reset");

    l6 ([&]{trace << "resetting build state";});

    variable_overrides vos;

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

    // Create global scope. For Win32 this is not a "real" root path.
    // On POSIX, however, this is a real path. See the comment in
    // <build2/path-map> for details.
    //
    scope& gs (*scopes.insert (dir_path ("/"), nullptr, true, false)->second);
    global_scope = &gs;

    // Parse and enter the command line variables. We do it before entering
    // any other variables so that all the variables that are overriden are
    // marked as such first. Then, as we enter variables, we can verify that
    // the override is alowed.
    //
    for (const string& s: cmd_vars)
    {
      char c (s[0]); // Should at least have '='.
      string a (s, c == '!' || c == '%' ? 1 : 0);

      istringstream is (a);
      is.exceptions (istringstream::failbit | istringstream::badbit);
      lexer l (is, path ("<cmdline>"));

      // This should be a name followed by =, +=, or =+.
      //
      token t (l.next ());
      token_type tt (l.next ().type);

      if (t.type != token_type::name ||
          (tt != token_type::assign &&
           tt != token_type::prepend &&
           tt != token_type::append))
      {
        fail << "expected variable assignment instead of '" << s << "'" <<
          info << "use double '--' to treat this argument as buildspec";
      }

      const variable& var (var_pool.find (t.value));
      const string& n (var.name);

      // The first variable in the override list is always the cache. Note
      // that we might already be overridden by an earlier cmd line var.
      //
      if (var.override == nullptr)
        var.override.reset (new variable {
            n + ".__cache", nullptr, nullptr, variable_visibility::normal});

      // Calculate visibility and kind.
      //
      variable_visibility v (c == '%'
                             ? variable_visibility::project
                             : variable_visibility::normal);
      const char* k (tt == token_type::assign ? ".__override" :
                     tt == token_type::append ? ".__suffix" : ".__prefix");

      // We might already have a variable for this kind of override.
      //
      const variable* o (var.override.get ());
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
      // things simple.
      //
      parser p;
      names val;
      t = p.parse_variable_value (l, gs, val);

      if (t.type != token_type::eos)
        fail << "unexpected " << t << " in variable assignment '" << s << "'";

      if (c == '!')
      {
        auto p (gs.vars.assign (*o));

        if (!p.second)
          fail << "multiple global overrides of variable " << var.name;

        value& v (p.first);
        v.assign (move (val), var); // Original var for diagnostics.

        // Also make sure the original variable itself is set (to at least
        // NULL) so that lookup finds something if nobody actually sets it
        // down the line.
        //
        gs.vars.assign (var);
      }
      else
        vos.emplace_back (variable_override {var, *o, move (val)});
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

      v.insert<string> ("extension");
    }

    gs.assign<dir_path> ("build.work") = work;
    gs.assign<dir_path> ("build.home") = home;

    // @@ Backwards-compatibility hack.
    //
    gs.assign<bool> ("__build2_greater_than_0_2_0_hack__") = true;

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
#ifndef BUILD2_HOST_TRIPLET
#error BUILD2_HOST_TRIPLET is not defined
#endif
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

  fs_status<mkdir_status>
  mkdir (const dir_path& d)
  {
    // We don't want to print the command if the directory already
    // exists. This makes the below code a bit ugly.
    //
    mkdir_status ms;

    try
    {
      ms = try_mkdir (d);
    }
    catch (const system_error& e)
    {
      if (verb)
        text << "mkdir " << d;

      fail << "unable to create directory " << d << ": " << e.what ();
    }

    if (ms == mkdir_status::success)
    {
      if (verb)
        text << "mkdir " << d;
    }

    return ms;
  }

  fs_status<mkdir_status>
  mkdir_p (const dir_path& d)
  {
    // We don't want to print the command if the directory already
    // exists. This makes the below code a bit ugly.
    //
    mkdir_status ms;

    try
    {
      ms = try_mkdir_p (d);
    }
    catch (const system_error& e)
    {
      if (verb)
        text << "mkdir -p " << d;

      fail << "unable to create directory " << d << ": " << e.what ();
    }

    if (ms == mkdir_status::success)
    {
      if (verb)
        text << "mkdir -p " << d;
    }

    return ms;
  }

  fs_status<butl::rmdir_status>
  rmdir_r (const dir_path& d)
  {
    using namespace butl;

    if (work.sub (d)) // Don't try to remove working directory.
      return rmdir_status::not_empty;

    if (!dir_exists (d))
      return rmdir_status::not_exist;

    if (verb)
      text << "rmdir -r " << d;

    try
    {
      butl::rmdir_r (d);
    }
    catch (const system_error& e)
    {
      fail << "unable to remove directory " << d << ": " << e.what ();
    }

    return rmdir_status::success;
  }

  dir_path
  src_out (const dir_path& out, scope& s)
  {
    scope& rs (*s.root_scope ());
    return src_out (out, rs.out_path (), rs.src_path ());
  }

  dir_path
  out_src (const dir_path& src, scope& s)
  {
    scope& rs (*s.root_scope ());
    return out_src (src, rs.out_path (), rs.src_path ());
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

  // relative()
  //
  const dir_path* relative_base = &work;

  string
  diag_relative (const path& p)
  {
    const path& b (*relative_base);

    if (p.absolute ())
    {
      if (p == b)
        return ".";

#ifndef _WIN32
      if (p == home)
        return "~";
#endif

      path rb (relative (p));

#ifndef _WIN32
      if (rb.relative ())
      {
        // See if the original path with the ~/ shortcut is better
        // that the relative to base.
        //
        if (p.sub (home))
        {
          path rh (p.leaf (home));
          if (rb.string ().size () > rh.string ().size () + 2) // 2 for '~/'
            return "~/" + rh.string ();
        }
      }
      else if (rb.sub (home))
        return "~/" + rb.leaf (home).string ();
#endif

      return rb.string ();
    }

    return p.string ();
  }

  string
  diag_relative (const dir_path& d, bool cur)
  {
    string r (diag_relative (static_cast<const path&> (d)));

    // Translate "." to empty.
    //
    if (!cur && d.absolute () && r == ".")
      r.clear ();

    // Add trailing '/'.
    //
    if (!r.empty () && !dir_path::traits::is_separator (r.back ()))
      r += '/';

    return r;
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
