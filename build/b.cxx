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
#include <iostream>
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
        cerr << "error: no rule to update target " << t << endl;
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
        cerr << "info: required by " << t << endl;
        return false;
      }
    }

    return true;
  }

  target_state
  update (target& t)
  {
    assert (t.state () == target_state::unknown);

    target_state ts;

    for (prerequisite& p: t.prerequisites)
    {
      target& pt (*p.target);

      if (pt.state () == target_state::unknown)
      {
        pt.state ((ts = update (pt)));

        if (ts == target_state::failed)
          return ts;
      }
    }

    const recipe& r (t.recipe ());

    {
      auto g (
        make_exception_guard (
          [] (target& t)
          {
            cerr << "info: while building target " << t << endl;
          },
          t));

      ts = r (t);
    }

    assert (ts != target_state::unknown);
    t.state (ts);
    return ts;
  }

  void
  dump ()
  {
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
  }

}

#include <build/native>

#include <build/cxx/target>
#include <build/cxx/rule>


using namespace build;

int
main (int argc, char* argv[])
{
  // Initialize time conversion data that is used by localtime_r().
  //
  tzset ();

  // Register target types.
  //
  target_types.insert (file::static_type);

  target_types.insert (exe::static_type);
  target_types.insert (obj::static_type);

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
      cerr << "error: unable to determine home directory: " << msg << endl;
      return 1;
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

  cerr << "work dir: " << work << endl;
  cerr << "home dir: " << home << endl;
  cerr << "out_base: " << out_base << endl;
  cerr << "src_base: " << src_base << endl;
  cerr << "out_root: " << out_root << endl;
  cerr << "src_root: " << src_root << endl;

  // Parse buildfile.
  //
  path bf ("buildfile");

  ifstream ifs (bf.string ().c_str ());
  if (!ifs.is_open ())
  {
    cerr << "error: unable to open " << bf << " in read mode" << endl;
    return 1;
  }

  ifs.exceptions (ifstream::failbit | ifstream::badbit);
  parser p (cerr);

  try
  {
    p.parse (ifs, bf, scopes[path::current ()]);
  }
  catch (const lexer_error&)
  {
    return 1; // Diagnostics has already been issued.
  }
  catch (const parser_error&)
  {
    return 1; // Diagnostics has already been issued.
  }
  catch (const std::ios_base::failure&)
  {
    cerr << "error: failed to read from " << bf << endl;
    return 1;
  }

  dump ();

  // Register rules.
  //
  cxx::link cxx_link;
  rules.emplace (typeid (exe), cxx_link);

  cxx::compile cxx_compile;
  rules.emplace (typeid (obj), cxx_compile);

  default_path_rule path_exists;
  rules.emplace (typeid (path_target), path_exists);

  // Build.
  //
  if (default_target == nullptr)
  {
    cerr << "error: no default target" << endl;
    return 1;
  }

  try
  {
    target& d (*default_target);

    if (!match_recursive (d))
      return 1; // Diagnostics has already been issued.

    //dump ();

    switch (update (d))
    {
    case target_state::uptodate:
      {
        cerr << "info: target " << d << " is up to date" << endl;
        break;
      }
    case target_state::updated:
      break;
    case target_state::failed:
      {
        cerr << "error: failed to update target " << d << endl;
        return 1;
      }
    case target_state::unknown:
      assert (false);
    }
  }
  catch (const error&)
  {
    return 1; // Diagnostics has already been issued.
  }
  catch (const std::exception& e)
  {
    cerr << "error: " << e.what () << endl;
    return 1;
  }
}
