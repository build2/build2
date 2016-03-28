// file      : build2/b.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <time.h>      // tzset()
#include <string.h>    // strerror()

#include <stdlib.h>    // getenv()
#include <unistd.h>    // getuid()
#include <sys/types.h> // uid_t
#include <pwd.h>       // struct passwd, getpwuid()

#include <sstream>
#include <cstring>     // strcmp(), strchr()
#include <typeinfo>
#include <iostream>

#include <butl/pager>
#include <butl/filesystem>

#include <build2/types>
#include <build2/utility>
#include <build2/version>

#include <build2/spec>
#include <build2/operation>
#include <build2/scope>
#include <build2/target>
#include <build2/prerequisite>
#include <build2/rule>
#include <build2/file>
#include <build2/module>
#include <build2/algorithm>
#include <build2/diagnostics>
#include <build2/context>
#include <build2/variable>

#include <build2/token>
#include <build2/lexer>
#include <build2/parser>

#include <build2/b-options>

using namespace butl;
using namespace std;

#include <build2/config/module>
#include <build2/dist/module>
#include <build2/bin/module>
#include <build2/cxx/module>
#include <build2/cli/module>
#include <build2/test/module>
#include <build2/install/module>

using namespace build2;

int
main (int argc, char* argv[])
{
  try
  {
    tracer trace ("main");

    // Parse the command line. We want to be able to specify options, vars,
    // and buildspecs in any order (it is really handy to just add -v at the
    // end of the command line).
    //
    strings vars;
    string args;
    try
    {
      cl::argv_scanner scan (argc, argv);

      for (bool opt (true), var (true); scan.more (); )
      {
        if (opt)
        {
          // If we see first "--", then we are done parsing options.
          //
          if (strcmp (scan.peek (), "--") == 0)
          {
            scan.next ();
            opt = false;
            continue;
          }

          // Parse the next chunk of options until we reach an argument (or
          // eos).
          //
          ops.parse (scan);

          if (!scan.more ())
            break;

          // Fall through.
        }

        const char* s (scan.next ());

        // See if this is a command line variable. What if someone needs to
        // pass a buildspec that contains '='? One way to support this would
        // be to quote such a buildspec (e.g., "'/tmp/foo=bar/'"). Or invent
        // another separator. Or use a second "--". Actually, let's just do
        // the second "--".
        //
        if (var)
        {
          // If we see second "--", then we are also done parsing variables.
          //
          if (strcmp (s, "--") == 0)
          {
            var = false;
            continue;
          }

          if (strchr (s, '=') != nullptr) // Covers =, +=, and =+.
          {
            vars.push_back (s);
            continue;
          }

          // Fall through.
        }

        // Merge all the individual buildspec arguments into a single string.
        // Instead, we could also parse them individually (and merge the
        // result). The benefit of doing it this way is potentially better
        // diagnostics (i.e., we could have used <buildspec-1>, <buildspec-2>
        // to give the idea about which argument is invalid).
        //
        if (!args.empty ())
          args += ' ';

        args += s;
      }
    }
    catch (const cl::exception& e)
    {
      fail << e;
    }

    // Diagnostics verbosity.
    //
    verb = ops.verbose_specified ()
      ? ops.verbose ()
      : ops.v () ? 2 : ops.q () ? 0 : 1;

    // Version.
    //
    if (ops.version ())
    {
      cout << "build2 " << BUILD2_VERSION_STR<< endl
           << "libbutl " << LIBBUTL_VERSION_STR << endl
           << "Copyright (c) 2014-2016 Code Synthesis Ltd" << endl
           << "This is free software released under the MIT license." << endl;
      return 0;
    }

    // Help.
    //
    if (ops.help ())
    {
      try
      {
        pager p ("b help",
                 verb >= 2,
                 ops.pager_specified () ? &ops.pager () : nullptr,
                 &ops.pager_option ());

        print_b_usage (p.stream ());

        // If the pager failed, assume it has issued some diagnostics.
        //
        return p.wait () ? 0 : 1;
      }
      catch (const system_error& e)
      {
        error << "pager failed: " << e.what ();
        return 1;
      }
    }

    // Initialize time conversion data that is used by localtime_r().
    //
    tzset ();

    // Register builtin modules.
    //
    builtin_modules["config"]  = module_functions {&config::config_boot,
                                                   &config::config_init};
    builtin_modules["dist"]    = module_functions {&dist::dist_boot,
                                                   &dist::dist_init};
    builtin_modules["test"]    = module_functions {&test::test_boot,
                                                   &test::test_init};
    builtin_modules["install"] = module_functions {&install::install_boot,
                                                   &install::install_init};

    builtin_modules["bin"] = module_functions {nullptr, &bin::bin_init};
    builtin_modules["cxx"] = module_functions {nullptr, &cxx::cxx_init};
    builtin_modules["cli"] = module_functions {nullptr, &cli::cli_init};

    // Figure out work and home directories.
    //
    work = dir_path::current ();

    if (const char* h = getenv ("HOME"))
      home = dir_path (h);
    else
    {
      struct passwd* pw (getpwuid (getuid ()));

      if (pw == nullptr)
      {
        const char* msg (strerror (errno));
        fail << "unable to determine home directory: " << msg;
      }

      home = dir_path (pw->pw_dir);
    }

    if (verb >= 5)
    {
      trace << "work dir: " << work;
      trace << "home dir: " << home;
    }

    // Initialize the dependency state.
    //
    reset ();

    // Parse the command line variables.
    //
    for (const string& v: vars)
    {
      istringstream is (v);
      is.exceptions (istringstream::failbit | istringstream::badbit);
      lexer l (is, path ("<cmdline>"));

      // This should be a name followed by =, +=, or =+.
      //
      token t (l.next ());
      token_type tt (l.next ().type);

      if (t.type != token_type::name ||
          (tt != token_type::assign &&
           tt != token_type::prepend &&
           tt != token_type::append))
      {
        fail << "expected variable assignment instead of '" << v << "'" <<
          info << "use double '--' to treat this argument as buildspec";
      }

      parser p;
      t = p.parse_variable (l, *global_scope, t.value, tt);

      if (t.type != token_type::eos)
        fail << "unexpected " << t << " in variable assignment '" << v << "'";
    }

    // Parse the buildspec.
    //
    buildspec bspec;
    try
    {
      istringstream is (args);
      is.exceptions (istringstream::failbit | istringstream::badbit);

      parser p;
      bspec = p.parse_buildspec (is, path ("<buildspec>"));
    }
    catch (const istringstream::failure&)
    {
      fail << "unable to parse buildspec '" << args << "'";
    }

    l5 ([&]{trace << "buildspec: " << bspec;});

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
        const path p ("<buildspec>");
        const location l (&p, 1, 0); //@@ TODO

        if (os.empty ()) // Default target: dir{}.
          os.push_back (targetspec (name ("dir", string ())));

        operation_id oid (0); // Not yet translated.
        const operation_info* oif (nullptr);

        operation_id pre_oid (0);
        const operation_info* pre_oif (nullptr);

        operation_id post_oid (0);
        const operation_info* post_oif (nullptr);

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

          l5 ([&]{trace << "end meta-operation batch " << mif->name << ", id "
                        << static_cast<uint16_t> (mid);});

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
          dir_path out_base (tn.dir);
          if (out_base.empty ())
          {
            const string& v (tn.value);

            // Handle a few common cases as special: empty name, '.',
            // '..', as well as dir{foo/bar} (without trailing '/').
            // This code must be consistent with find_target_type().
            //
            if (v.empty () || v == "." || v == ".." || tn.type == "dir")
              out_base = dir_path (v);
            //
            // Otherwise, if this is a simple name, see if there is a
            // directory part in value.
            //
            else if (tn.untyped ())
            {
              // We cannot assume it is a valid filesystem name so we
              // will have to do the splitting manually.
              //
              path::size_type i (path::traits::rfind_separator (v));

              if (i != string::npos)
                out_base = dir_path (v, i != 0 ? i : 1); // Special case: "/".
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
          dir_path src_root;
          dir_path out_root;

          dir_path& src_base (ts.src_base); // Update it in buildspec.

          if (!src_base.empty ())
          {
            // Make sure it exists. While we will fail further down
            // if it doesn't, the diagnostics could be confusing (e.g.,
            // unknown operation because we don't load bootstrap.build).
            //
            if (!dir_exists (src_base))
              fail << "src_base directory " << src_base << " does not exist";

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
              try
              {
                out_root = out_base.directory (src_base.leaf (src_root));
              }
              catch (const invalid_path&)
              {
                fail << "out_base suffix does not match src_root" <<
                  info << "src_root: " << src_root <<
                  info << "out_base: " << out_base;
              }
          }
          else
          {
            // If no src_base was explicitly specified, search for out_root.
            //
            bool src;
            out_root = find_out_root (out_base, &src);

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
                         << info << "src_base is " << src_base
                         << info << "src_root is " << src_root
                         << info << "out_base is " << out_base
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
          // If we know src_root, set that variable as well. This could
          // be of use to the bootstrap files (other than src-root.build,
          // which, BTW, doesn't need to exist if src_root == out_root).
          //
          scope& rs (create_root (out_root, src_root));

          bool bootstrapped (build2::bootstrapped (rs));

          if (!bootstrapped)
          {
            bootstrap_out (rs);

            // See if the bootstrap process set/changed src_root.
            //
            value& v (rs.assign ("src_root"));

            if (v)
            {
              // If we also have src_root specified by the user, make
              // sure they match.
              //
              const dir_path& p (cast<dir_path> (v));

              if (src_root.empty ())
                src_root = p;
              else if (src_root != p)
                fail << "bootstrapped src_root " << p << " does not match "
                     << "specified " << src_root;
            }
            else
            {
              // Neither bootstrap nor the user produced src_root.
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

            setup_root (rs);

            // Now that we have src_root, load the src_root bootstrap file,
            // if there is one.
            //
            bootstrapped = bootstrap_src (rs);
          }

          // At this stage we should have both roots and out_base figured
          // out. If src_base is still undetermined, calculate it.
          //
          if (src_base.empty ())
            src_base = src_root / out_base.leaf (out_root);

          // Check that out_root that we have found is the innermost root
          // for this project. If it is not, then it means we are trying
          // to load a disfigured sub-project and that we do not support.
          // Why don't we support it? Because things are already complex
          // enough here.
          //
          // Note that the subprojects variable has already been processed
          // and converted to a map by the bootstrap_src() call above.
          //
          if (auto l = rs.vars["subprojects"])
          {
            for (const auto& p: cast<subprojects> (*l))
            {
              if (out_base.sub (out_root / p.second))
                fail << tn << " is in a subproject of " << out_root <<
                  info << "explicitly specify src_base for this target";
            }
          }

          // Create and bootstrap outer roots if any. Loading is done
          // by load_root_pre() (that would normally be called by the
          // meta-operation's load() callback below).
          //
          create_bootstrap_outer (rs);

          // The src bootstrap should have loaded all the modules that
          // may add new meta/operations. So at this stage they should
          // all be known. We store the combined action id in uint8_t;
          // see <operation> for details.
          //
          assert (operation_table.size () <= 128);
          assert (meta_operation_table.size () <= 128);

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
              m = meta_operation_table.find (on);

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

                    l5 ([&]{trace << "end meta-operation batch " << mif->name
                                  << ", id " << static_cast<uint16_t> (mid);});
                    mid = 0;
                  }

                  lifted = true; // Flag to also end it; see above.
                }
              }
              else
              {
                o = operation_table.find (on);

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
              m = meta_operation_table.find (mn);

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
              mif = rs.meta_operations[m];

              if (mif == nullptr)
                fail (l) << "target " << tn << " does not support meta-"
                         << "operation " << meta_operation_table[m];

              l5 ([&]{trace << "start meta-operation batch " << mif->name
                            << ", id " << static_cast<uint16_t> (mid);});

              if (mif->meta_operation_pre != nullptr)
                mif->meta_operation_pre ();

              current_mif = mif;
            }
            //
            // Otherwise, check that all the targets in a meta-operation
            // batch have the same meta-operation implementation.
            //
            else
            {
              const meta_operation_info* mi (rs.meta_operations[mid]);

              if (mi == nullptr)
                fail (l) << "target " << tn << " does not support meta-"
                         << "operation " << meta_operation_table[mid];

              if (mi != mif)
                fail (l) << "different implementations of meta-operation "
                         << mif->name << " in the same meta-operation batch";
            }

            // If this is the first target in the operation batch, then set
            // the batch operation id.
            //
            if (oid == 0)
            {
              auto lookup =
                [&rs, &l, &tn] (operation_id o) -> const operation_info*
                {
                  const operation_info* r (rs.operations[o]);

                  if (r == nullptr)
                    fail (l) << "target " << tn << " does not support "
                             << "operation " << operation_table[o];
                  return r;
                };

              if (o == 0)
                o = default_id;

              oif = lookup (o);

              l5 ([&]{trace << "start operation batch " << oif->name
                            << ", id " << static_cast<uint16_t> (o);});

              // Allow the meta-operation to translate the operation.
              //
              if (mif->operation_pre != nullptr)
                oid = mif->operation_pre (o);
              else // Otherwise translate default to update.
                oid = (o == default_id ? update_id : o);

              if (o != oid)
              {
                oif = lookup (oid);
                l5 ([&]{trace << "operation translated to " << oif->name
                              << ", id " << static_cast<uint16_t> (oid);});
              }

              // Handle pre/post operations.
              //
              if (oif->pre != nullptr && (pre_oid = oif->pre (mid)) != 0)
              {
                assert (pre_oid != default_id);
                pre_oif = lookup (pre_oid);
              }

              if (oif->post != nullptr && (post_oid = oif->post (mid)) != 0)
              {
                assert (post_oid != default_id);
                post_oif = lookup (post_oid);
              }
            }
            //
            // Similar to meta-operations, check that all the targets in
            // an operation batch have the same operation implementation.
            //
            else
            {
              auto check =
                [&rs, &l, &tn] (operation_id o, const operation_info* i)
                {
                  const operation_info* r (rs.operations[o]);

                  if (r == nullptr)
                    fail (l) << "target " << tn << " does not support "
                             << "operation " << operation_table[o];

                  if (r != i)
                    fail (l) << "different implementations of operation "
                             << i->name << " in the same operation batch";
                };

              check (oid, oif);

              if (pre_oid != 0)
                check (pre_oid, pre_oif);

              if (post_oid != 0)
                check (post_oid, post_oif);
            }
          }

          if (verb >= 5)
          {
            trace << "target " << tn << ':';
            trace << "  out_base: " << out_base;
            trace << "  src_base: " << src_base;
            trace << "  out_root: " << out_root;
            trace << "  src_root: " << src_root;
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

          // Next search and match the targets. We don't want to start
          // building before we know how to for all the targets in this
          // operation batch.
          //
          {
            scope& bs (scopes.find (out_base));

            const string* e;
            const target_type* ti (bs.find_target_type (tn, e));

            if (ti == nullptr)
              fail (l) << "unknown target type " << tn.type;

            // If the directory is relative, assume it is relative to work
            // (must be consistent with how we derived out_base above).
            //
            dir_path& d (tn.dir);

            if (d.relative ())
              d = work / d;

            d.normalize ();

            mif->search (rs, target_key {ti, &d, &tn.value, e}, l, tgs);
          }
        }

        if (pre_oid != 0)
        {
          l5 ([&]{trace << "start pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});

          if (mif->operation_pre != nullptr)
            mif->operation_pre (pre_oid); // Cannot be translated.

          current_inner_oif = pre_oif;
          current_outer_oif = oif;
          current_mode = pre_oif->mode;
          dependency_count = 0;

          action a (mid, pre_oid, oid);

          mif->match (a, tgs);
          mif->execute (a, tgs, true); // Run quiet.

          if (mif->operation_post != nullptr)
            mif->operation_post (pre_oid);

          l5 ([&]{trace << "end pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});
        }

        current_inner_oif = oif;
        current_outer_oif = nullptr;
        current_mode = oif->mode;
        dependency_count = 0;

        action a (mid, oid, 0);

        mif->match (a, tgs);
        mif->execute (a, tgs, verb == 0);

        if (post_oid != 0)
        {
          l5 ([&]{trace << "start post-operation batch " << post_oif->name
                        << ", id " << static_cast<uint16_t> (post_oid);});

          if (mif->operation_pre != nullptr)
            mif->operation_pre (post_oid); // Cannot be translated.

          current_inner_oif = post_oif;
          current_outer_oif = oif;
          current_mode = post_oif->mode;
          dependency_count = 0;

          action a (mid, post_oid, oid);

          mif->match (a, tgs);
          mif->execute (a, tgs, true); // Run quiet.

          if (mif->operation_post != nullptr)
            mif->operation_post (post_oid);

          l5 ([&]{trace << "end post-operation batch " << post_oif->name
                        << ", id " << static_cast<uint16_t> (post_oid);});
        }

        if (mif->operation_post != nullptr)
          mif->operation_post (oid);

        l5 ([&]{trace << "end operation batch " << oif->name
                      << ", id " << static_cast<uint16_t> (oid);});
      }

      if (mif->meta_operation_post != nullptr)
        mif->meta_operation_post ();

      l5 ([&]{trace << "end meta-operation batch " << mif->name
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
