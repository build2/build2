// file      : build/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/context>

#include <ostream>
#include <sstream>
#include <cassert>
#include <system_error>

#include <build/scope>
#include <build/target>
#include <build/rule>
#include <build/diagnostics>

using namespace std;
using namespace butl;

namespace build
{
  dir_path work;
  dir_path home;

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
    extension_pool.clear ();
    project_name_pool.clear ();

    targets.clear ();
    scopes.clear ();
    variable_pool.clear ();

    // Reset meta/operation tables. Note that the order should match
    // the id constants in <build/operation>.
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
    variable_pool.find ("work", dir_path_type);
    variable_pool.find ("home", dir_path_type);

    variable_pool.find ("src_root", dir_path_type);
    variable_pool.find ("out_root", dir_path_type);
    variable_pool.find ("src_base", dir_path_type);
    variable_pool.find ("out_base", dir_path_type);

    variable_pool.find ("project", string_type);
    variable_pool.find ("amalgamation", dir_path_type);

    // Shouldn't be typed since the value requires pre-processing.
    //
    variable_pool.find ("subprojects", nullptr, '=');

    // Create global scope. For Win32 this is not a "real" root path.
    // On POSIX, however, this is a real path. See the comment in
    // <build/path-map> for details.
    //
    global_scope = scopes.insert (
      dir_path ("/"), nullptr, true, false)->second;

    global_scope->assign ("work") = work;
    global_scope->assign ("home") = home;

    // Register builtin target types.
    //
    {
      target_type_map& tts (global_scope->target_types);

      tts.insert<file>  ();
      tts.insert<alias> ();
      tts.insert<dir>   ();
      tts.insert<fsdir> ();
      tts.insert<doc>   ();
      tts.insert<man>   ();
      tts.insert<man1>  ();
    }

    // Register builtin rules.
    //
    {
      rule_map& rs (global_scope->rules);

      rs.insert<alias> (perform_id, 0, "alias", alias_rule::instance);

      rs.insert<fsdir> (perform_id, update_id, "fsdir", fsdir_rule::instance);
      rs.insert<fsdir> (perform_id, clean_id, "fsdir", fsdir_rule::instance);

      rs.insert<file> (perform_id, update_id, "file", file_rule::instance);
      rs.insert<file> (perform_id, clean_id, "file", file_rule::instance);
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
    catch (const std::system_error& e)
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
  string
  diag_do (const action&, const target& t)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    ostringstream os;

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
    return os.str ();
  }

  string
  diag_doing (const action&, const target& t)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    ostringstream os;

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
    return os.str ();
  }

  string
  diag_done (const action&, const target& t)
  {
    const meta_operation_info& m (*current_mif);
    const operation_info& io (*current_inner_oif);
    const operation_info* oo (current_outer_oif);

    ostringstream os;

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

    return os.str ();
  }
}
