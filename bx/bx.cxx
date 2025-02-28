// file      : bx/bx.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <limits>
#include <cstring>   // strcmp(), strchr()
#include <iostream>  // cout
#include <exception> // terminate(), set_terminate(), terminate_handler

#include <libbutl/pager.hxx>
#include <libbutl/fdstream.hxx>        // stderr_fd(), fdterm()
#include <libbutl/backtrace.hxx>       // backtrace()
#include <libbutl/default-options.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/context.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/shell/script/parser.hxx>
#include <libbuild2/shell/script/script.hxx>
#include <libbuild2/shell/script/runner.hxx>

#include <bx/bx-options.hxx>

using namespace butl;
using namespace std;

namespace build2
{
  namespace cli = build::cli;

  int
  main (int argc, char* argv[]);

  static int
  run_script (bx_options&, path script, strings args);

  static int
  run_task (bx_options&, string task, strings args);

  struct bx_cmdline
  {
    // Either script path or task name is present but not both. Empty task
    // name means the default task.
    //
    optional<path> script;
    optional<string> task;

    strings args;
    //strings cmd_vars;

    // Processed/merged option values (unless --help or --version specified).
    //
    uint16_t verbosity = 1;
    optional<bool> diag_color;
  };

  static bx_cmdline
  parse_bx_cmdline (tracer&, int argc, char* argv[], bx_options&);
}

int build2::
run_script (bx_options&, path script, strings args)
{
  context ctx (true /* no_diag_buffer */);

  shell::script::parser p (ctx);
  shell::script::script s (p.pre_parse (ctx.global_scope, script));
  shell::script::environment e (ctx.global_scope, move (script), move (args));
  shell::script::default_runner r;
  return p.execute (e, s, r);
}

int build2::
run_task (bx_options&, string, strings /*args */)
{
  error << "task running not yet implemented";
  return 1;
}

build2::bx_cmdline build2::
parse_bx_cmdline (tracer& trace, int argc, char* argv[], bx_options& ops)
{
  // Note that the diagnostics verbosity level can only be calculated after
  // default options are loaded and merged (see below). Thus, until then we
  // refer to the verbosity level specified on the command line.
  //
  auto verbosity = [&ops] ()
  {
    uint16_t v (
      ops.verbose_specified ()
      ? ops.verbose ()
      : (ops.V ()     ? 3 :
         ops.v ()     ? 2 :
         ops.quiet () ? 0 : 1));
    return v;
  };

  bx_cmdline r;

  // Note that while it's handy to be able to specify options and arguments in
  // any order, here a trailing option may belong to bx or to script/task. So
  // instead we are going to treat script/task as a separator -- everything
  // before belongs to bx (so only bx options) and everything after belongs to
  // the script/task (or to default task if instead of script/task we have
  // `--`).
  //
  try
  {
    // Command line arguments starting position.
    //
    // We want the positions of the command line arguments to be after the
    // default options files. Normally that would be achieved by passing the
    // last position of the previous scanner to the next. The problem is that
    // we parse the command line arguments first (for good reasons). Also the
    // default options files parsing machinery needs the maximum number of
    // arguments to be specified and assigns the positions below this value
    // (see load_default_options() for details). So we are going to "reserve"
    // the first half of the size_t value range for the default options
    // positions and the second half for the command line arguments positions.
    //
    size_t args_pos (numeric_limits<size_t>::max () / 2);
    cli::argv_file_scanner scan (argc, argv, "--options-file", args_pos);

    for (bool opt (true) /*, var (false)*/; scan.more (); )
    {
      // @@ Note that it's unclear whether command line variables will be
      //    specified before the task or after. So we keep interleaving
      //    option parsing support in case it's the former.
      //
      // @@ Note that there are also overrides that come from default
      //    option files below.

      if (opt)
      {
        // Parse the next chunk of options until we reach an argument (or
        // eos).
        //
        if (ops.parse (scan) && !scan.more ())
          break;

        // If we see `--` before we determined if we run a script or a task,
        // then we run the default task and what follows are the arguments for
        // this task.
        //
        // Note that `--` is always a separator before script/task arguments,
        // not before script/task. Which means script/task cannot start with
        // `-`.
        //
        if (strcmp (scan.peek (), "--") == 0)
        {
          scan.next ();

          r.task = "";

          opt = false;
          //var = true

          continue;
        }

        // Fall through.
      }

      const char* s (scan.next ());

      // Treat the first command line argument as a script path or task name.
      //
      // This is a script path if it contains the directory separator or the
      // .bx extension. Otherwise it's a task name.
      //
      // Note that there will always be a directory seperator if invoked via
      // #! since if executed as a simple name, it will be searched in PATH
      // and we will be passed the resolved absolute path.
      //
      if (!r.script && !r.task)
      {
        const char* e;
        if (path_traits::find_separator (s) != nullptr ||
            ((e = path_traits::find_extension (s)) != nullptr &&
             strcmp (e + 1, "bx") == 0))
        {
          try
          {
            r.script = path (s);
          }
          catch (const invalid_path&)
          {
            fail << "invalid script path '" << s << "'";
          }
        }
        else
        {
          r.task = s;
          //var = true;
        }

        opt = false;
        continue;
      }

#if 0
      // See if this is a command line variable.
      //
      // @@ TODO: probably will require a rewrite once we nail the semantics.
      //
      if (var)
      {
        // If we see "--" while parsing command line variables, then we are
        // done parsing them.
        //
        if (strcmp (s, "--") == 0)
        {
          var = false;
          continue;
        }

        if (const char* p = strchr (s, '=')) // Covers =, +=, and =+.
        {
          // Diagnose the empty variable name situation. Note that we don't
          // allow "partially broken down" assignments (as in foo =bar) since
          // foo= bar would be ambigous.
          //
          if (p == s || (p == s + 1 && *s == '+'))
            fail << "missing variable name in '" << s << "'";

          r.cmd_vars.push_back (s);
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

            r.cmd_vars.push_back (move (v));
            continue;
          }
        }

        // Fall through.
      }
#endif

      r.args.push_back (s);
    }

    // If there are no arguments on the command line, then assume this is the
    // default task.
    //
    if (!r.script && !r.task)
      r.task = "";

    // At this point it should be either script or task but not both.
    //
    assert (r.script.has_value () == !r.task.has_value ());

    // Get/set an environment variable tracing the operation.
    //
    auto get_env = [&verbosity, &trace] (const char* nm)
    {
      optional<string> r (getenv (nm));

      if (verbosity () >= 5)
      {
        if (r)
          trace << nm << ": '" << *r << "'";
        else
          trace << nm << ": <NULL>";
      }

      return r;
    };

    auto set_env = [&verbosity, &trace] (const char* nm, const string& vl)
    {
      try
      {
        if (verbosity () >= 5)
          trace << "setting " << nm << "='" << vl << "'";

        setenv (nm, vl);
      }
      catch (const system_error& e)
      {
        // The variable value can potentially be long/multi-line, so let's
        // print it last.
        //
        fail << "unable to set environment variable " << nm << ": " << e <<
          info << "value: '" << vl << "'";
      }
    };

    // Load the default options files, unless --no-default-options is
    // specified on the command line or the BUILD2_DEF_OPT environment
    // variable is set to a value other than 'true' or '1'.
    //
    // @@ If loaded, prepend the default global overrides to the variables
    //    specified on the command line.
    //
    optional<string> env_def (get_env ("BUILD2_DEF_OPT"));

    // False if --no-default-options is specified on the command line. Note
    // that we cache the flag since it can be overridden by a default
    // options file.
    //
    bool cmd_def (!ops.no_default_options ());

    if (cmd_def && (!env_def || *env_def == "true" || *env_def == "1"))
    try
    {
      optional<dir_path> extra;
      if (ops.default_options_specified ())
      {
        extra = ops.default_options ();

        // Note that load_default_options() expects absolute and normalized
        // directory.
        //
        try
        {
          if (extra->relative ())
            extra->complete ();

          extra->normalize ();
        }
        catch (const invalid_path& e)
        {
          fail << "invalid --default-options value " << e.path;
        }
      }

      // Load default options files.
      //
      // Note: the default arguments (command line overrides) are only allowed
      // for the task runner.
      //
      default_options<bx_options> def_ops (
        load_default_options<bx_options,
                             cli::argv_file_scanner,
                             cli::unknown_mode> (
          nullopt /* sys_dir */,
          path::home_directory (), // The home variable is not assigned yet.
          extra,
          default_options_files {{path ("bx.options"),
                                  path (r.script
                                        ? "bx-script.options"
                                        : "bx-task.options")},
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
          },
          "--options-file",
          args_pos,
          1024,
          false /* r.task.has_value () */ /* args */)); // @@ var overrides.

      // Merge the default and command line options.
      //
      ops = merge_default_options (def_ops, ops);

#if 0
      // Merge the default and command line global overrides.
      //
      // Note that the "broken down" variable assignments occupying a single
      // line are naturally supported.
      //
      // @@ Note that in contrast to b driver we don't ignore the default
      //    overrides if BUILD2_VAR_OVR is already set, which feels to be the
      //    right thing to do for some situations and wrong for others:
      //
      //    b (sets BUILD2_VAR_OVR) -> bx (loads ovrs) -> b
      //    bx (loads ovrs) -> b (sets BUILD2_VAR_OVR) ->bx (loads ovrs) -> b
      //
      //    This is the right thing for the first case. For the second case it
      //    feels wrong since the bx default overrides are loaded twice. The
      //    questions is also whether the second scenario is plausible.
      //
      //    Let's leave this for later, when we actually need variable
      //    overrides.
      //
      if (r.task)
        r.cmd_vars =
          merge_default_arguments (
            def_ops,
            r.cmd_vars,
            [] (const default_options_entry<bx_options>& e, const strings&)
            {
              path_name fn (e.file);

              // Verify that all arguments are global overrides.
              //
              for (const string& a: e.arguments)
                //
                // @@ Probably, it makes sense to turn the lambda
                //    parse_b_cmdline()::verify_glb_ovr() into a function and
                //    reuse it here.
                //
                verify_glb_ovr (a, fn, true /* opt */);
            });
#endif
    }
    catch (const invalid_argument& e)
    {
      fail << "unable to load default options files: " << e;
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

    // Propagate disabling of the default options files to the potential
    // nested invocations.
    //
    if (!cmd_def && (!env_def || *env_def != "0"))
      set_env ("BUILD2_DEF_OPT", "0");

    // Validate options.
    //
    if (ops.diag_color () && ops.no_diag_color ())
      fail << "both --diag-color and --no-diag-color specified";
  }
  catch (const cli::exception& e)
  {
    fail << e;
  }

  if (ops.help () || ops.version ())
    return r;

  r.verbosity = verbosity ();

  r.diag_color = (ops.diag_color ()    ? optional<bool> (true)  :
                  ops.no_diag_color () ? optional<bool> (false) : nullopt);

  return r;
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

  try
  {
    init_process ();

    // Parse the command line.
    //
    bx_options ops;
    bx_cmdline cmdl (parse_bx_cmdline (trace, argc, argv, ops));

    // Handle --build2-metadata (see also buildfile).
    //
    if (ops.build2_metadata_specified ())
    {
      auto& o (cout);

      // Note that the export.metadata variable should be the first non-
      // blank/comment line.
      //
      o << "# build2 buildfile bx" << endl
        << "export.metadata = 1 bx" << endl
        << "bx.name = [string] bx" << endl
        << "bx.version = [string] '" << LIBBUILD2_VERSION_FULL << '\'' << endl
        << "bx.checksum = [string] '" << LIBBUILD2_VERSION_FULL << '\'' << endl
        << "bx.environment = [strings] BUILD2_DEF_OPT" << endl;

      return 0;
    }

    // Handle --version.
    //
    if (ops.version ())
    {
      auto& o (cout);

      o << "build2 " << LIBBUILD2_VERSION_ID << endl
        << "libbutl " << LIBBUTL_VERSION_ID << endl
        << "host " << BUILD2_HOST_TRIPLET << endl
        << "Copyright (c) " << BUILD2_COPYRIGHT << "." << endl
        << "This is free software released under the MIT license." << endl;

      return 0;
    }

    // Initialize the diagnostics state.
    //
    {
      init_diag (cmdl.verbosity,
                 false /* silent     */,
                 false /* progress   */,
                 cmdl.diag_color,
                 false /* no_line    */,
                 false /* no_column  */,
                 fdterm (stderr_fd ()));
    }

    // Handle --help.
    //
    if (ops.help ())
    {
      try
      {
        pager p ("bx help",
                 verb >= 2,
                 ops.pager_specified () ? &ops.pager () : nullptr,
                 &ops.pager_option ());

        print_bx_usage (p.stream ());

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

    // Initialize the global state.
    //
    init (&::terminate,
          argv[0],
          true    /* serial_stop  */,
          false   /* mtime_check  */,
          nullopt /* config_sub   */,
          nullopt /* config_guess */);

    // Trace some overall environment information.
    //
    if (verb >= 5)
    {
      optional<string> p (getenv ("PATH"));

      trace << "work: " << work;
      trace << "home: " << home;
      trace << "path: " << (p ? *p : "<NULL>");
      trace << "type: " << (build_installed ? "installed" : "development");
    }

    return cmdl.script
      ? run_script (ops, move (*cmdl.script), move (cmdl.args))
      : run_task (ops, move (*cmdl.task), move (cmdl.args));
  }
  catch (const failed&)
  {
    // Diagnostics has already been issued.
    //
    r = 1;
  }

  return r;
}

int
main (int argc, char* argv[])
{
  return build2::main (argc, argv);
}
