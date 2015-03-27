// file      : build/b.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <time.h>      // tzset()
#include <string.h>    // strerror()

#include <stdlib.h>    // getenv()
#include <unistd.h>    // getuid()
#include <sys/types.h> // uid_t
#include <pwd.h>       // struct passwd, getpwuid()

#include <sstream>
#include <cassert>
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
#include <build/file>
#include <build/module>
#include <build/algorithm>
#include <build/process>
#include <build/diagnostics>
#include <build/context>
#include <build/utility>
#include <build/filesystem>
#include <build/parser>

using namespace std;

namespace build
{
  inline bool
  is_src_root (const path& d)
  {
    return file_exists (d / path ("build/bootstrap.build")) ||
      file_exists (d / path ("build/root.build"));
  }

  inline bool
  is_out_root (const path& d)
  {
    return file_exists (d / path ("build/bootstrap/src-root.build"));
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
  find_out_root (const path& b, bool& src)
  {
    for (path d (b); !d.root () && d != home; d = d.directory ())
    {
      if ((src = is_src_root (d)) || is_out_root (d))
        return d;
    }

    src = false;
    return path ();
  }

  static void
  bootstrap_out (scope& root)
  {
    path bf (root.path () / path ("build/bootstrap/src-root.build"));

    if (!file_exists (bf))
      return;

    //@@ TODO: if bootstrap files can source other bootstrap files
    //   (the way to express dependecies), then we need a way to
    //   prevent multiple sourcing. We handle it here but we still
    //   need something like source_once (once [scope] source).
    //
    source_once (bf, root, root);
  }

  // Return true if we loaded anything.
  //
  static bool
  bootstrap_src (scope& root)
  {
    tracer trace ("bootstrap_src");

    path bf (root.src_path () / path ("build/bootstrap.build"));

    if (!file_exists (bf))
      return false;

    // We assume that bootstrap out cannot load this file explicitly. It
    // feels wrong to allow this since that makes the whole bootstrap
    // process hard to reason about. But we may try to bootstrap the
    // same root scope multiple time.
    //
    source_once (bf, root, root);
    return true;
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
    modules["config"] = &config::init;

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

    // Register rules.
    //
    cxx::link cxx_link;
    rules[default_id][typeid (exe)].emplace ("cxx.gnu.link", cxx_link);
    rules[update_id][typeid (exe)].emplace ("cxx.gnu.link", cxx_link);
    rules[clean_id][typeid (exe)].emplace ("cxx.gnu.link", cxx_link);

    cxx::compile cxx_compile;
    rules[default_id][typeid (obj)].emplace ("cxx.gnu.compile", cxx_compile);
    rules[update_id][typeid (obj)].emplace ("cxx.gnu.compile", cxx_compile);
    rules[clean_id][typeid (obj)].emplace ("cxx.gnu.compile", cxx_compile);

    dir_rule dir_r;
    rules[default_id][typeid (dir)].emplace ("dir", dir_r);
    rules[update_id][typeid (dir)].emplace ("dir", dir_r);
    rules[clean_id][typeid (dir)].emplace ("dir", dir_r);

    fsdir_rule fsdir_r;
    rules[default_id][typeid (fsdir)].emplace ("fsdir", fsdir_r);
    rules[update_id][typeid (fsdir)].emplace ("fsdir", fsdir_r);
    rules[clean_id][typeid (fsdir)].emplace ("fsdir", fsdir_r);

    path_rule path_r;
    rules[default_id][typeid (path_target)].emplace ("path", path_r);
    rules[update_id][typeid (path_target)].emplace ("path", path_r);
    rules[clean_id][typeid (path_target)].emplace ("path", path_r);

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

    // Initialize the dependency state.
    //
    reset ();

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
      is.exceptions (istringstream::failbit | istringstream::badbit);
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

    if (bspec.empty ())
      bspec.push_back (metaopspec ()); // Default meta-operation.

    for (metaopspec& ms: bspec)
    {
      if (ms.empty ())
        ms.push_back (opspec ()); // Default operation.

      meta_operation_id mid (0); // Not yet translated.
      const meta_operation_info* mif (nullptr);

      bool lifted (false); // See below.

      for (opspec& os: ms)
      {
        const location l ("<buildspec>", 1, 0); //@@ TODO

        if (os.empty ()) // Default target: dir{}.
          os.push_back (targetspec (name ("dir", path (), string ())));

        operation_id oid (0); // Not yet translated.
        const operation_info* oif (nullptr);

        action act (0, 0); // Not yet initialized.

        // We do meta-operation and operation batches sequentially (no
        // parallelism). But multiple targets in an operation batch
        // can be done in parallel.
        //
        action_targets tgs;
        tgs.reserve (os.size ());

        // If the previous operation was lifted to meta-operation,
        // end the meta-operation batch.
        //
        if (lifted)
        {
          if (mif->meta_operation_post != nullptr)
            mif->meta_operation_post ();

          level4 ([&]{trace << "end meta-operation batch " << mif->name
                            << ", id " << static_cast<uint16_t> (mid);});

          mid = 0;
          lifted = false;
        }

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
            const string& v (tn.value);

            // Handle a few common cases as special: empty name, '.',
            // '..', as well as dir{foo/bar} (without trailing '/').
            // This code must be consistent with target_type_map::find().
            //
            if (v.empty () || v == "." || v == ".." || tn.type == "dir")
              out_base = path (v);
            else
            {
              // See if there is a directory part in value. We cannot
              // assume it is a valid filesystem name so we will have
              // to do the splitting manually.
              //
              path::size_type i (path::traits::rfind_separator (v));

              if (i != string::npos)
                out_base = path (v, i != 0 ? i : 1); // Special case: "/".
            }
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
            bool src;
            out_root = find_out_root (out_base, src);

            // If not found (i.e., we have no idea where the roots are),
            // then this can mean two things: an in-tree build of a
            // simple project or a fresh out-of-tree build. To test for
            // the latter, try to find src_root starting from work. If
            // we can't, then assume it is the former case.
            //
            if (out_root.empty ())
            {
              src_root = find_src_root (work);

              if (!src_root.empty ())
              {
                src_base = work;

                if (src_root != src_base)
                {
                  try
                  {
                    out_root = out_base.directory (src_base.leaf (src_root));
                  }
                  catch (const invalid_path&)
                  {
                    fail << "out_base directory suffix does not match src_base"
                         << info << "src_base is " << src_base.string ()
                         << info << "src_root is " << src_root.string ()
                         << info << "out_base is " << out_base.string ()
                         << info << "consider explicitly specifying src_base "
                         << "for " << tn;
                  }
                }
                else
                  out_root = out_base;
              }
              else
                src_root = src_base = out_root = out_base;

              guessing = true;
            }
            else if (src)
              src_root = out_root;
          }

          // Now we know out_root and, if it was explicitly specified
          // or the same as out_root, src_root. The next step is to
          // create the root scope and load the out_root bootstrap
          // files, if any. Note that we might already have done this
          // as a result of one of the preceding target processing.
          //
          scope& rs (scopes[out_root]);

          // Enter built-in meta-operation and operation names. Note that
          // the order of registration should match the id constants; see
          // <operation> for details. Loading of modules (via the src_root
          // bootstrap; see below) can result in additional names being
          // added.
          //
          if (rs.meta_operations.empty ())
          {
            assert (rs.meta_operations.insert (perform) == perform_id);

            assert (rs.operations.insert (default_) == default_id);
            assert (rs.operations.insert (update) == update_id);
            assert (rs.operations.insert (clean) == clean_id);
          }

          rs.variables["out_root"] = out_root;

          // If we know src_root, add that variable as well. This could
          // be of use to the bootstrap file (other than src-root.build,
          // which, BTW, doesn't need to exist if src_root == out_root).
          //
          if (!src_root.empty ())
            rs.variables["src_root"] = src_root;

          bootstrap_out (rs);

          // See if the bootstrap process set/changed src_root.
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
                  //
                  src_base = work;
                  src_root = src_base.directory (out_base.leaf (out_root));
                  guessing = true;
                }
              }

              v = src_root;
            }

            rs.src_path_ = &v.as<const path&> ();
          }

          // At this stage we should have both roots and out_base figured
          // out. If src_base is still undetermined, calculate it.
          //
          if (src_base.empty ())
            src_base = src_root / out_base.leaf (out_root);

          // Now that we have src_root, load the src_root bootstrap file,
          // if there is one.
          //
          bool bootstrapped (bootstrap_src (rs));

          // The src bootstrap should have loaded all the modules that
          // may add new meta/operations. So at this stage they should
          // all be known. We store the combined action id in uint8_t;
          // see <operation> for details.
          //
          assert (rs.operations.size () <= 128);
          assert (rs.meta_operations.size () <= 128);

          // Since we now know all the names of meta-operations and
          // operations, "lift" names that we assumed (from buildspec
          // syntax) were operations but are actually meta-operations.
          // Also convert empty names (which means they weren't explicitly
          // specified) to the defaults and verify that all the names are
          // known.
          //
          {
            const auto& mn (ms.name);
            const auto& on (os.name);

            meta_operation_id m (0);
            operation_id o (0);

            if (!on.empty ())
            {
              m = rs.meta_operations.find (on);

              if (m != 0)
              {
                if (!mn.empty ())
                  fail (l) << "nested meta-operation " << mn
                           << '(' << on << ')';

                if (!lifted) // If this is the first target.
                {
                  // End the previous meta-operation batch if there was one
                  // and start a new one.
                  //
                  if (mid != 0)
                  {
                    assert (oid == 0);

                    if (mif->meta_operation_post != nullptr)
                      mif->meta_operation_post ();

                    level4 ([&]{trace << "end meta-operation batch "
                                      << mif->name << ", id "
                                      << static_cast<uint16_t> (mid);});

                    mid = 0;
                  }

                  lifted = true; // Flag to also end it; see above.
                }
              }
              else
              {
                o = rs.operations.find (on);

                if (o == 0)
                {
                  diag_record dr;
                  dr << fail (l) << "unknown operation " << on;

                  // If we guessed src_root and didn't load anything during
                  // bootstrap, then this is probably a meta-operation that
                  // would have been added by the module if src_root was
                  // correct.
                  //
                  if (guessing && !bootstrapped)
                    dr << info << "consider explicitly specifying src_base "
                       << "for " << tn;
                }
              }
            }

            if (!mn.empty ())
            {
              m = rs.meta_operations.find (mn);

              if (m == 0)
              {
                diag_record dr;
                dr << fail (l) << "unknown meta-operation " << mn;

                // Same idea as for the operation case above.
                //
                if (guessing && !bootstrapped)
                  dr << info << "consider explicitly specifying src_base "
                     << "for " << tn;
              }
            }

            // The default meta-operation is perform. The default
            // operation is assigned by the meta-operation below.
            //
            if (m == 0)
              m = perform_id;

            // If this is the first target in the meta-operation batch,
            // then set the batch meta-operation id.
            //
            if (mid == 0)
            {
              mid = m;
              mif = &rs.meta_operations[mid].get ();

              level4 ([&]{trace << "start meta-operation batch " << mif->name
                                << ", id " << static_cast<uint16_t> (mid);});

              if (mif->meta_operation_pre != nullptr)
                mif->meta_operation_pre ();
            }
            //
            // Otherwise, check that all the targets in a meta-operation
            // batch have the same meta-operation implementation.
            //
            else
            {
              if (mid > rs.meta_operations.size () ||     // Not a valid index.
                  mif != &rs.meta_operations[mid].get ()) // Not the same impl.
                fail (l) << "different meta-operation implementations "
                         << "in a meta-operation batch";
            }

            // If this is the first target in the operation batch, then set
            // the batch operation id.
            //
            if (oid == 0)
            {
              if (o == 0)
                o = default_id;

              oif = &rs.operations[o].get ();

              level4 ([&]{trace << "start operation batch " << oif->name
                                << ", id " << static_cast<uint16_t> (o);});

              // Allow the meta-operation to translate the operation.
              //
              if (mif->operation_pre != nullptr)
                oid = mif->operation_pre (o);
              else // Otherwise translate default to update.
                oid = (o == default_id ? update_id : o);

              if (o != oid)
              {
                oif = &rs.operations[oid].get ();
                level4 ([&]{trace << "operation translated to " << oif->name
                                  << ", id " << static_cast<uint16_t> (oid);});
              }

              act = action (mid, oid);

              current_mode = oif->mode;
              current_rules = &rules[oid];
            }
            //
            // Similar to meta-operations, check that all the targets in
            // an operation batch have the same operation implementation.
            //
            else
            {
              if (oid > rs.operations.size () ||     // Not a valid index.
                  oif != &rs.operations[oid].get ()) // Not the same impl.
                fail (l) << "different operation implementations "
                         << "in an operation batch";
            }
          }

          if (verb >= 4)
          {
            trace << "target " << tn << ':';
            trace << "  out_base: " << out_base.string ();
            trace << "  src_base: " << src_base.string ();
            trace << "  out_root: " << out_root.string ();
            trace << "  src_root: " << src_root.string ();
          }

          path bf (src_base / path ("buildfile"));

          // If we were guessing src_base, check that the buildfile
          // exists and if not, issue more detailed diagnostics.
          //
          if (guessing && !file_exists (bf))
            fail << bf << " does not exist"
                 << info << "consider explicitly specifying src_base "
                 << "for " << tn;

          // Load the buildfile.
          //
          mif->load (bf, rs, out_base, src_base, l);

          // Next resolve and match the target. We don't want to start
          // building before we know how to for all the targets in this
          // operation batch.
          //
          {
            const string* e;
            const target_type* ti (target_types.find (tn, e));

            if (ti == nullptr)
              fail (l) << "unknown target type " << tn.type;

            // If the directory is relative, assume it is relative to work
            // (must be consistent with how we derived out_base above).
            //
            path& d (tn.dir);

            if (d.relative ())
              d = work / d;

            d.normalize ();

            mif->match (act, rs, target_key {ti, &d, &tn.value, &e}, l, tgs);
          }
        }

        // Now execute the action on the list of targets.
        //
        mif->execute (act, tgs);

        if (mif->operation_post != nullptr)
          mif->operation_post (oid);

        level4 ([&]{trace << "end operation batch " << oif->name
                          << ", id " << static_cast<uint16_t> (oid);});
      }

      if (mif->meta_operation_post != nullptr)
        mif->meta_operation_post ();

      level4 ([&]{trace << "end meta-operation batch " << mif->name
                        << ", id " << static_cast<uint16_t> (mid);});
    }
  }
  catch (const failed&)
  {
    return 1; // Diagnostics has already been issued.
  }
  /*
  catch (const std::exception& e)
  {
    error << e.what ();
    return 1;
  }
  */
}
