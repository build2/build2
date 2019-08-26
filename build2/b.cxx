// file      : build2/b.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef _WIN32
#  include <signal.h> // signal()
#else
#  include <libbutl/win32-utility.hxx>
#endif

#ifdef __GLIBCXX__
#  include <locale>
#endif

#include <sstream>
#include <cstring>   // strcmp(), strchr()
#include <typeinfo>
#include <iostream>  // cout
#include <exception> // terminate(), set_terminate(), terminate_handler

#include <libbutl/pager.mxx>
#include <libbutl/fdstream.mxx>        // stderr_fd(), fdterm()
#include <libbutl/backtrace.mxx>       // backtrace()
#include <libbutl/default-options.mxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/dump.hxx>
#include <libbuild2/file.hxx>
#include <libbuild2/rule.hxx>
#include <libbuild2/spec.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/module.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/operation.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/prerequisite.hxx>

#include <libbuild2/parser.hxx>

#include <build2/b-options.hxx>

// Build system modules.
//
#include <libbuild2/dist/init.hxx>
#include <libbuild2/test/init.hxx>
#include <libbuild2/config/init.hxx>
#include <libbuild2/install/init.hxx>

#include <libbuild2/in/init.hxx>
#include <libbuild2/version/init.hxx>

#ifndef BUILD2_BOOTSTRAP
#  include <libbuild2/bash/init.hxx>

#  include <build2/cli/init.hxx>
#endif

#include <build2/bin/init.hxx>
#include <build2/c/init.hxx>
#include <build2/cc/init.hxx>
#include <build2/cxx/init.hxx>

using namespace butl;
using namespace std;

namespace build2
{
  static options ops;

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
        const target& t (at.as_target ());
        context& ctx (t.ctx);

        cout << at.state
             << ' ' << ctx.current_mif->name
             << ' ' << ctx.current_inner_oif->name;

        if (ctx.current_outer_oif != nullptr)
          cout << '(' << ctx.current_outer_oif->name << ')';

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

        cout << ' ' << t << endl;

        stream_verb (cout, sv);
      }
    }
  }
}

// Print backtrace if terminating due to an unhandled exception. Note that
// custom_terminate is non-static and not a lambda to reduce the noise.
//
static terminate_handler default_terminate;

void
custom_terminate ()
{
  *diag_stream << backtrace ();

  if (default_terminate != nullptr)
    default_terminate ();
}

static void
terminate (bool trace)
{
  if (!trace)
    set_terminate (default_terminate);

  std::terminate ();
}

int build2::
main (int argc, char* argv[])
{
  default_terminate = set_terminate (custom_terminate);

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
    string mp;
    if (optional<string> p = getenv ("PATH"))
    {
      mp = move (*p);
      mp += ';';
    }
    mp += "/bin";

    setenv ("PATH", mp);
  }
#endif

  // A data race happens in the libstdc++ (as of GCC 7.2) implementation of
  // the ctype<char>::narrow() function (bug #77704). The issue is easily
  // triggered by the testscript runner that indirectly (via regex) uses
  // ctype<char> facet of the global locale (and can potentially be triggered
  // by other locale- aware code). We work around this by pre-initializing the
  // global locale facet internal cache.
  //
#ifdef __GLIBCXX__
  {
    const ctype<char>& ct (use_facet<ctype<char>> (locale ()));

    for (size_t i (0); i != 256; ++i)
      ct.narrow (static_cast<char> (i), '\0');
  }
#endif

  // On POSIX ignore SIGPIPE which is signaled to a pipe-writing process if
  // the pipe reading end is closed. Note that by default this signal
  // terminates a process. Also note that there is no way to disable this
  // behavior on a file descriptor basis or for the write() function call.
  //
#ifndef _WIN32
  if (signal (SIGPIPE, SIG_IGN) == SIG_ERR)
    fail << "unable to ignore broken pipe (SIGPIPE) signal: "
         << system_error (errno, generic_category ()); // Sanitize.
#endif

  scheduler sched;

  // Parse the command line.
  //
  try
  {
    // Note that the diagnostics verbosity level can only be calculated after
    // default options are loaded and merged (see below). Thus, to trace the
    // default options files search, we refer to the verbosity level specified
    // on the command line.
    //
    auto verbosity = [] ()
    {
      return ops.verbose_specified ()
             ? ops.verbose ()
             : ops.V () ? 3 : ops.v () ? 2 : ops.quiet () ? 0 : 1;
    };

    // We want to be able to specify options, vars, and buildspecs in any
    // order (it is really handy to just add -v at the end of the command
    // line).
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

      // Handle default options files.
      //
      if (!ops.no_default_options ()) // Command line option.
      try
      {
        optional<dir_path> extra;
        if (ops.default_options_specified ())
          extra = ops.default_options ();

        ops = merge_default_options (
          load_default_options<options,
                               cl::argv_file_scanner,
                               cl::unknown_mode> (
            nullopt /* sys_dir */,
            path::home_directory (), // The home variable is not assigned yet.
            extra,
            default_options_files {{path ("b.options")},
                                   nullopt /* start */},
            [&trace, &verbosity] (const path& f, bool r, bool o)
            {
              if (verbosity () >= 3)
              {
                if (o)
                  trace << "treating " << f << " as "
                        << (r ? "remote" : "local");
                else
                  trace << "loading " << (r ? "remote " : "local ") << f;
              }
            }),
          ops);
      }
      catch (const pair<path, system_error>& e)
      {
        fail << "unable to load default options files: " << e.first << ": "
             << e.second;
      }
      catch (const system_error& e)
      {
        fail << "unable to obtain home directory: " << e;
      }

      // Validate options.
      //
      if (ops.progress () && ops.no_progress ())
        fail << "both --progress and --no-progress specified";

      if (ops.mtime_check () && ops.no_mtime_check ())
        fail << "both --mtime-check and --no-mtime-check specified";
    }
    catch (const cl::exception& e)
    {
      fail << e;
    }

    // Initialize the diagnostics state.
    //
    init_diag (verbosity (),
               (ops.progress ()    ? optional<bool> (true)  :
                ops.no_progress () ? optional<bool> (false) : nullopt),
               ops.no_line (),
               ops.no_column (),
               fdterm (stderr_fd ()));

    // Handle --version.
    //
    if (ops.version ())
    {
      cout << "build2 " << LIBBUILD2_VERSION_ID << endl
           << "libbutl " << LIBBUTL_VERSION_ID << endl
           << "host " << BUILD2_HOST_TRIPLET << endl
           << "Copyright (c) 2014-2019 Code Synthesis Ltd" << endl
           << "This is free software released under the MIT license." << endl;
      return 0;
    }

    // Handle --help.
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

    // Initialize time conversion data that is used by localtime_r().
    //
#ifndef _WIN32
    tzset ();
#else
    _tzset ();
#endif

    // Initialize the global state.
    //
    init (&::terminate,
          argv[0],
          (ops.mtime_check ()    ? optional<bool> (true)  :
           ops.no_mtime_check () ? optional<bool> (false) : nullopt),
          (ops.config_sub_specified ()
           ? optional<path> (ops.config_sub ())
           : nullopt),
          (ops.config_guess_specified ()
           ? optional<path> (ops.config_guess ())
           : nullopt));

#ifdef _WIN32
    // On Windows disable displaying error reporting dialog box for the
    // current and child processes unless we are in the stop mode. Failed that
    // we may have multiple dialog boxes popping up.
    //
    if (!ops.serial_stop ())
      SetErrorMode (SetErrorMode (0) | // Returns the current mode.
                    SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif

    // Load builtin modules.
    //
    {
      auto load = [] (module_load_function* lf)
      {
        for (const module_functions* i (lf ()); i->name != nullptr; ++i)
          loaded_modules[i->name] = i;
      };

      // @@ TMP
      //
#define TMP_LOAD(N, S, I)                                 \
        static const module_functions N {S, nullptr, &I};  \
        loaded_modules[S] = &N

      load (&config::build2_config_load);
      load (&dist::build2_dist_load);
      load (&test::build2_test_load);
      load (&install::build2_install_load);

      load (&version::build2_version_load);
      load (&in::build2_in_load);

      TMP_LOAD (bin_vars, "bin.vars", bin::vars_init);
      TMP_LOAD (bin_config, "bin.config", bin::config_init);
      TMP_LOAD (bin, "bin", bin::init);
      TMP_LOAD (bin_ar_config, "bin.ar.config", bin::ar_config_init);
      TMP_LOAD (bin_ar, "bin.ar", bin::ar_init);
      TMP_LOAD (bin_ld_config, "bin.ld.config", bin::ld_config_init);
      TMP_LOAD (bin_ld, "bin.ld", bin::ld_init);
      TMP_LOAD (bin_rc_config, "bin.rc.config", bin::rc_config_init);
      TMP_LOAD (bin_rc, "bin.rc", bin::rc_init);

      TMP_LOAD (cc_core_vars, "cc.core.vars", cc::core_vars_init);
      TMP_LOAD (cc_core_guess, "cc.core.guess", cc::core_guess_init);
      TMP_LOAD (cc_core_config, "cc.core.config", cc::core_config_init);
      TMP_LOAD (cc_core, "cc.core", cc::core_init);
      TMP_LOAD (cc_config, "cc.config", cc::config_init);
      TMP_LOAD (cc, "cc", cc::init);

      TMP_LOAD (c_guess, "c.guess", c::guess_init);
      TMP_LOAD (c_config, "c.config", c::config_init);
      TMP_LOAD (c, "c", c::init);

      TMP_LOAD (cxx_guess, "cxx.guess", cxx::guess_init);
      TMP_LOAD (cxx_config, "cxx.config", cxx::config_init);
      TMP_LOAD (cxx, "cxx", cxx::init);

#ifndef BUILD2_BOOTSTRAP
      TMP_LOAD (cli_config, "cli.config", cli::config_init);
      TMP_LOAD (cli, "cli", cli::init);

      load (&bash::build2_bash_load);
#endif
    }

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

    // @@ CTX: should these be per-context?
    //
    variable_cache_mutex_shard_size = sched.shard_size ();
    variable_cache_mutex_shard.reset (
      new shared_mutex[variable_cache_mutex_shard_size]);

    // Trace some overall environment information.
    //
    if (verb >= 5)
    {
      optional<string> p (getenv ("PATH"));

      trace << "work: " << work;
      trace << "home: " << home;
      trace << "path: " << (p ? *p : "<NULL>");
      trace << "jobs: " << jobs;
    }

    // Set the build context before parsing the buildspec since it relies on
    // the global scope being setup. We reset it for every meta-operation (see
    // below).
    //
    unique_ptr<context> ctx;
    auto new_context = [&ctx, &sched, &cmd_vars]
    {
      ctx = nullptr; // Free first.
      ctx.reset (new context (sched,
                              ops.dry_run (),
                              !ops.serial_stop () /* keep_going */,
                              cmd_vars));
    };

    new_context ();

    // Parse the buildspec.
    //
    buildspec bspec;
    try
    {
      istringstream is (args);
      is.exceptions (istringstream::failbit | istringstream::badbit);

      parser p (*ctx);
      bspec = p.parse_buildspec (is, path ("<buildspec>"));
    }
    catch (const io_error&)
    {
      fail << "unable to parse buildspec '" << args << "'";
    }

    l5 ([&]{trace << "buildspec: " << bspec;});

    if (bspec.empty ())
      bspec.push_back (metaopspec ()); // Default meta-operation.

    // Check for a buildfile starting from the specified directory and
    // continuing in the parent directories until root. Return empty path if
    // not found.
    //
    auto find_buildfile = [] (const dir_path& sd,
                              const dir_path& root,
                              optional<bool>& altn)
    {
      const path& n (ops.buildfile_specified () ? ops.buildfile () : path ());

      if (n.string () == "-")
        return n;

      path f;
      dir_path p;

      for (;;)
      {
        const dir_path& d (p.empty () ? sd : p.directory ());

        // Note that we don't attempt to derive the project's naming scheme
        // from the buildfile name specified by the user.
        //
        bool e;
        if (!n.empty () || altn)
        {
          f = d / (!n.empty () ? n : (*altn
                                      ? alt_buildfile_file
                                      : std_buildfile_file));
          e = exists (f);
        }
        else
        {
          // Note: this case seems to be only needed for simple projects.
          //

          // Check the alternative name first since it is more specific.
          //
          f = d / alt_buildfile_file;

          if ((e = exists (f)))
            altn = true;
          else
          {
            f = d / std_buildfile_file;

            if ((e = exists (f)))
              altn = false;
          }
        }

        if (e)
          return f;

        p = f.directory ();
        if (p == root)
          break;
      }

      return path ();
    };

    bool dump_load (false);
    bool dump_match (false);
    if (ops.dump_specified ())
    {
      dump_load  = ops.dump ().find ("load") != ops.dump ().end ();
      dump_match = ops.dump ().find ("match") != ops.dump ().end ();
    }

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

      // Reset the build context for each meta-operation since there is no
      // guarantee their assumptions (e.g., in the load callback) are
      // compatible.
      //
      if (dirty)
      {
        new_context ();
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

      ctx->current_mname = mname; // Set early.

      if (!mname.empty ())
      {
        if (meta_operation_id m = ctx->meta_operation_table.find (mname))
        {
          // Can modify params, opspec, change meta-operation name.
          //
          if (auto f = ctx->meta_operation_table[m].process)
            mname = ctx->current_mname = f (
              *ctx, mparams, opspecs, lifted != nullptr, l);
        }
      }

      // Expose early so can be used during bootstrap (with the same
      // limitations as for pre-processing).
      //
      scope& gs (ctx->global_scope.rw ());
      gs.assign (ctx->var_build_meta_operation) = mname;

      for (auto oit (opspecs.begin ()); oit != opspecs.end (); ++oit)
      {
        opspec& os (*oit);

        // A lifted meta-operation will always have default operation.
        //
        const values& oparams (lifted == nullptr ? os.params : values ());
        const string& oname   (lifted == nullptr ? os.name   : empty_string);

        ctx->current_oname = oname; // Set early.

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
        auto lift = [&ctx,
                     &oname, &mname,
                     &os, &mit, &lifted, &skip, &l, &trace] ()
        {
          meta_operation_id m (ctx->meta_operation_table.find (oname));

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

          // First figure out the out_base of this target. The logic is as
          // follows: if a directory was specified in any form, then that's
          // the out_base. Otherwise, we check if the name value has a
          // directory prefix. This has a good balance of control and the
          // expected result in most cases.
          //
          dir_path out_base (tn.dir);
          if (out_base.empty ())
          {
            const string& v (tn.value);

            // Handle a few common cases as special: empty name, '.', '..', as
            // well as dir{foo/bar} (without trailing '/'). This logic must be
            // consistent with find_target_type() and other places (grep for
            // "..").
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
              path::size_type i (path::traits_type::rfind_separator (v));

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

          // Standard/alternative build file/directory naming.
          //
          optional<bool> altn;

          // Update these in buildspec.
          //
          bool& forwarded (ts.forwarded);
          dir_path& src_base (ts.src_base);

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
            src_root = find_src_root (src_base, altn);

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
            auto p (find_out_root (out_base, altn));

            if (p.second) // Also src_root.
            {
              src_root = move (p.first);

              // Handle a forwarded configuration. Note that if we've changed
              // out_root then we also have to remap out_base.
              //
              out_root = bootstrap_fwd (*ctx, src_root, altn);
              if (src_root != out_root)
              {
                out_base = out_root / out_base.leaf (src_root);
                forwarded = true;
              }
            }
            else
            {
              out_root = move (p.first);

              // If not found (i.e., we have no idea where the roots are),
              // then this can only mean a simple project. Which in turn means
              // there should be a buildfile in out_base.
              //
              // Note that unlike the normal project case below, here we don't
              // try to look for outer buildfiles since we don't have the root
              // to stop at. However, this shouldn't be an issue since simple
              // project won't normally have targets in subdirectories (or, in
              // other words, we are not very interested in "complex simple
              // projects").
              //
              if (out_root.empty ())
              {
                if (find_buildfile (out_base, out_base, altn).empty ())
                {
                  fail << "no buildfile in " << out_base <<
                    info << "consider explicitly specifying its src_base";
                }

                src_root = src_base = out_root = out_base;
              }
            }
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
          scope& rs (create_root (gs, out_root, src_root)->second);

          bool bstrapped (bootstrapped (rs));

          if (!bstrapped)
          {
            bootstrap_out (rs, altn);

            // See if the bootstrap process set/changed src_root.
            //
            value& v (rs.assign (ctx->var_src_root));

            if (v)
            {
              // If we also have src_root specified by the user, make sure
              // they match.
              //
              dir_path& p (cast<dir_path> (v));

              if (src_root.empty ())
                src_root = p;
              else if (src_root != p)
              {
                // We used to fail here but that meant there were no way to
                // actually fix the problem (i.e., remove a forward or
                // reconfigure the out directory). So now we warn (unless
                // quiet, which is helful to tools like the package manager
                // that are running info underneath).
                //
                // We also save the old/new values since we may have to remap
                // src_root for subprojects (amalgamations are handled by not
                // loading outer project for disfigure and info).
                //
                if (verb)
                  warn << "configured src_root " << p << " does not match "
                       << (forwarded ? "forwarded " : "specified ")
                       << src_root;

                ctx->new_src_root = src_root;
                ctx->old_src_root = move (p);
                p = src_root;
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

            setup_root (rs, forwarded);

            // Now that we have src_root, load the src_root bootstrap file,
            // if there is one.
            //
            bootstrap_pre (rs, altn);
            bootstrap_src (rs, altn);
            // bootstrap_post() delayed until after create_bootstrap_outer().
          }
          else
          {
            // Note that we only "upgrade" the forwarded value since the same
            // project root can be arrived at via multiple paths (think
            // command line and import).
            //
            if (forwarded)
              rs.assign (ctx->var_forwarded) = true;

            // Sync local variable that are used below with actual values.
            //
            if (src_root.empty ())
              src_root = rs.src_path ();

            if (!altn)
              altn = rs.root_extra->altn;
            else
              assert (*altn == rs.root_extra->altn);
          }

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
          if (auto l = rs.vars[ctx->var_subprojects])
          {
            for (const auto& p: cast<subprojects> (l))
            {
              if (out_base.sub (out_root / p.second))
                fail << tn << " is in a subproject of " << out_root <<
                  info << "explicitly specify src_base for this target";
            }
          }

          // The src bootstrap should have loaded all the modules that
          // may add new meta/operations. So at this stage they should
          // all be known. We store the combined action id in uint8_t;
          // see <operation> for details.
          //
          assert (ctx->operation_table.size () <= 128);
          assert (ctx->meta_operation_table.size () <= 128);

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
              m = ctx->meta_operation_table.find (mname);

              if (m == 0)
                fail (l) << "unknown meta-operation " << mname;
            }

            if (!oname.empty ())
            {
              o = ctx->operation_table.find (oname);

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
            bool first (mid == 0);
            if (first)
            {
              mid = m;
              mif = rs.root_extra->meta_operations[m];

              if (mif == nullptr)
                fail (l) << "target " << tn << " does not support meta-"
                         << "operation " << ctx->meta_operation_table[m].name;
            }
            //
            // Otherwise, check that all the targets in a meta-operation
            // batch have the same meta-operation implementation.
            //
            else
            {
              const meta_operation_info* mi (
                rs.root_extra->meta_operations[mid]);

              if (mi == nullptr)
                fail (l) << "target " << tn << " does not support meta-"
                         << "operation " << ctx->meta_operation_table[mid].name;

              if (mi != mif)
                fail (l) << "different implementations of meta-operation "
                         << mif->name << " in the same meta-operation batch";
            }

            // Create and bootstrap outer roots if any. Loading is done by
            // load_root() (that would be called by the meta-operation's
            // load() callback below).
            //
            if (mif->bootstrap_outer)
              create_bootstrap_outer (rs);

            if (!bstrapped)
              bootstrap_post (rs);

            if (first)
            {
              l5 ([&]{trace << "start meta-operation batch " << mif->name
                            << ", id " << static_cast<uint16_t> (mid);});

              if (mif->meta_operation_pre != nullptr)
                mif->meta_operation_pre (mparams, l);
              else if (!mparams.empty ())
                fail (l) << "unexpected parameters for meta-operation "
                         << mif->name;

              ctx->current_meta_operation (*mif);
              dirty = true;
            }

            // If this is the first target in the operation batch, then set
            // the batch operation id.
            //
            if (oid == 0)
            {
              auto lookup = [&ctx, &rs, &l, &tn] (operation_id o) ->
                const operation_info*
              {
                const operation_info* r (rs.root_extra->operations[o]);

                if (r == nullptr)
                  fail (l) << "target " << tn << " does not support "
                           << "operation " << ctx->operation_table[o];
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
              auto check = [&ctx, &rs, &l, &tn] (operation_id o,
                                                 const operation_info* i)
              {
                const operation_info* r (rs.root_extra->operations[o]);

                if (r == nullptr)
                  fail (l) << "target " << tn << " does not support "
                           << "operation " << ctx->operation_table[o];

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

          // If we cannot find the buildfile in this directory, then try our
          // luck with the nearest outer buildfile, in case our target is
          // defined there (common with non-intrusive project conversions
          // where everything is built from a single root buildfile).
          //
          // The directory target case is ambigous since it can also be the
          // implied buildfile. The heuristics that we use is to check whether
          // the implied buildfile is plausible: there is a subdirectory with
          // a buildfile. Checking for plausability feels expensive since we
          // have to recursively traverse the directory tree. Note, however,
          // that if the answer is positive, then shortly after we will be
          // traversing this tree anyway and presumably this time getting the
          // data from the cash (we don't really care about the negative
          // answer since this is a degenerate case).
          //
          path bf (find_buildfile (src_base, src_base, altn));
          if (bf.empty ())
          {
            // If the target is a directory and the implied buildfile is
            // plausible, then assume that. Otherwise, search for an outer
            // buildfile.
            //
            if ((tn.directory () || tn.type == "dir") &&
                exists (src_base)                     &&
                dir::check_implied (rs, src_base))
              ; // Leave bf empty.
            else
            {
              if (src_base != src_root)
                bf = find_buildfile (src_base.directory (), src_root, altn);

              if (bf.empty ())
                fail << "no buildfile in " << src_base << " or parent "
                     << "directories" <<
                  info << "consider explicitly specifying src_base for "
                     << out_base;

              // Adjust bases to match the directory where we found the
              // buildfile since that's the scope it will be loaded in. Note:
              // but not the target since it is resolved relative to work; see
              // below.
              //
              src_base = bf.directory ();
              out_base = out_src (src_base, out_root, src_root);
            }
          }

          if (verb >= 5)
          {
            trace << "bootstrapped " << tn << ':';
            trace << "  out_base:     " << out_base;
            trace << "  src_base:     " << src_base;
            trace << "  out_root:     " << out_root;
            trace << "  src_root:     " << src_root;
            trace << "  forwarded:    " << (forwarded ? "true" : "false");
            if (auto l = rs.vars[ctx->var_amalgamation])
            {
              trace << "  amalgamation: " << cast<dir_path> (l);
              trace << "  strong scope: " << *rs.strong_scope ();
              trace << "  weak scope:   " << *rs.weak_scope ();
            }
          }

          // Enter project-wide (as opposed to global) variable overrides.
          //
          // The mildly tricky part here is to distinguish the situation where
          // we are bootstrapping the same project multiple times. The first
          // override that we set cannot already exist (because the override
          // variable names are unique) so if it is already set, then it can
          // only mean this project is already bootstrapped.
          //
          // This is further complicated by the project vs amalgamation logic
          // (we may have already done the amalgamation but not the project).
          // So we split it into two passes.
          //
          {
            auto& sm (ctx->scopes.rw ());

            for (const variable_override& o: ctx->var_overrides)
            {
              if (o.ovr.visibility != variable_visibility::normal)
                continue;

              // If we have a directory, enter the scope, similar to how we do
              // it in the context's reset().
              //
              scope& s (o.dir
                        ? sm.insert ((out_base / *o.dir).normalize ())->second
                        : *rs.weak_scope ());

              auto p (s.vars.insert (o.ovr));

              if (!p.second)
                break;

              value& v (p.first);
              v = o.val;
            }

            for (const variable_override& o: ctx->var_overrides)
            {
              // Ours is either project (%foo) or scope (/foo).
              //
              if (o.ovr.visibility == variable_visibility::normal)
                continue;

              scope& s (o.dir
                        ? sm.insert ((out_base / *o.dir).normalize ())->second
                        : rs);

              auto p (s.vars.insert (o.ovr));

              if (!p.second)
                break;

              value& v (p.first);
              v = o.val;
            }
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
          const scope& bs (ctx->scopes.find (ts.out_base));

          // Find the target type and extract the extension.
          //
          auto rp (bs.find_target_type (tn, l));
          const target_type* tt (rp.first);
          optional<string>& e (rp.second);

          if (tt == nullptr)
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

            if (ts.forwarded)
              d = rs.out_path () / d.leaf (rs.src_path ()); // Remap.

            // Figure out if this target is in the src tree.
            //
            dir_path out (ts.out_base != ts.src_base && d.sub (ts.src_base)
                          ? out_src (d, rs)
                          : dir_path ());

            mif->search (mparams,
                         rs, bs,
                         ts.buildfile,
                         target_key {tt, &d, &out, &tn.value, e},
                         l,
                         tgs);
          }
        } // target

        if (dump_load)
          dump (*ctx);

        // Finally, match the rules and perform the operation.
        //
        if (pre_oid != 0)
        {
          l5 ([&]{trace << "start pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});

          if (mif->operation_pre != nullptr)
            mif->operation_pre (mparams, pre_oid); // Cannot be translated.

          ctx->current_operation (*pre_oif, oif);

          action a (mid, pre_oid, oid);

          {
            result_printer p (tgs);
            uint16_t diag (ops.structured_result () ? 0 : 1);

            if (mif->match != nullptr)
              mif->match (mparams, a, tgs, diag, true /* progress */);

            if (dump_match)
              dump (*ctx, a);

            if (mif->execute != nullptr && !ops.match_only ())
              mif->execute (mparams, a, tgs, diag, true /* progress */);
          }

          if (mif->operation_post != nullptr)
            mif->operation_post (mparams, pre_oid);

          l5 ([&]{trace << "end pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});

          tgs.reset ();
        }

        ctx->current_operation (*oif, outer_oif);

        action a (mid, oid, oif->outer_id);

        {
          result_printer p (tgs);
          uint16_t diag (ops.structured_result () ? 0 : 2);

          if (mif->match != nullptr)
            mif->match (mparams, a, tgs, diag, true /* progress */);

          if (dump_match)
            dump (*ctx, a);

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

          ctx->current_operation (*post_oif, oif);

          action a (mid, post_oid, oid);

          {
            result_printer p (tgs);
            uint16_t diag (ops.structured_result () ? 0 : 1);

            if (mif->match != nullptr)
              mif->match (mparams, a, tgs, diag, true /* progress */);

            if (dump_match)
              dump (*ctx, a);

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

  if (ops.stat ())
  {
    text << '\n'
         << "build statistics:" << "\n\n"
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
