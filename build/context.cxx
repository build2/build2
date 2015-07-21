// file      : build/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/context>

#include <ostream>
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
  const operation_info* current_oif;
  execution_mode current_mode;

  void
  reset ()
  {
    extension_pool.clear ();
    project_name_pool.clear ();

    targets.clear ();
    scopes.clear ();
    variable_pool.clear ();

    // Enter builtin variables.
    //
    variable_pool.insert (variable ("subprojects", '='));

    // Create global scope. For Win32 we use the empty path since there
    // is no "real" root path. On POSIX, however, this is a real path.
    // See the comment in <build/path-map> for details.
    //
#ifdef _WIN32
    global_scope = &scopes[dir_path ()];
#else
    global_scope = &scopes[dir_path ("/")];
#endif

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
    }

    // Register builtin rules.
    //
    {
      rule_map& rs (global_scope->rules);

      rs.insert<alias> (default_id, "alias", alias_rule::instance);
      rs.insert<alias> (update_id, "alias", alias_rule::instance);
      rs.insert<alias> (clean_id, "alias", alias_rule::instance);

      rs.insert<fsdir> (default_id, "fsdir", fsdir_rule::instance);
      rs.insert<fsdir> (update_id, "fsdir", fsdir_rule::instance);
      rs.insert<fsdir> (clean_id, "fsdir", fsdir_rule::instance);

      rs.insert<file> (default_id, "file", file_rule::instance);
      rs.insert<file> (update_id, "file", file_rule::instance);
      rs.insert<file> (clean_id, "file", file_rule::instance);
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
      text << "mkdir " << d;
      fail << "unable to create directory " << d << ": " << e.what ();
    }

    if (ms == mkdir_status::success)
      text << "mkdir " << d;

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
      text << "mkdir -p " << d;
      fail << "unable to create directory " << d << ": " << e.what ();
    }

    if (ms == mkdir_status::success)
      text << "mkdir -p " << d;

    return ms;
  }

  dir_path
  src_out (const dir_path& out, scope& s)
  {
    scope& rs (*s.root_scope ());
    return src_out (out, rs.path (), rs.src_path ());
  }

  dir_path
  out_src (const dir_path& src, scope& s)
  {
    scope& rs (*s.root_scope ());
    return out_src (src, rs.path (), rs.src_path ());
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

  const dir_path* relative_base = &work;
}
