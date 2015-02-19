// file      : build/b.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <time.h>      // tzset()
#include <string.h>    // strerror()

#include <stdlib.h>    // getenv()
#include <unistd.h>    // getuid()
#include <sys/types.h> // uid_t
#include <pwd.h>       // struct passwd, getpwuid()

#include <vector>
#include <cassert>
#include <fstream>
#include <iostream>  //@@ TMP, for dump()
#include <typeinfo>
#include <system_error>

#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/rule>
#include <build/algorithm>
#include <build/process>
#include <build/diagnostics>
#include <build/context>
#include <build/utility>

#include <build/lexer>
#include <build/parser>

using namespace std;

namespace build
{
  bool
  match_recursive (target& t)
  {
    // Because we match the target first and then prerequisites,
    // any additional dependency information injected by the rule
    // will be covered as well.
    //
    if (!t.recipe ())
    {
      if (!match (t))
      {
        error << "no rule to update target " << t;
        return false;
      }
    }

    for (prerequisite& p: t.prerequisites)
    {
      // Resolve prerequisite to target (prerequisite search). We
      // do this after matching since the rule can alter search
      // paths.
      //
      if (p.target == nullptr)
        search (p);

      if (!match_recursive (*p.target))
      {
        info << "required by " << t;
        return false;
      }
    }

    return true;
  }

  target_state
  update (target& t)
  {
    assert (t.state () == target_state::unknown);

    auto g (
      make_exception_guard (
        [](target& t){info << "while building target " << t;},
        t));

    for (prerequisite& p: t.prerequisites)
    {
      target& pt (*p.target);

      if (pt.state () == target_state::unknown)
      {
        target_state ts (update (pt));

        if (ts == target_state::failed)
          return ts;
      }
    }

    // @@ Why do we indicate failure via code rather than throw? Now
    //    there is no diagnostics via exception_guard above.

    const recipe& r (t.recipe ());

    target_state ts (r (t));

    assert (ts != target_state::unknown);
    t.state (ts);
    return ts;
  }

  void
  dump ()
  {
    cout << endl;

    for (const auto& pt: targets)
    {
      target& t (*pt);

      cout << t << ':';

      for (const auto& p: t.prerequisites)
      {
        cout << ' ' << p;
      }

      cout << endl;
    }

    cout << endl;
  }

}

#include <build/native>

#include <build/cxx/target>
#include <build/cxx/rule>


using namespace build;

int
main (int argc, char* argv[])
{
  try
  {
    tracer trace ("main");

    // Initialize time conversion data that is used by localtime_r().
    //
    tzset ();

    // Trace verbosity.
    //
    verb = 5;

    // Register target types.
    //
    target_types.insert (file::static_type);
    target_types.insert (dir::static_type);

    target_types.insert (exe::static_type);
    target_types.insert (obj::static_type);

    target_types.insert (cxx::h::static_type);
    target_types.insert (cxx::c::static_type);

    target_types.insert (cxx::cxx::static_type);
    target_types.insert (cxx::hxx::static_type);
    target_types.insert (cxx::ixx::static_type);
    target_types.insert (cxx::txx::static_type);

    // Figure out directories: work, home, and {src,out}_{root,base}.
    //
    work = path::current ();

    if (const char* h = getenv ("HOME"))
      home = path (h);
    else
    {
      struct passwd* pw (getpwuid (getuid ()));

      if (pw == nullptr)
      {
        const char* msg (strerror (errno));
        fail << "unable to determine home directory: " << msg;
      }

      home = path (pw->pw_dir);
    }

    //@@ Must be normalized.
    //
    out_base = work;
    src_base = out_base;

    // The project's root directory is the one that contains the build/
    // sub-directory which contains the pre.build file.
    //
    for (path d (src_base); !d.root () && d != home; d = d.directory ())
    {
      path f (d / path ("build/pre.build"));
      if (path_mtime (f) != timestamp_nonexistent)
      {
        src_root = d;
        break;
      }
    }

    if (src_root.empty ())
    {
      src_root = src_base;
      out_root = out_base;
    }
    else
      out_root = out_base.directory (src_base.leaf (src_root));

    if (verb >= 4)
    {
      trace << "work dir: " << work.string ();
      trace << "home dir: " << home.string ();
      trace << "out_base: " << out_base.string ();
      trace << "src_base: " << src_base.string ();
      trace << "out_root: " << out_root.string ();
      trace << "src_root: " << src_root.string ();
    }

    // Parse buildfile.
    //
    path bf ("buildfile");

    ifstream ifs (bf.string ());
    if (!ifs.is_open ())
      fail << "unable to open " << bf;

    ifs.exceptions (ifstream::failbit | ifstream::badbit);
    parser p;

    try
    {
      p.parse (ifs, bf, scopes[path::current ()]);
    }
    catch (const std::ios_base::failure&)
    {
      fail << "failed to read from " << bf;
    }

    dump ();

    // Register rules.
    //
    cxx::link cxx_link;
    rules[typeid (exe)].emplace ("cxx.gnu.link", cxx_link);

    cxx::compile cxx_compile;
    rules[typeid (obj)].emplace ("cxx.gnu.compile", cxx_compile);

    dir_rule dir_r;
    rules[typeid (dir)].emplace ("", dir_r);

    path_rule path_r;
    rules[typeid (path_target)].emplace ("", path_r);

    // Build.
    //
    if (default_target == nullptr)
    {
      fail << "no default target";
    }

    target& d (*default_target);

    if (!match_recursive (d))
      return 1; // Diagnostics has already been issued.

    dump ();

    switch (update (d))
    {
    case target_state::uptodate:
      {
        info << "target " << d << " is up to date";
        break;
      }
    case target_state::updated:
      break;
    case target_state::failed:
      {
        fail << "failed to update target " << d;
      }
    case target_state::unknown:
      assert (false);
    }
  }
  catch (const failed&)
  {
    return 1; // Diagnostics has already been issued.
  }
  catch (const std::exception& e)
  {
    error << e.what ();
    return 1;
  }
}
