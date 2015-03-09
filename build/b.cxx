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
#include <sstream>
#include <iostream>  //@@ TMP, for dump()
#include <typeinfo>
#include <system_error>

#include <build/spec>
#include <build/name>
#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/rule>
#include <build/algorithm>
#include <build/process>
#include <build/diagnostics>
#include <build/context>
#include <build/utility>
#include <build/dump>

#include <build/lexer>
#include <build/parser>

using namespace std;

namespace build
{
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
    target_types.insert (fsdir::static_type);

    target_types.insert (exe::static_type);
    target_types.insert (obj::static_type);

    target_types.insert (cxx::h::static_type);
    target_types.insert (cxx::c::static_type);

    target_types.insert (cxx::cxx::static_type);
    target_types.insert (cxx::hxx::static_type);
    target_types.insert (cxx::ixx::static_type);
    target_types.insert (cxx::txx::static_type);

    // Figure out work and home directories.
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

    if (verb >= 4)
    {
      trace << "work dir: " << work.string ();
      trace << "home dir: " << home.string ();
    }

    // Create root scope. For Win32 we use the empty path since there
    // is no such "real" root path. On POSIX, however, this is a real
    // path. See the comment in <build/path-map> for details.
    //
#ifdef _WIN32
    root_scope = &scopes[path ()];
#else
    root_scope = &scopes[path ("/")];
#endif

    root_scope->variables["work"] = work;
    root_scope->variables["home"] = home;

    // Parse the buildspec.
    //
    buildspec bspec;
    {
      // Merge all the individual buildspec arguments into a single
      // string. Instead, we could also parse them individually (
      // and merge the result). The benefit of doing it this way
      // is potentially better diagnostics (i.e., we could have
      // used <buildspec-1>, <buildspec-2> to give the idea about
      // which argument is invalid).
      //
      string s;
      for (int i (1); i != argc;)
      {
        s += argv[i];
        if (++i != argc)
          s += ' ';
      }

      istringstream is (s);
      is.exceptions (ifstream::failbit | ifstream::badbit);
      parser p;

      try
      {
        bspec = p.parse_buildspec (is, "<buildspec>");
      }
      catch (const std::ios_base::failure&)
      {
        fail << "failed to parse buildspec string";
      }
    }

    level4 ([&]{trace << "buildspec: " << bspec;});

    // Load all the buildfiles.
    //
    if (bspec.empty ())
      bspec.push_back (metaopspec ()); // Default meta-operation.

    for (metaopspec& ms: bspec)
    {
      if (ms.empty ())
        ms.push_back (opspec ()); // Default operation.

      for (opspec& os: ms)
      {
        if (os.empty ())
          // Default target: dir{}.
          //
          os.push_back (targetspec (name ("dir", path (), string ())));

        for (targetspec& ts: os)
        {
          name& tn (ts.target);

          // First figure out the out_base of this target. The logic
          // is as follows: if a directory was specified in any form,
          // then that's the out_base. Otherwise, we check if the name
          // value has a directory prefix. This has a good balance of
          // control and the expected result in most cases.
          //
          path out_base (tn.dir);
          if (out_base.empty ())
          {
            // See if there is a directory part in value. We cannot
            // assume it is a valid filesystem name so we will have
            // to do the splitting manually.
            //
            path::size_type i (path::traits::rfind_separator (tn.value));

            if (i != string::npos)
              out_base = path (tn.value, i != 0 ? i : 1); // Special case: "/".
          }

          if (out_base.relative ())
            out_base = work / out_base;

          out_base.normalize ();

          path& src_base (ts.src_base);
          if (src_base.empty ())
          {
            //@@ TODO: Configured case: find out_root (looking for
            //   "build/bootstrap.build" or some such), then src_root
            //   (stored in this file). Need to also detect the in-tree
            //   build.
            //

            // If that doesn't work out (e.g., the first build), then
            // default to the working directory as src_base.
            //
            src_base = work;
          }

          if (src_base.relative ())
            src_base = work / src_base;

          src_base.normalize ();

          path src_root;
          path out_root;

          // The project's root directory is the one that contains the build/
          // sub-directory which contains the pre.build file.
          //
          for (path d (src_base), f ("build/pre.build");
               !d.root () && d != home;
               d = d.directory ())
          {
            if (path_mtime (d / f) != timestamp_nonexistent)
            {
              src_root = d;
              break;
            }
          }

          // If there is no such sub-directory, assume this is a simple
          // project with src_root being the same as src_base.
          //
          if (src_root.empty ())
          {
            src_root = src_base;
            out_root = out_base;
          }
          else
            out_root = out_base.directory (src_base.leaf (src_root));

          if (verb >= 4)
          {
            trace << tn << ':';
            trace << "  out_base: " << out_base.string ();
            trace << "  src_base: " << src_base.string ();
            trace << "  out_root: " << out_root.string ();
            trace << "  src_root: " << src_root.string ();
          }

          // Create project root and base scopes, set the corresponding
          // variables. Note that we might already have all of this set
          // up as a result of one of the preceding target processing.
          //
          scope& proot_scope (scopes[out_root]);
          scope& pbase_scope (scopes[out_base]);

          proot_scope.variables["out_root"] = move (out_root);
          proot_scope.variables["src_root"] = move (src_root);

          pbase_scope.variables["out_base"] = out_base;
          pbase_scope.variables["src_base"] = src_base;

          // Parse the buildfile.
          //
          path bf (src_base / path ("buildfile"));

          // Check if this buildfile has already been loaded.
          //
          if (!proot_scope.buildfiles.insert (bf).second)
          {
            level4 ([&]{trace << "skipping already loaded " << bf;});
            continue;
          }

          level4 ([&]{trace << "loading " << bf;});

          ifstream ifs (bf.string ());
          if (!ifs.is_open ())
            fail << "unable to open " << bf;

          ifs.exceptions (ifstream::failbit | ifstream::badbit);
          parser p;

          try
          {
            p.parse_buildfile (ifs, bf, pbase_scope, proot_scope);
          }
          catch (const std::ios_base::failure&)
          {
            fail << "failed to read from " << bf;
          }
        }
      }
    }

    dump_scopes ();
    dump ();

    // Register rules.
    //
    cxx::link cxx_link;
    rules[typeid (exe)].emplace ("cxx.gnu.link", cxx_link);

    cxx::compile cxx_compile;
    rules[typeid (obj)].emplace ("cxx.gnu.compile", cxx_compile);

    dir_rule dir_r;
    rules[typeid (dir)].emplace ("dir", dir_r);

    fsdir_rule fsdir_r;
    rules[typeid (fsdir)].emplace ("fsdir", fsdir_r);

    path_rule path_r;
    rules[typeid (path_target)].emplace ("path", path_r);

    // Do the operations. We do meta-operations and operations sequentially
    // (no parallelism).
    //
    for (metaopspec& ms: bspec)
    {
      for (opspec& os: ms)
      {
        // But multiple targets in the same operation can be done in
        // parallel.
        //
        vector<reference_wrapper<target>> tgs;
        tgs.reserve (os.size ());

        // First resolve and match all the targets. We don't want to
        // start building before we know how for all the targets in
        // this operation.
        //
        for (targetspec& ts: os)
        {
          name& tn (ts.target);
          const location l ("<buildspec>", 1, 0); //@@ TODO

          const string* e;
          const target_type* ti (target_types.find (tn, e));

          if (ti == nullptr)
            fail (l) << "unknown target type " << tn.type;

          // If the directory is relative, assume it is relative to work
          // (must be consistent with how we derive out_base).
          //
          path& d (tn.dir);

          if (d.relative ())
            d = work / d;

          d.normalize ();

          target_set::key tk {ti, &d, &tn.value, &e};
          auto i (targets.find (tk, trace));
          if (i == targets.end ())
            fail (l) << "unknown target " << tk;

          target& t (**i);

          if (!t.recipe ())
          {
            level4 ([&]{trace << "matching target " << t;});
            match (t);
          }

          tgs.push_back (t);
        }

        dump ();

        // Now build.
        //
        for (target& t: tgs)
        {
          // The target might have already been updated indirectly. We
          // still want to inform the user about its status since they
          // requested its update explicitly.
          //
          target_state s (t.state ());
          if (s == target_state::unknown)
          {
            level4 ([&]{trace << "updating target " << t;});
            s = update (t);
          }

          switch (s)
          {
          case target_state::uptodate:
            {
              info << "target " << t << " is up to date";
              break;
            }
          case target_state::updated:
            break;
          case target_state::failed:
            //@@ This could probably happen in a parallel build.
          case target_state::unknown:
            assert (false);
          }
        }
      }
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
