// file      : build/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/context>

#include <ostream>
#include <cassert>
#include <system_error>

#include <build/scope>
#include <build/diagnostics>

using namespace std;

namespace build
{
  dir_path work;
  dir_path home;

  const meta_operation_info* current_mif;
  const operation_info* current_oif;
  execution_mode current_mode;
  const target_rule_map* current_rules;

  void
  reset ()
  {
    targets.clear ();
    scopes.clear ();
    variable_pool.clear ();

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
