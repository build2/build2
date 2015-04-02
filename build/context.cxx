// file      : build/context.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
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
  path work;
  path home;

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
    global_scope = &scopes[path ()];
#else
    global_scope = &scopes[path ("/")];
#endif

    global_scope->variables["work"] = work;
    global_scope->variables["home"] = home;
  }

  fs_status<mkdir_status>
  mkdir (const path& d)
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
      if (verb >= 1)
        text << "mkdir " << d.string ();
      else
        text << "mkdir " << d;

      fail << "unable to create directory " << d.string () << ": "
           << e.what ();
    }

    if (ms == mkdir_status::success)
    {
      if (verb >= 1)
        text << "mkdir " << d.string ();
      else
        text << "mkdir " << d;
    }

    return ms;
  }

  path
  src_out (const path& out, scope& s)
  {
    return src_out (out,
                    s["out_root"].as<const path&> (),
                    s["src_root"].as<const path&> ());
  }

  path
  out_src (const path& src, scope& s)
  {
    return out_src (src,
                    s["out_root"].as<const path&> (),
                    s["src_root"].as<const path&> ());
  }

  path
  src_out (const path& o, const path& out_root, const path& src_root)
  {
    assert (o.sub (out_root));
    return src_root / o.leaf (out_root);
  }

  path
  out_src (const path& s, const path& out_root, const path& src_root)
  {
    assert (s.sub (src_root));
    return out_root / s.leaf (src_root);
  }

  const path* relative_base = &work;

  path
  relative (const path& p)
  {
    const path& b (*relative_base);

    if (b.empty ())
      return p;

    if (p.sub (b))
      return p.leaf (b);

    // If base is a sub-path of {src,out}_root and this path is also a
    // sub-path of it, then use '..' to form a relative path.
    //
    // Don't think this is a good heuristic. For example, why shouldn't
    // we display paths from imported projects as relative if they are
    // more readable than absolute?
    //
    /*
    if ((work.sub (src_root) && p.sub (src_root)) ||
        (work.sub (out_root) && p.sub (out_root)))
      return p.relative (work);
    */

    if (p.root_directory () == b.root_directory ())
    {
      path r (p.relative (b));

      if (r.string ().size () < p.string ().size ())
        return r;
    }

    return p;
  }
}
