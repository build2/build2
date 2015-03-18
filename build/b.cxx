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
#include <iterator> // make_move_iterator()
#include <iostream> //@@ TMP, for dump()
#include <typeinfo>
#include <system_error>

#include <build/path>
#include <build/name>
#include <build/spec>
#include <build/operation>
#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/rule>
#include <build/module>
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

  inline bool
  is_src_root (const path& d)
  {
    return path_mtime (d / path ("build/root.build")) !=
      timestamp_nonexistent;
  }

  inline bool
  is_out_root (const path& d)
  {
    return path_mtime (d / path ("build/bootstrap/src-root.build")) !=
      timestamp_nonexistent;
  }

  // Given an src_base directory, look for the project's src_root
  // based on the presence of known special files. Return empty
  // path if not found.
  //
  path
  find_src_root (const path& b)
  {
    for (path d (b); !d.root () && d != home; d = d.directory ())
    {
      if (is_src_root (d))
        return d;
    }

    return path ();
  }

  // The same but for out. Note that we also check whether a
  // directory happens to be src_root, in case this is an in-
  // tree build.
  //
  path
  find_out_root (const path& b)
  {
    for (path d (b); !d.root () && d != home; d = d.directory ())
    {
      if (is_out_root (d) || is_src_root (d))
        return d;
    }

    return path ();
  }

  void
  bootstrap (scope& rs)
  {
    tracer trace ("bootstrap");

    path bf (rs.path () / path ("build/bootstrap/src-root.build"));

    if (path_mtime (bf) == timestamp_nonexistent)
      return;

    //@@ TODO: if bootstrap files can source other bootstrap files
    //   (the way to express dependecies), then we need a way to
    //   prevent multiple sourcing.
    //

    level4 ([&]{trace << "loading " << bf;});

    ifstream ifs (bf.string ());
    if (!ifs.is_open ())
      fail << "unable to open " << bf;

    ifs.exceptions (ifstream::failbit | ifstream::badbit);
    parser p;

    try
    {
      p.parse_buildfile (ifs, bf, rs, rs);
    }
    catch (const std::ios_base::failure&)
    {
      fail << "failed to read from " << bf;
    }
  }

  void
  root_pre (scope& rs, const path& src_root)
  {
    tracer trace ("root_pre");

    path bf (src_root / path ("build/root.build"));

    if (path_mtime (bf) == timestamp_nonexistent)
      return;

    level4 ([&]{trace << "loading " << bf;});

    ifstream ifs (bf.string ());
    if (!ifs.is_open ())
      fail << "unable to open " << bf;

    ifs.exceptions (ifstream::failbit | ifstream::badbit);
    parser p;

    try
    {
      p.parse_buildfile (ifs, bf, rs, rs);
    }
    catch (const std::ios_base::failure&)
    {
      fail << "failed to read from " << bf;
    }
  }
}

#include <build/native>

#include <build/cxx/target>
#include <build/cxx/rule>

#include <build/config/module>

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

    // Register modules.
    //
    modules["config"] = &config::load;

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

    // Enter built-in meta-operation and operation names into tables.
    // Note that the order of registration should match the id constants;
    // see <operation> for details. Loading of the buildfiles can result
    // in additional names being added (via module loading).
    //
    meta_operations.insert (meta_operation_info {"perform"});
    meta_operations.insert (meta_operation_info {"configure"});
    meta_operations.insert (meta_operation_info {"disfigure"});

    operations.insert (operation_info {"update", execution_mode::first});
    operations.insert (operation_info {"clean", execution_mode::last});

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
          name& tn (ts.name);

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

          // The order in which we determine the roots depends on whether
          // src_base was specified explicitly. There will also be a few
          // cases where we are guessing things that can turn out wrong.
          // Keep track of that so that we can issue more extensive
          // diagnostics for such cases.
          //
          bool guessing (false);
          path src_root;
          path out_root;

          path& src_base (ts.src_base); // Update it in buildspec.

          if (!src_base.empty ())
          {
            if (src_base.relative ())
              src_base = work / src_base;

            src_base.normalize ();

            // If the src_base was explicitly specified, search for src_root.
            //
            src_root = find_src_root (src_base);

            // If not found, assume this is a simple project with src_root
            // being the same as src_base.
            //
            if (src_root.empty ())
            {
              src_root = src_base;
              out_root = out_base;
            }
            else
              // Calculate out_root based on src_root/src_base.
              //
              out_root = out_base.directory (src_base.leaf (src_root));
          }
          else
          {
            // If no src_base was explicitly specified, search for out_root.
            //
            out_root = find_out_root (out_base);

            // If not found (i.e., we have no idea where the roots are),
            // then this can mean two things: an in-tree build of a
            // simple project or a fresh out-of-tree build. Assume this
            // is the former and set out_root to out_base. If we are
            // wrong (most likely) and this is the latter, then things
            // will go badly when we try to load the buildfile.
            //
            if (out_root.empty ())
            {
              src_root = src_base = out_root = out_base;
              guessing = true;
            }
          }

          // Now we know out_root and, if it was explicitly specified,
          // src_root. The next step is to create the root scope and
          // load the bootstrap files, if any. Note that we might already
          // have done this as a result of one of the preceding target
          // processing.
          //
          auto rsp (scopes.insert (out_root));
          scope& rs (rsp.first);

          if (rsp.second)
          {
            rs.variables["out_root"] = out_root;
            bootstrap (rs);
          }

          // See if the bootstrap process set src_root.
          //
          {
            auto v (rs.variables["src_root"]);

            if (v)
            {
              // If we also have src_root specified by the user, make
              // sure they match.
              //
              const path& p (v.as<const path&> ());

              if (src_root.empty ())
                src_root = p;
              else if (src_root != p)
                fail << "bootstrapped src_root " << p << " does not match "
                     << "specified " << src_root;
            }
            else
            {
              // Bootstrap didn't produce src_root.
              //
              if (src_root.empty ())
              {
                // If it also wasn't explicitly specified, see if it is
                // the same as out_root.
                //
                if (is_src_root (out_root))
                  src_root = out_root;
                else
                {
                  // If not, then assume we are running from src_base
                  // and calculate src_root based on out_root/out_base.
                  // Note that this is different from the above case
                  // were we couldn't determine either root.
                  //
                  src_base = work;
                  src_root = src_base.directory (out_base.leaf (out_root));
                  guessing = true;
                }
              }

              v = src_root;
            }
          }

          // At this stage we should have both roots and out_base figured
          // out. If src_base is still undetermined, calculate it.
          //
          if (src_base.empty ())
            src_base = src_root / out_base.leaf (out_root);

          if (verb >= 4)
          {
            trace << tn << ':';
            trace << "  out_base: " << out_base.string ();
            trace << "  src_base: " << src_base.string ();
            trace << "  out_root: " << out_root.string ();
            trace << "  src_root: " << src_root.string ();
          }

          // Load project's root[-pre].build.
          //
          root_pre (rs, src_root);

          // Create the base scope. Note that its existence doesn't
          // mean it was already processed as a base scope; it can
          // be the same as root.
          //
          scope& bs (scopes[out_base]);

          bs.variables["out_base"] = out_base;
          bs.variables["src_base"] = src_base;

          // Parse the buildfile.
          //
          path bf (src_base / path ("buildfile"));

          // Check if this buildfile has already been loaded.
          //
          if (!rs.buildfiles.insert (bf).second)
          {
            level4 ([&]{trace << "skipping already loaded " << bf;});
            continue;
          }

          level4 ([&]{trace << "loading " << bf;});

          ifstream ifs (bf.string ());
          if (!ifs.is_open ())
          {
            diag_record dr;
            dr << fail << "unable to open " << bf;
            if (guessing)
              dr << info << "consider explicitly specifying src_base "
                 << "for " << tn;
          }

          ifs.exceptions (ifstream::failbit | ifstream::badbit);
          parser p;

          try
          {
            p.parse_buildfile (ifs, bf, bs, rs);
          }
          catch (const std::ios_base::failure&)
          {
            fail << "failed to read from " << bf;
          }
        }
      }
    }

    // We store the combined action id in uint8_t; see <operation> for
    // details.
    //
    assert (operations.size () <= 128);
    assert (meta_operations.size () <= 128);

    dump_scopes ();
    dump ();

    // At this stage we know all the names of meta-operations and
    // operations so "lift" names that we assumed (from buildspec
    // syntax) were operations but are actually meta-operations.
    // Also convert empty names (which means they weren't explicitly
    // specified) to the defaults and verify that all the names are
    // known.
    //
    for (auto mi (bspec.begin ()); mi != bspec.end (); ++mi)
    {
      metaopspec& ms (*mi);
      const location l ("<buildspec>", 1, 0); //@@ TODO

      for (auto oi (ms.begin ()); oi != ms.end (); ++oi)
      {
        opspec& os (*oi);
        const location l ("<buildspec>", 1, 0); //@@ TODO

        if (os.name.empty ())
        {
          os.name = "update";
          continue;
        }

        if (meta_operations.find (os.name) != 0)
        {
          if (!ms.name.empty ())
            fail (l) << "nested meta-operation " << os.name;

          // The easy case is when the metaopspec contains a
          // single opspec (us). In this case we can just move
          // the name.
          //
          if (ms.size () == 1)
          {
            ms.name = move (os.name);
            os.name = "update";
            continue;
          }
          // The hard case is when there are other operations that
          // need to keep their original meta-operation. In this
          // case we have to "split" the metaopspec, in the worst
          // case scenario, into three parts: prefix, us, and suffix.
          //
          else
          {
            if (oi != ms.begin ()) // We have a prefix of opspec's.
            {
              // Keep the prefix in the original metaopspec and move
              // the suffix into a new one that is inserted after the
              // prefix. Then simply let the loop finish with the prefix
              // and naturally move to the suffix (in other words, we
              // are reducing this case to the one without a prefix).
              //
              metaopspec suffix;
              suffix.insert (suffix.end (),
                             make_move_iterator (oi),
                             make_move_iterator (ms.end ()));
              ms.resize (oi - ms.begin ());

              mi = bspec.insert (++mi, move (suffix)); // Insert after prefix.
              --mi; // Move back to prefix.
              break;
            }

            // We are the first element and have a suffix of opspec's
            // (otherwise one of the previous cases would have matched).
            //
            assert (oi == ms.begin () && (oi + 1) != ms.end ());

            // Move this opspec into a new metaopspec and insert it before
            // the current one. Then continue with the next opspec.
            //
            metaopspec prefix (move (os.name));
            os.name = "update";
            prefix.push_back (move (os));
            ms.erase (oi);

            mi = bspec.insert (mi, move (prefix)); // Insert before suffix.
            break; // Restart inner loop: outer loop ++ moves back to suffix.
          }
        }

        if (operations.find (os.name) == 0)
          fail (l) << "unknown operation " << os.name;
      }

      // Note: using mi rather than ms since ms is invalidated by above
      // insert()'s.
      //
      if (mi->name.empty ())
        mi->name = "perform";
      else if (meta_operations.find (mi->name) == 0)
        fail (l) << "unknown meta-operation " << mi->name;
    }

    level4 ([&]{trace << "buildspec: " << bspec;});

    // Register rules.
    //
    cxx::link cxx_link;
    rules["update"][typeid (exe)].emplace ("cxx.gnu.link", cxx_link);
    rules["clean"][typeid (exe)].emplace ("cxx.gnu.link", cxx_link);

    cxx::compile cxx_compile;
    rules["update"][typeid (obj)].emplace ("cxx.gnu.compile", cxx_compile);
    rules["clean"][typeid (obj)].emplace ("cxx.gnu.compile", cxx_compile);

    dir_rule dir_r;
    rules["update"][typeid (dir)].emplace ("dir", dir_r);
    rules["clean"][typeid (dir)].emplace ("dir", dir_r);

    fsdir_rule fsdir_r;
    rules["update"][typeid (fsdir)].emplace ("fsdir", fsdir_r);
    rules["clean"][typeid (fsdir)].emplace ("fsdir", fsdir_r);

    path_rule path_r;
    rules["update"][typeid (path_target)].emplace ("path", path_r);
    rules["clean"][typeid (path_target)].emplace ("path", path_r);


    // Do the operations. We do meta-operations and operations sequentially
    // (no parallelism).
    //
    for (metaopspec& ms: bspec)
    {
      for (opspec& os: ms)
      {
        action act (meta_operations.find (ms.name), operations.find (os.name));

        current_mode = operations[act.operation ()].mode;
        current_rules = &rules[os.name];

        level4 ([&]{trace << ms.name << " " << os.name << " " << act;});

        // Multiple targets in the same operation can be done in parallel.
        //
        vector<reference_wrapper<target>> tgs, psp;
        tgs.reserve (os.size ());

        // First resolve and match all the targets. We don't want to
        // start building before we know how for all the targets in
        // this operation.
        //
        for (targetspec& ts: os)
        {
          name& tn (ts.name);
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

          level4 ([&]{trace << "matching target " << t;});
          match (act, t);

          tgs.push_back (t);
        }

        dump ();

        // Now build.
        //
        for (target& t: tgs)
        {
          level4 ([&]{trace << "updating target " << t;});

          switch (execute (act, t))
          {
          case target_state::postponed:
            {
              info << "target " << t << " is postponed";
              psp.push_back (t);
              break;
            }
          case target_state::unchanged:
            {
              info << "target " << t << " is up to date";
              break;
            }
          case target_state::changed:
            break;
          case target_state::failed:
            //@@ This could probably happen in a parallel build.
          default:
            assert (false);
          }
        }

        // Re-examine postponed targets.
        //
        for (target& t: psp)
        {
          switch (t.state)
          {
          case target_state::postponed:
            {
              info << "target " << t << " unable to do at this time";
              break;
            }
          case target_state::unchanged:
            {
              info << "target " << t << " is up to date";
              break;
            }
          case target_state::unknown: // Assume something was done to it.
          case target_state::changed:
            break;
          case target_state::failed:
            //@@ This could probably happen in a parallel build.
          default:
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
