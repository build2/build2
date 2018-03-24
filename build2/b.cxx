// file      : build2/b.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef _WIN32
#  include <signal.h> // signal()
#else
#  include <libbutl/win32-utility.hxx>
#endif

#include <stdlib.h>    // getenv() _putenv()(_WIN32)

#ifdef __GLIBCXX__
#  include <locale>
#endif

#include <sstream>
#include <cstring>     // strcmp(), strchr()
#include <typeinfo>
#include <iostream>    // cout

#include <libbutl/pager.mxx>
#include <libbutl/fdstream.mxx> // stderr_fd(), fdterm()

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/spec.hxx>
#include <build2/operation.hxx>
#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/prerequisite.hxx>
#include <build2/rule.hxx>
#include <build2/file.hxx>
#include <build2/module.hxx>
#include <build2/algorithm.hxx>
#include <build2/filesystem.hxx>
#include <build2/diagnostics.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>

#include <build2/parser.hxx>

#include <build2/b-options.hxx>

using namespace butl;
using namespace std;

#include <build2/config/init.hxx>
#include <build2/dist/init.hxx>
#include <build2/bin/init.hxx>
#include <build2/c/init.hxx>
#include <build2/cc/init.hxx>
#include <build2/cxx/init.hxx>
#include <build2/cli/init.hxx>
#include <build2/test/init.hxx>
#include <build2/install/init.hxx>
#include <build2/version/init.hxx>

namespace build2
{
  int
  main (int argc, char* argv[]);

  // Structured result printer (--structured-result mode).
  //
  class result_printer
  {
  public:
    result_printer (const action_targets& tgs): tgs_ (tgs) {}
    ~result_printer ();

  private:
    const action_targets& tgs_;
  };

  result_printer::
  ~result_printer ()
  {
    // Let's do some sanity checking even when we are not in the structred
    // output mode.
    //
    for (const action_target& at: tgs_)
    {
      switch (at.state)
      {
      case target_state::unknown:   continue; // Not a target/no result.
      case target_state::unchanged:
      case target_state::changed:
      case target_state::failed:    break;    // Valid states.
      default:                      assert (false);
      }

      if (ops.structured_result ())
      {
        cout << at.state
             << ' ' << current_mif->name
             << ' ' << current_inner_oif->name;

        if (current_outer_oif != nullptr)
          cout << '(' << current_outer_oif->name << ')';

        // There are two ways one may wish to identify the target of the
        // operation: as something specific but inherently non-portable (say,
        // a filesystem path, for example c:\tmp\foo.exe) or as something
        // regular that can be used to refer to a target in a portable way
        // (for example, c:\tmp\exe{foo}; note that the directory part is
        // still not portable). Which one should we use is a good question.
        // Let's go with the portable one for now and see how it goes (we
        // can always add a format version, e.g., --structured-result=2).

        // Set the stream extension verbosity to 0 to suppress extension
        // printing by default (this can still be overriden by the target
        // type's print function as is the case for file{}, for example).
        // And set the path verbosity to 1 to always print absolute.
        //
        stream_verbosity sv (stream_verb (cout));
        stream_verb (cout, stream_verbosity (1, 0));

        cout << ' ' << at.as_target () << endl;

        stream_verb (cout, sv);
      }
    }
  }
}

int build2::
main (int argc, char* argv[])
{
  tracer trace ("main");

  int r (0);

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

// A data race happens in the libstdc++ (as of GCC 7.2) implementation of the
// ctype<char>::narrow() function (bug #77704). The issue is easily triggered
// by the testscript runner that indirectly (via regex) uses ctype<char> facet
// of the global locale (and can potentially be triggered by other locale-
// aware code). We work around this by pre-initializing the global locale
// facet internal cache.
//
#ifdef __GLIBCXX__
  {
    const ctype<char>& ct (use_facet<ctype<char>> (locale ()));

    for (size_t i (0); i != 256; ++i)
      ct.narrow (static_cast<char> (i), '\0');
  }
#endif

  try
  {
    // On POSIX ignore SIGPIPE which is signaled to a pipe-writing process if
    // the pipe reading end is closed. Note that by default this signal
    // terminates a process. Also note that there is no way to disable this
    // behavior on a file descriptor basis or for the write() function call.
    //
    // On Windows disable displaying error reporting dialog box for the current
    // and child processes unless we run serially. This way we avoid multiple
    // dialog boxes to potentially pop up.
    //
#ifndef _WIN32
    if (signal (SIGPIPE, SIG_IGN) == SIG_ERR)
      fail << "unable to ignore broken pipe (SIGPIPE) signal: "
           << system_error (errno, generic_category ()); // Sanitize.
#endif

    // Parse the command line. We want to be able to specify options, vars,
    // and buildspecs in any order (it is really handy to just add -v at the
    // end of the command line).
    //
    strings cmd_vars;
    string args;
    try
    {
      cl::argv_scanner scan (argc, argv);

      size_t argn (0);       // Argument count.
      bool shortcut (false); // True if the shortcut syntax is used.

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
          if (ops.parse (scan))
            continue;

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

          if (const char* p = strchr (s, '=')) // Covers =, +=, and =+.
          {
            // Diagnose the empty variable name situation. Note that we don't
            // allow "partially broken down" assignments (as in foo =bar)
            // since foo= bar would be ambigous.
            //
            if (p == s || (p == s + 1 && *s == '+'))
              fail << "missing variable name in '" << s << "'";

            cmd_vars.push_back (s);
            continue;
          }

          // Handle the "broken down" variable assignments (i.e., foo = bar
          // instead of foo=bar).
          //
          if (scan.more ())
          {
            const char* a (scan.peek ());

            if (strcmp (a, "=" ) == 0 ||
                strcmp (a, "+=") == 0 ||
                strcmp (a, "=+") == 0)
            {
              string v (s);
              v += a;

              scan.next ();

              if (scan.more ())
                v += scan.next ();

              cmd_vars.push_back (move (v));
              continue;
            }
          }

          // Fall through.
        }

        // Merge all the individual buildspec arguments into a single string.
        // We wse newlines to separate arguments so that line numbers in
        // diagnostics signify argument numbers. Clever, huh?
        //
        if (argn != 0)
          args += '\n';

        args += s;

        // See if we are using the shortcut syntax.
        //
        if (argn == 0 && args.back () == ':')
        {
          args.back () = '(';
          shortcut = true;
        }

        argn++;
      }

      // Add the closing parenthesis unless there wasn't anything in between
      // in which case pop the opening one.
      //
      if (shortcut)
      {
        if (argn == 1)
          args.pop_back ();
        else
          args += ')';
      }
    }
    catch (const cl::exception& e)
    {
      fail << e;
    }

    // Validate options.
    //
    if (ops.progress () && ops.no_progress ())
      fail << "inconsistent progress display options";

    // Global initializations.
    //
    stderr_term = fdterm (stderr_fd ());
    init (argv[0],
          ops.verbose_specified ()
          ? ops.verbose ()
          : ops.V () ? 3 : ops.v () ? 2 : ops.quiet () ? 0 : 1);

    // Version.
    //
    if (ops.version ())
    {
      cout << "build2 " << BUILD2_VERSION_ID << endl
           << "libbutl " << LIBBUTL_VERSION_ID << endl
           << "host " << BUILD2_HOST_TRIPLET << endl
           << "Copyright (c) 2014-2017 Code Synthesis Ltd" << endl
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
        fail << "pager failed: " << e;
      }
    }

#ifdef _WIN32
    if (!ops.serial_stop ())
      SetErrorMode (SetErrorMode (0) | // Returns the current mode.
                    SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif

    // Register builtin modules.
    //
    {
      using mf = module_functions;
      auto& bm (builtin_modules);

      bm["config"]  = mf {&config::boot, &config::init};
      bm["dist"]    = mf {&dist::boot, &dist::init};
      bm["test"]    = mf {&test::boot, &test::init};
      bm["install"] = mf {&install::boot, &install::init};
      bm["version"] = mf {&version::boot, &version::init};

      bm["bin.vars"] = mf {nullptr, &bin::vars_init};
      bm["bin.config"] = mf {nullptr, &bin::config_init};
      bm["bin"] = mf {nullptr, &bin::init};
      bm["bin.ar.config"] = mf {nullptr, &bin::ar_config_init};
      bm["bin.ar"] = mf {nullptr, &bin::ar_init};
      bm["bin.ld.config"] = mf {nullptr, &bin::ld_config_init};
      bm["bin.ld"] = mf {nullptr, &bin::ld_init};
      bm["bin.rc.config"] = mf {nullptr, &bin::rc_config_init};
      bm["bin.rc"] = mf {nullptr, &bin::rc_init};

      bm["cc.core.vars"] = mf {nullptr, &cc::core_vars_init};
      bm["cc.core.guess"] = mf {nullptr, &cc::core_guess_init};
      bm["cc.core.config"] = mf {nullptr, &cc::core_config_init};
      bm["cc.core"] = mf {nullptr, &cc::core_init};
      bm["cc.config"] = mf {nullptr, &cc::config_init};
      bm["cc"] = mf {nullptr, &cc::init};

      bm["c.guess"] = mf {nullptr, &c::guess_init};
      bm["c.config"] = mf {nullptr, &c::config_init};
      bm["c"] = mf {nullptr, &c::init};

      bm["cxx.guess"] = mf {nullptr, &cxx::guess_init};
      bm["cxx.config"] = mf {nullptr, &cxx::config_init};
      bm["cxx"] = mf {nullptr, &cxx::init};

      bm["cli.config"] = mf {nullptr, &cli::config_init};
      bm["cli"] = mf {nullptr, &cli::init};
    }

    keep_going = !ops.serial_stop ();

    // Start up the scheduler and allocate lock shards.
    //
    size_t jobs (0);

    if (ops.jobs_specified ())
      jobs = ops.jobs ();
    else if (ops.serial_stop ())
      jobs = 1;

    if (jobs == 0)
      jobs = scheduler::hardware_concurrency ();

    if (jobs == 0)
    {
      warn << "unable to determine the number of hardware threads" <<
        info << "falling back to serial execution" <<
        info << "use --jobs|-j to override";

      jobs = 1;
    }

    size_t max_jobs (0);

    if (ops.max_jobs_specified ())
    {
      max_jobs = ops.max_jobs ();

      if (max_jobs != 0 && max_jobs < jobs)
        fail << "invalid --max-jobs|-J value";
    }

    sched.startup (jobs,
                   1,
                   max_jobs,
                   jobs * ops.queue_depth (),
                   (ops.max_stack_specified ()
                    ? optional<size_t> (ops.max_stack () * 1024)
                    : nullopt));

    variable_cache_mutex_shard_size = sched.shard_size ();
    variable_cache_mutex_shard.reset (
      new shared_mutex[variable_cache_mutex_shard_size]);

    // Trace some overall environment information.
    //
    if (verb >= 5)
    {
      const char* p (getenv ("PATH"));

      trace << "work: " << work;
      trace << "home: " << home;
      trace << "path: " << (p != nullptr ? p : "<NULL>");
      trace << "jobs: " << jobs;
    }

    // Set the build state before parsing the buildspec since it relies on
    // global scope being setup.
    //
    variable_overrides var_ovs (reset (cmd_vars));

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

    // Check for a buildfile in the specified directory returning empty path
    // if it does not exist.
    //
    auto find_buildfile = [] (const dir_path& d)
    {
      const path& n (ops.buildfile ());

      if (n.string () != "-")
      {
        path f (d / n);
        return exists (f) ? f : path ();
      }
      else
        return n;
    };

    // If not NULL, then lifted points to the operation that has been "lifted"
    // to the meta-operaion (see the logic below for details). Skip is the
    // position of the next operation.
    //
    opspec* lifted (nullptr);
    size_t skip (0);

    // The dirty flag indicated whether we managed to execute anything before
    // lifting an operation.
    //
    bool dirty (false); // Already (re)set for the first run.

    for (auto mit (bspec.begin ()); mit != bspec.end (); )
    {
      vector_view<opspec> opspecs;

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

      const path p ("<buildspec>");
      const location l (&p, 0, 0); //@@ TODO

      meta_operation_id mid (0); // Not yet translated.
      const meta_operation_info* mif (nullptr);

      // See if this meta-operation wants to pre-process the opspecs. Note
      // that this functionality can only be used for build-in meta-operations
      // that were explicitly specified on the command line (so cannot be used
      // for perform) and that will be lifted early (see below).
      //
      values& mparams (lifted == nullptr ? mit->params : lifted->params);
      string  mname   (lifted == nullptr ? mit->name   : lifted->name);

      current_mname = mname; // Set early.

      if (!mname.empty ())
      {
        if (meta_operation_id m = meta_operation_table.find (mname))
        {
          // Can modify params, opspec, change meta-operation name.
          //
          if (auto f = meta_operation_table[m].process)
            mname = current_mname =
              f (var_ovs, mparams, opspecs, lifted != nullptr, l);
        }
      }

      for (auto oit (opspecs.begin ()); oit != opspecs.end (); ++oit)
      {
        opspec& os (*oit);

        // A lifted meta-operation will always have default operation.
        //
        const values& oparams (lifted == nullptr ? os.params : values ());
        const string& oname   (lifted == nullptr ? os.name   : empty_string);

        current_oname = oname; // Set early.

        if (lifted != nullptr)
          lifted = nullptr; // Clear for the next iteration.

        if (os.empty ()) // Default target: dir{}.
          os.push_back (targetspec (name ("dir", string ())));

        operation_id oid (0), orig_oid (0);
        const operation_info* oif (nullptr);
        const operation_info* outer_oif (nullptr);

        operation_id pre_oid (0), orig_pre_oid (0);
        const operation_info* pre_oif (nullptr);

        operation_id post_oid (0), orig_post_oid (0);
        const operation_info* post_oif (nullptr);

        // Return true if this operation is lifted.
        //
        auto lift = [&oname, &mname, &os, &mit, &lifted, &skip, &l, &trace] ()
        {
          meta_operation_id m (meta_operation_table.find (oname));

          if (m != 0)
          {
            if (!mname.empty ())
              fail (l) << "nested meta-operation " << mname << '('
                       << oname << ')';

            l5 ([&]{trace << "lifting operation " << oname
                          << ", id " << uint16_t (m);});

            lifted = &os;
            skip = lifted - mit->data () + 1;
          }

          return m != 0;
        };

        // We do meta-operation and operation batches sequentially (no
        // parallelism). But multiple targets in an operation batch can be
        // done in parallel.

        // First see if we can lift this operation early by checking if it
        // is one of the built-in meta-operations. This is important to make
        // sure we pre-process the opspec before loading anything.
        //
        if (!oname.empty () && lift ())
          break;

        // Next bootstrap projects for all the target so that all the variable
        // overrides are set (if we also load/search/match in the same loop
        // then we may end up loading a project (via import) before this
        // happends.
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

            // Handle a few common cases as special: empty name, '.', '..', as
            // well as dir{foo/bar} (without trailing '/'). This code must be
            // consistent with find_target_type() and other places.
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
          // src_base was specified explicitly.
          //
          dir_path src_root;
          dir_path out_root;

          dir_path& src_base (ts.src_base); // Update it in buildspec.

          if (!src_base.empty ())
          {
            // Make sure it exists. While we will fail further down if it
            // doesn't, the diagnostics could be confusing (e.g., unknown
            // operation because we didn't load bootstrap.build).
            //
            if (!exists (src_base))
              fail << "src_base directory " << src_base << " does not exist";

            if (src_base.relative ())
              src_base = work / src_base;

            // Also came from the command line, so actualize.
            //
            src_base.normalize (true);

            // Make sure out_base is not a subdirectory of src_base. Who would
            // want to do that, you may ask. Well, you would be surprised...
            //
            if (out_base != src_base && out_base.sub (src_base))
              fail << "out_base directory is inside src_base" <<
                info << "src_base: " << src_base <<
                info << "out_base: " << out_base;

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
            {
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
          }
          else
          {
            // If no src_base was explicitly specified, search for out_root.
            //
            auto p (find_out_root (out_base));
            out_root = move (p.first);

            // If not found (i.e., we have no idea where the roots are), then
            // this can only mean a simple project. Which in turn means there
            // should be a buildfile in out_base.
            //
            if (out_root.empty ())
            {
              if (find_buildfile (out_base).empty ())
              {
                fail << "no buildfile in " << out_base <<
                  info << "consider explicitly specifying its src_base";
              }

              src_root = src_base = out_root = out_base;
            }
            else if (p.second)
              src_root = out_root;
          }

          // Now we know out_root and, if it was explicitly specified or the
          // same as out_root, src_root. The next step is to create the root
          // scope and load the out_root bootstrap files, if any. Note that we
          // might already have done this as a result of one of the preceding
          // target processing.
          //
          // If we know src_root, set that variable as well. This could be of
          // use to the bootstrap files (other than src-root.build, which,
          // BTW, doesn't need to exist if src_root == out_root).
          //
          scope& rs (
            create_root (*scope::global_, out_root, src_root)->second);

          bool bootstrapped (build2::bootstrapped (rs));

          if (!bootstrapped)
          {
            bootstrap_out (rs);

            // See if the bootstrap process set/changed src_root.
            //
            value& v (rs.assign (var_src_root));

            if (v)
            {
              // If we also have src_root specified by the user, make sure
              // they match.
              //
              const dir_path& p (cast<dir_path> (v));

              if (src_root.empty ())
                src_root = p;
              else if (src_root != p)
              {
                fail << "bootstrapped src_root " << p << " does not match "
                     << "specified " << src_root;
              }
            }
            else
            {
              // Neither bootstrap nor the user produced src_root.
              //
              if (src_root.empty ())
              {
                fail << "no bootstrapped src_root for " << out_root <<
                  info << "consider reconfiguring this out_root";
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
              fail << src_base << " does not exist" <<
                info << "consider explicitly specifying src_base for "
                   << out_base;
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
          if (auto l = rs.vars[var_subprojects])
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
          // operations, "lift" names that we assumed (from buildspec syntax)
          // were operations but are actually meta-operations. Also convert
          // empty names (which means they weren't explicitly specified) to
          // the defaults and verify that all the names are known.
          //
          {
            if (!oname.empty () && lift ())
              break; // Out of targetspec loop.

            meta_operation_id m (0);
            operation_id o (0);

            if (!mname.empty ())
            {
              m = meta_operation_table.find (mname);

              if (m == 0)
                fail (l) << "unknown meta-operation " << mname;
            }

            if (!oname.empty ())
            {
              o = operation_table.find (oname);

              if (o == 0)
                fail (l) << "unknown operation " << oname;
            }

            // The default meta-operation is perform. The default operation is
            // assigned by the meta-operation below.
            //
            if (m == 0)
              m = perform_id;

            // If this is the first target in the meta-operation batch, then
            // set the batch meta-operation id.
            //
            if (mid == 0)
            {
              mid = m;
              mif = rs.meta_operations[m];

              if (mif == nullptr)
                fail (l) << "target " << tn << " does not support meta-"
                         << "operation " << meta_operation_table[m].name;

              l5 ([&]{trace << "start meta-operation batch " << mif->name
                            << ", id " << static_cast<uint16_t> (mid);});

              if (mif->meta_operation_pre != nullptr)
                mif->meta_operation_pre (mparams, l);
              else if (!mparams.empty ())
                fail (l) << "unexpected parameters for meta-operation "
                         << mif->name;

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
                         << "operation " << meta_operation_table[mid].name;

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

              // Save the original oid before de-aliasing.
              //
              orig_oid = o;
              oif = lookup (o);

              l5 ([&]{trace << "start operation batch " << oif->name
                            << ", id " << static_cast<uint16_t> (oif->id);});

              // Allow the meta-operation to translate the operation.
              //
              if (mif->operation_pre != nullptr)
                oid = mif->operation_pre (mparams, oif->id);
              else // Otherwise translate default to update.
                oid = (oif->id == default_id ? update_id : oif->id);

              if (oif->id != oid)
              {
                // Update the original id (we assume in the check below that
                // translation would have produced the same result since we've
                // verified the meta-operation implementation is the same).
                //
                orig_oid = oid;
                oif = lookup (oid);
                oid = oif->id; // De-alias.

                l5 ([&]{trace << "operation translated to " << oif->name
                              << ", id " << static_cast<uint16_t> (oid);});
              }

              if (oif->outer_id != 0)
                outer_oif = lookup (oif->outer_id);

              // Handle pre/post operations.
              //
              if (oif->pre != nullptr)
              {
                if ((orig_pre_oid = oif->pre (oparams, mid, l)) != 0)
                {
                  assert (orig_pre_oid != default_id);
                  pre_oif = lookup (orig_pre_oid);
                  pre_oid = pre_oif->id; // De-alias.
                }
              }
              else if (!oparams.empty ())
                fail (l) << "unexpected parameters for operation "
                         << oif->name;

              if (oif->post != nullptr)
              {
                if ((orig_post_oid = oif->post (oparams, mid)) != 0)
                {
                  assert (orig_post_oid != default_id);
                  post_oif = lookup (orig_post_oid);
                  post_oid = post_oif->id;
                }
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

              check (orig_oid, oif);

              if (oif->outer_id != 0)
                check (oif->outer_id, outer_oif);

              if (pre_oid != 0)
                check (orig_pre_oid, pre_oif);

              if (post_oid != 0)
                check (orig_post_oid, post_oif);
            }
          }

          if (verb >= 5)
          {
            trace << "bootstrapped " << tn << ':';
            trace << "  out_base:  " << out_base;
            trace << "  src_base:  " << src_base;
            trace << "  out_root:  " << out_root;
            trace << "  src_root:  " << src_root;

            if (auto l = rs.vars[var_amalgamation])
              trace << "  amalgamat: " << cast<dir_path> (l);
          }

          path bf (find_buildfile (src_base));
          if (bf.empty ())
          {
            // If the target is a directory and src_base exists, then assume
            // implied buildfile; see dir::search_implied().
            //
            if (!((tn.directory () || tn.type == "dir") && exists (src_base)))
              fail << "no buildfile in " << src_base <<
                info << "consider explicitly specifying src_base for "
                   << out_base;
          }

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
        } // target

        // If this operation has been lifted, break out.
        //
        if (lifted == &os)
        {
          assert (oid == 0); // Should happend on the first target.
          break;
        }

        // Now load the buildfiles and search the targets.
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
          mif->load (mparams, rs, ts.buildfile, ts.out_base, ts.src_base, l);

          // Next search and match the targets. We don't want to start
          // building before we know how to for all the targets in this
          // operation batch.
          //
          const scope& bs (scopes.find (ts.out_base));

          // Find the target type and extract the extension.
          //
          optional<string> e;
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

            d.normalize (true); // Actualize since came from command line.

            // Figure out if this target is in the src tree.
            //
            dir_path out (ts.out_base != ts.src_base && d.sub (ts.src_base)
                          ? out_src (d, rs)
                          : dir_path ());

            mif->search (mparams,
                         rs, bs,
                         target_key {ti, &d, &out, &tn.value, e},
                         l,
                         tgs);
          }
        } // target

        // Finally, match the rules and perform the operation.
        //
        if (pre_oid != 0)
        {
          l5 ([&]{trace << "start pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});

          if (mif->operation_pre != nullptr)
            mif->operation_pre (mparams, pre_oid); // Cannot be translated.

          set_current_oif (*pre_oif, oif);

          action a (mid, pre_oid, oid);

          {
            result_printer p (tgs);
            uint16_t diag (ops.structured_result () ? 0 : 1);

            if (mif->match != nullptr)
              mif->match (mparams, a, tgs, diag, true /* progress */);

            if (mif->execute != nullptr && !ops.match_only ())
              mif->execute (mparams, a, tgs, diag, true /* progress */);
          }

          if (mif->operation_post != nullptr)
            mif->operation_post (mparams, pre_oid);

          l5 ([&]{trace << "end pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});

          tgs.reset ();
        }

        set_current_oif (*oif, outer_oif);

        action a (mid, oid, oif->outer_id);

        {
          result_printer p (tgs);
          uint16_t diag (ops.structured_result () ? 0 : 2);

          if (mif->match != nullptr)
            mif->match (mparams, a, tgs, diag, true /* progress */);

          if (mif->execute != nullptr && !ops.match_only ())
            mif->execute (mparams, a, tgs, diag, true /* progress */);
        }

        if (post_oid != 0)
        {
          tgs.reset ();

          l5 ([&]{trace << "start post-operation batch " << post_oif->name
                        << ", id " << static_cast<uint16_t> (post_oid);});

          if (mif->operation_pre != nullptr)
            mif->operation_pre (mparams, post_oid); // Cannot be translated.

          set_current_oif (*post_oif, oif);

          action a (mid, post_oid, oid);

          {
            result_printer p (tgs);
            uint16_t diag (ops.structured_result () ? 0 : 1);

            if (mif->match != nullptr)
              mif->match (mparams, a, tgs, diag, true /* progress */);

            if (mif->execute != nullptr && !ops.match_only ())
              mif->execute (mparams, a, tgs, diag, true /* progress */);
          }

          if (mif->operation_post != nullptr)
            mif->operation_post (mparams, post_oid);

          l5 ([&]{trace << "end post-operation batch " << post_oif->name
                        << ", id " << static_cast<uint16_t> (post_oid);});
        }

        if (mif->operation_post != nullptr)
          mif->operation_post (mparams, oid);

        l5 ([&]{trace << "end operation batch " << oif->name
                      << ", id " << static_cast<uint16_t> (oid);});
      } // operation

      if (mid != 0)
      {
        if (mif->meta_operation_post != nullptr)
          mif->meta_operation_post (mparams);

        l5 ([&]{trace << "end meta-operation batch " << mif->name
                      << ", id " << static_cast<uint16_t> (mid);});
      }

      if (lifted == nullptr && skip == 0)
        ++mit;
    } // meta-operation
  }
  catch (const failed&)
  {
    // Diagnostics has already been issued.
    //
    r = 1;
  }

  // Shutdown the scheduler and print statistics.
  //
  scheduler::stat st (sched.shutdown ());

  // In our world we wait for all the tasks to complete, even in case of a
  // failure (see, for example, wait_guard).
  //
  assert (st.task_queue_remain == 0);

  if (verb >= (st.thread_max_active > 1 ? 3 : 4))
  {
    info << "scheduler statistics:" << "\n\n"
         << "  thread_max_active      " << st.thread_max_active     << '\n'
         << "  thread_max_total       " << st.thread_max_total      << '\n'
         << "  thread_helpers         " << st.thread_helpers        << '\n'
         << "  thread_max_waiting     " << st.thread_max_waiting    << '\n'
         << '\n'
         << "  task_queue_depth       " << st.task_queue_depth      << '\n'
         << "  task_queue_full        " << st.task_queue_full       << '\n'
         << '\n'
         << "  wait_queue_slots       " << st.wait_queue_slots      << '\n'
         << "  wait_queue_collisions  " << st.wait_queue_collisions << '\n';
  }

  return r;
}

int
main (int argc, char* argv[])
{
  return build2::main (argc, argv);
}
