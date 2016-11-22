// file      : build2/b.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <string.h>    // strerror()
#include <stdlib.h>    // getenv() _putenv()(_WIN32)

#include <sstream>
#include <cstring>     // strcmp(), strchr()
#include <typeinfo>
#include <iostream>    // cout

#include <butl/pager>

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
#include <build2/filesystem>
#include <build2/diagnostics>
#include <build2/context>
#include <build2/variable>

#include <build2/parser>

#include <build2/b-options>

using namespace butl;
using namespace std;

#include <build2/config/init>
#include <build2/dist/init>
#include <build2/bin/init>
#include <build2/c/init>
#include <build2/cc/init>
#include <build2/cxx/init>
#include <build2/cli/init>
#include <build2/test/init>
#include <build2/install/init>
#include <build2/pkgconfig/init>

using namespace build2;

int
main (int argc, char* argv[])
{
  // This is a little hack to make out baseutils for Windows work when called
  // with absolute path. In a nutshell, MSYS2's exec*p() doesn't search in the
  // parent's executable directory, only in PATH. And since we are running
  // without a shell (that would read /etc/profile which sets PATH to some
  // sensible values), we are only getting Win32 PATH values. And MSYS2 /bin
  // is not one of them. So what we are going to do is add /bin at the end of
  // PATH (which will be passed as is by the MSYS2 machinery). This will make
  // MSYS2 search in /bin (where our baseutils live). And for everyone else
  // this should be harmless since it is not a valid Win32 path.
  //
#ifdef _WIN32
  {
    string mp ("PATH=");
    if (const char* p = getenv ("PATH"))
    {
      mp += p;
      mp += ';';
    }
    mp += "/bin";

    _putenv (mp.c_str ());
  }
#endif

  try
  {
    tracer trace ("main");

    // Parse the command line. We want to be able to specify options, vars,
    // and buildspecs in any order (it is really handy to just add -v at the
    // end of the command line).
    //
    strings cmd_vars;
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
            cmd_vars.push_back (s);
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

    // Global initializations.
    //
    init (argv[0],
          ops.verbose_specified ()
          ? ops.verbose ()
          : ops.V () ? 3 : ops.v () ? 2 : ops.q () ? 0 : 1);

    // Version.
    //
    if (ops.version ())
    {
      cout << "build2 " << BUILD2_VERSION_STR<< endl
           << "libbutl " << LIBBUTL_VERSION_STR << endl
           << "host " << BUILD2_HOST_TRIPLET << endl
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
      // Catch io_error as std::system_error together with the pager-specific
      // exceptions.
      //
      catch (const system_error& e)
      {
        error << "pager failed: " << e.what ();
        return 1;
      }
    }

    // Register builtin modules.
    //
    {
      using mf = module_functions;
      auto& bm (builtin_modules);

      bm["config"]  = mf {&config::boot, &config::init};
      bm["dist"]    = mf {&dist::boot, &dist::init};
      bm["test"]    = mf {&test::boot, &test::init};
      bm["install"] = mf {&install::boot, &install::init};

      bm["bin.config"] = mf {nullptr, &bin::config_init};
      bm["bin"] = mf {nullptr, &bin::init};
      bm["bin.ar.config"] = mf {nullptr, &bin::ar_config_init};
      bm["bin.ar"] = mf {nullptr, &bin::ar_init};
      bm["bin.ld.config"] = mf {nullptr, &bin::ld_config_init};
      bm["bin.ld"] = mf {nullptr, &bin::ld_init};
      bm["bin.rc.config"] = mf {nullptr, &bin::rc_config_init};
      bm["bin.rc"] = mf {nullptr, &bin::rc_init};

      bm["pkgconfig.config"] = mf {nullptr, &pkgconfig::config_init};
      bm["pkgconfig"] = mf {nullptr, &pkgconfig::init};

      bm["cc.core.vars"] = mf {nullptr, &cc::core_vars_init};
      bm["cc.core.config"] = mf {nullptr, &cc::core_config_init};
      bm["cc.core"] = mf {nullptr, &cc::core_init};
      bm["cc.config"] = mf {nullptr, &cc::config_init};
      bm["cc"] = mf {nullptr, &cc::init};

      bm["c.config"] = mf {nullptr, &c::config_init};
      bm["c"] = mf {nullptr, &c::init};

      bm["cxx.config"] = mf {nullptr, &cxx::config_init};
      bm["cxx"] = mf {nullptr, &cxx::init};

      bm["cli.config"] = mf {nullptr, &cli::config_init};
      bm["cli"] = mf {nullptr, &cli::init};
    }

    if (verb >= 5)
    {
      const char* p (getenv ("PATH"));

      trace << "work: " << work;
      trace << "home: " << home;
      trace << "path: " << (p != nullptr ? p : "<NULL>");
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
    catch (const io_error&)
    {
      fail << "unable to parse buildspec '" << args << "'";
    }

    l5 ([&]{trace << "buildspec: " << bspec;});

    if (bspec.empty ())
      bspec.push_back (metaopspec ()); // Default meta-operation.

    // If not NULL, then lifted points to the operation that has been "lifted"
    // to the meta-operaion (see the logic below for details). Skip is the
    // position of the next operation. Dirty indicated whether we managed to
    // execute anything before lifting an operation.
    //
    opspec* lifted (nullptr);
    size_t skip (0);
    bool dirty (true);
    variable_overrides var_ovs;

    for (auto mit (bspec.begin ()); mit != bspec.end (); )
    {
      vector_view<opspec> opspecs;
      const string& mname (lifted == nullptr ? mit->name : lifted->name);
      current_mname = &mname;

      if (lifted == nullptr)
      {
        metaopspec& ms (*mit);

        if (ms.empty ())
          ms.push_back (opspec ()); // Default operation.

        // Continue where we left off after lifting an operation.
        //
        opspecs.assign (ms.data () + skip, ms.size () - skip);

        // Reset since unless we lift another operation, we move to the
        // next meta-operation (see bottom of the loop).
        //
        skip = 0;

        // This can happen if we have lifted the last operation in opspecs.
        //
        if (opspecs.empty ())
        {
          ++mit;
          continue;
        }
      }
      else
        opspecs.assign (lifted, 1);

      // Reset the build state for each meta-operation since there is no
      // guarantee their assumptions (e.g., in the load callback) are
      // compatible.
      //
      if (dirty)
      {
        var_ovs = reset (cmd_vars);
        dirty = false;
      }

      meta_operation_id mid (0); // Not yet translated.
      const meta_operation_info* mif (nullptr);

      for (auto oit (opspecs.begin ()); oit != opspecs.end (); ++oit)
      {
        opspec& os (*oit);

        // A lifted meta-operation will always have default operation.
        //
        const string& oname (lifted == nullptr ? os.name : string ());
        current_oname = &oname;

        if (lifted != nullptr)
          lifted = nullptr; // Clear for the next iteration.

        if (os.empty ()) // Default target: dir{}.
          os.push_back (targetspec (name ("dir", string ())));

        const path p ("<buildspec>");
        const location l (&p, 0, 0); //@@ TODO

        operation_id oid (0); // Not yet translated.
        const operation_info* oif (nullptr);

        operation_id pre_oid (0);
        const operation_info* pre_oif (nullptr);

        operation_id post_oid (0);
        const operation_info* post_oif (nullptr);

        // We do meta-operation and operation batches sequentially (no
        // parallelism). But multiple targets in an operation batch can be
        // done in parallel.

        // First bootstrap projects for all the target so that all the
        // variable overrides are set (if we also load/search/match in the
        // same loop then we may end up loading a project (via import) before
        // this happends.
        //
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

          // This directory came from the command line so actualize it.
          //
          out_base.normalize (true);

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
            if (!exists (src_base))
              fail << "src_base directory " << src_base << " does not exist";

            if (src_base.relative ())
              src_base = work / src_base;

            // Also came from the command line, so actualize.
            //
            src_base.normalize (true);

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
              src_root = find_src_root (work); // Work is actualized.

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
                  src_base = work; // Work is actualized.
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
          else if (src_root.empty ())
            src_root = rs.src_path ();

          // At this stage we should have both roots and out_base figured
          // out. If src_base is still undetermined, calculate it.
          //
          if (src_base.empty ())
          {
            src_base = src_root / out_base.leaf (out_root);

            if (!exists (src_base))
            {
              diag_record dr;
              dr << fail << "src_base directory " << src_base
                 << " does not exist";

              if (guessing)
                dr << info << "consider explicitly specifying src_base "
                   << "for " << tn;
            }
          }

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
            for (const auto& p: cast<subprojects> (l))
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
            meta_operation_id m (0);
            operation_id o (0);

            if (!oname.empty ())
            {
              m = meta_operation_table.find (oname);

              if (m != 0)
              {
                if (!mname.empty ())
                  fail (l) << "nested meta-operation " << mname
                           << '(' << oname << ')';

                l5 ([&]{trace << "lifting operation " << oname
                              << ", id " << uint16_t (m);});

                lifted = &os;
                skip = lifted - mit->data () + 1;
                break; // Out of targetspec loop.
              }
              else
              {
                o = operation_table.find (oname);

                if (o == 0)
                {
                  diag_record dr;
                  dr << fail (l) << "unknown operation " << oname;

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

            if (!mname.empty ())
            {
              m = meta_operation_table.find (mname);

              if (m == 0)
              {
                diag_record dr;
                dr << fail (l) << "unknown meta-operation " << mname;

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

              set_current_mif (*mif);
              dirty = true;
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
            trace << "bootstrapped " << tn << ':';
            trace << "  out_base:  " << out_base;
            trace << "  src_base:  " << src_base;
            trace << "  out_root:  " << out_root;
            trace << "  src_root:  " << src_root;

            if (auto l = rs.vars["amalgamation"])
              trace << "  amalgamat: " << cast<dir_path> (l);
          }

          const path& bfn (ops.buildfile ());
          path bf (bfn.string () != "-" ? src_base / bfn : bfn);

          // If we were guessing src_base, check that the buildfile
          // exists and if not, issue more detailed diagnostics.
          //
          if (guessing && bf.string () != "-" && !exists (bf))
            fail << bf << " does not exist"
                 << info << "consider explicitly specifying src_base "
                 << "for " << tn;

          // Enter project-wide (as opposed to global) variable overrides.
          //
          // The mildly tricky part here is to distinguish the situation where
          // we are bootstrapping the same project multiple times (which is
          // ok) vs overriding the same variable multiple times (which is not
          // ok). The first override that we set cannot possibly end up in the
          // second sitution so if it is already set, then it can only be the
          // first case.
          //
          // This is further complicated by the project vs amalgamation logic
          // (we may have already done the amalgamation but not the project).
          //
          bool first_a (true);
          for (const variable_override& o: var_ovs)
          {
            if (o.ovr.visibility != variable_visibility::normal)
              continue;

            auto p (rs.weak_scope ()->vars.insert (o.ovr));

            if (!p.second)
            {
              if (first_a)
                break;

              fail << "multiple amalgamation overrides of variable "
                   << o.var.name;
            }

            value& v (p.first);
            v = o.val;
            first_a = false;
          }

          bool first_p (true);
          for (const variable_override& o: var_ovs)
          {
            // Ours is either project (%foo) or scope (/foo).
            //
            if (o.ovr.visibility == variable_visibility::normal)
              continue;

            auto p (rs.vars.insert (o.ovr));

            if (!p.second)
            {
              if (first_p)
                break;

              fail << "multiple project overrides of variable " << o.var.name;
            }

            value& v (p.first);
            v = o.val;
            first_p = false;
          }

          ts.root_scope = &rs;
          ts.out_base = move (out_base);
          ts.buildfile = move (bf);
        }

        // If this operation has been lifted, break out.
        //
        if (lifted == &os)
        {
          assert (oid == 0); // Should happend on the first target.
          break;
        }

        // Now load/search/match the targets.
        //
        action_targets tgs;
        tgs.reserve (os.size ());

        for (targetspec& ts: os)
        {
          name& tn (ts.name);
          scope& rs (*ts.root_scope);

          l5 ([&]{trace << "loading " << tn;});

          // Load the buildfile.
          //
          mif->load (ts.buildfile, rs, ts.out_base, ts.src_base, l);

          // Next search and match the targets. We don't want to start
          // building before we know how to for all the targets in this
          // operation batch.
          //
          scope& bs (scopes.find (ts.out_base));

          const string* e;
          const target_type* ti (bs.find_target_type (tn, e));

          if (ti == nullptr)
            fail (l) << "unknown target type " << tn.type;

          if (mif->search != nullptr)
          {
            // If the directory is relative, assume it is relative to work
            // (must be consistent with how we derived out_base above).
            //
            dir_path& d (tn.dir);

            if (d.relative ())
              d = work / d;

            d.normalize ();

            // Figure out if this target is in the src tree.
            //
            dir_path out (ts.out_base != ts.src_base && d.sub (ts.src_base)
                          ? out_src (d, rs)
                          : dir_path ());

            mif->search (rs, target_key {ti, &d, &out, &tn.value, e}, l, tgs);
          }
        }

        // Finally perform the operation.
        //
        if (pre_oid != 0)
        {
          l5 ([&]{trace << "start pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});

          if (mif->operation_pre != nullptr)
            mif->operation_pre (pre_oid); // Cannot be translated.

          set_current_oif (*pre_oif, oif);
          dependency_count = 0;

          action a (mid, pre_oid, oid);

          mif->match (a, tgs);
          mif->execute (a, tgs, true); // Run quiet.

          if (mif->operation_post != nullptr)
            mif->operation_post (pre_oid);

          l5 ([&]{trace << "end pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});
        }

        set_current_oif (*oif);
        dependency_count = 0;

        action a (mid, oid, 0);

        if (mif->match != nullptr)   mif->match (a, tgs);
        if (mif->execute != nullptr) mif->execute (a, tgs, verb == 0);

        if (post_oid != 0)
        {
          l5 ([&]{trace << "start post-operation batch " << post_oif->name
                        << ", id " << static_cast<uint16_t> (post_oid);});

          if (mif->operation_pre != nullptr)
            mif->operation_pre (post_oid); // Cannot be translated.

          set_current_oif (*post_oif, oif);
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

      if (mid != 0)
      {
        if (mif->meta_operation_post != nullptr)
          mif->meta_operation_post ();

        l5 ([&]{trace << "end meta-operation batch " << mif->name
                      << ", id " << static_cast<uint16_t> (mid);});
      }

      if (lifted == nullptr && skip == 0)
        ++mit;
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
