// file      : build2/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/context>

#include <butl/triplet>

#include <build2/rule>
#include <build2/scope>
#include <build2/target>
#include <build2/version>
#include <build2/diagnostics>

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

  void
  reset ()
  {
    tracer trace ("reset");

    extension_pool.clear ();
    project_name_pool.clear ();

    targets.clear ();
    scopes.clear ();
    var_pool.clear ();

    // Reset meta/operation tables. Note that the order should match
    // the id constants in <build2/operation>.
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

    // Enter builtin variables.
    //
    {
      auto& v (var_pool);

      v.find ("src_root", dir_path_type);
      v.find ("out_root", dir_path_type);
      v.find ("src_base", dir_path_type);
      v.find ("out_base", dir_path_type);

      v.find ("project", string_type);
      v.find ("amalgamation", dir_path_type);

      // Shouldn't be typed since the value requires pre-processing.
      //
      v.find ("subprojects", nullptr, '=');

      v.find ("extension", string_type);
    }

    // Create global scope. For Win32 this is not a "real" root path.
    // On POSIX, however, this is a real path. See the comment in
    // <build2/path-map> for details.
    //
    global_scope = scopes.insert (
      dir_path ("/"), nullptr, true, false)->second;

    scope& gs (*global_scope);

    gs.assign ("build.work", dir_path_type) = work;
    gs.assign ("build.home", dir_path_type) = home;

    // Enter the version.
    //
    // @@ VAR types
    //
    {
      gs.assign ("build.version", string_type) = to_string (BUILD2_VERSION);
      gs.assign ("build.version.string", string_type) = BUILD2_VERSION_STR;

      // AABBCCDD
      //
      auto comp = [] (unsigned int d) -> string
      {
        return to_string ((BUILD2_VERSION / d)% 100);
      };

      gs.assign ("build.version.release", string_type) = comp (1);
      gs.assign ("build.version.patch",   string_type) = comp (100);
      gs.assign ("build.version.minor",   string_type) = comp (10000);
      gs.assign ("build.version.major",   string_type) = comp (1000000);
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
        gs.assign ("build.host", string_type) = move (canon);
        gs.assign ("build.host.cpu", string_type) = move (t.cpu);
        gs.assign ("build.host.vendor", string_type) = move (t.vendor);
        gs.assign ("build.host.system", string_type) = move (t.system);
        gs.assign ("build.host.version", string_type) = move (t.version);
        gs.assign ("build.host.class", string_type) = move (t.class_);
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
