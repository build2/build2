// file      : bx/bx.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cstring>   // strcmp()
#include <iostream>  // cout
#include <exception> // terminate(), set_terminate(), terminate_handler

#include <libbutl/pager.hxx>
#include <libbutl/fdstream.hxx>  // stderr_fd(), fdterm()
#include <libbutl/backtrace.hxx> // backtrace()

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/context.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <bx/bx-options.hxx>

using namespace butl;
using namespace std;

namespace build2
{
  namespace cli = build::cli;

  int
  main (int argc, char* argv[]);

  static int
  run_script (bx_options&, path, cli::argv_file_scanner&);

  static int
  run_task (bx_options&, string, cli::argv_file_scanner&);
}

int build2::
run_script (bx_options&, path script, cli::argv_file_scanner& args)
{
  context ctx (true /* no_diag_buffer */);

  text << script;

  while (args.more ())
  {
    text << '\'' << args.next () << '\'';
  }

  return 0;
}

int build2::
run_task (bx_options&, string, cli::argv_file_scanner&)
{
  error << "task running not yet implemented";
  return 1;
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
    // @@ TODO: need to parse properly.
    // @@ TODO: BUILD2_VAR_OVR, BUILD2_DEF_OPT
    // @@ TODO: need to handle cli exceptions.
    //
    bx_options ops;
    cli::argv_file_scanner args (argc, argv, "--options-file");
    ops.parse (args);

    if (ops.diag_color () && ops.no_diag_color ())
      fail << "both --diag-color and --no-diag-color specified";

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
        << "bx.environment = [strings]" << endl;

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
      uint16_t v (
        ops.verbose_specified ()
        ? ops.verbose ()
        : (ops.V ()     ? 3 :
           ops.v ()     ? 2 :
           ops.quiet () ? 0 : 1));

      optional<bool> dc (ops.diag_color ()    ? optional<bool> (true)  :
                         ops.no_diag_color () ? optional<bool> (false) :
                         nullopt);

      init_diag (v     /* verbosity  */,
                 false /* silent     */,
                 false /* progress   */,
                 dc    /* diag_color */,
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

    // The first argument determines whether this is a script or a task.
    // Absent means default task.
    //
    // @@ Maybe provide --task and --script to make explicit?
    //
    const char* a;
    if (args.more ())
    {
      a = args.next ();

      if (strcmp (a, "--") != 0)
      {
        // This is a script path if it contains the directory separator or the
        // .bx extension. Otherwise it's a task name.
        //
        // Note that there will always be a directory seperator if invoked via
        // #! since if executed as a simple name, it will be searched in PATH
        // and we will be passed the resolved absolute path.
        //
        const char* e;
        if (path_traits::find_separator (a) != nullptr ||
            ((e = path_traits::find_extension (a)) != nullptr &&
             strcmp (e + 1, "bx") == 0))
        {
          path f;
          try
          {
            f = path (a);
          }
          catch (const invalid_path&)
          {
            fail << "invalid script path '" << a << "'";
          }

          return run_script (ops, move (f), args);
        }
      }
      else
        a = "";
    }
    else
      a = "";

    return run_task (ops, string (a), args);
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
