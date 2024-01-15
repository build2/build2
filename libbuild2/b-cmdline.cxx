// file      : libbuild2/b-cmdline.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/b-cmdline.hxx>

#include <limits>
#include <cstring> // strcmp(), strchr()

#include <libbutl/default-options.hxx>

#include <libbuild2/b-options.hxx>
#include <libbuild2/scheduler.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace cli = build2::build::cli;

namespace build2
{
  b_cmdline
  parse_b_cmdline (tracer& trace,
                   int argc, char* argv[],
                   b_options& ops,
                   uint16_t def_verb,
                   size_t def_jobs)
  {
    // Note that the diagnostics verbosity level can only be calculated after
    // default options are loaded and merged (see below). Thus, until then we
    // refer to the verbosity level specified on the command line.
    //
    auto verbosity = [&ops, def_verb] ()
    {
      uint16_t v (
        ops.verbose_specified ()
        ? ops.verbose ()
        : (ops.V () ? 3 :
           ops.v () ? 2 :
           ops.quiet () || ops.silent () ? 0 : def_verb));
      return v;
    };

    b_cmdline r;

    // We want to be able to specify options, vars, and buildspecs in any
    // order (it is really handy to just add -v at the end of the command
    // line).
    //
    try
    {
      // Command line arguments starting position.
      //
      // We want the positions of the command line arguments to be after the
      // default options files. Normally that would be achieved by passing the
      // last position of the previous scanner to the next. The problem is
      // that we parse the command line arguments first (for good reasons).
      // Also the default options files parsing machinery needs the maximum
      // number of arguments to be specified and assigns the positions below
      // this value (see load_default_options() for details). So we are going
      // to "reserve" the first half of the size_t value range for the default
      // options positions and the second half for the command line arguments
      // positions.
      //
      size_t args_pos (numeric_limits<size_t>::max () / 2);
      cli::argv_file_scanner scan (argc, argv, "--options-file", args_pos);

      size_t argn (0);       // Argument count.
      bool shortcut (false); // True if the shortcut syntax is used.

      for (bool opt (true), var (true); scan.more (); )
      {
        if (opt)
        {
          // Parse the next chunk of options until we reach an argument (or
          // eos).
          //
          if (ops.parse (scan) && !scan.more ())
            break;

          // If we see first "--", then we are done parsing options.
          //
          if (strcmp (scan.peek (), "--") == 0)
          {
            scan.next ();
            opt = false;
            continue;
          }

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

        // Merge all the individual buildspec arguments into a single string.
        // We use newlines to separate arguments so that line numbers in
        // diagnostics signify argument numbers. Clever, huh?
        //
        if (argn != 0)
          r.buildspec += '\n';

        r.buildspec += s;

        // See if we are using the shortcut syntax.
        //
        if (argn == 0 && r.buildspec.back () == ':')
        {
          r.buildspec.back () = '(';
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
          r.buildspec.pop_back ();
        else
          r.buildspec += ')';
      }

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

      // If the BUILD2_VAR_OVR environment variable is present, then parse its
      // value as a newline-separated global variable overrides and prepend
      // them to the overrides specified on the command line.
      //
      // Note that this means global overrides may not contain a newline.

      // Verify that the string is a valid global override. Uses the file name
      // and the options flag for diagnostics only.
      //
      auto verify_glb_ovr = [] (const string& v, const path_name& fn, bool opt)
      {
        size_t p (v.find ('=', 1));
        if (p == string::npos || v[0] != '!')
        {
          diag_record dr (fail (fn));
          dr << "expected " << (opt ? "option or " : "") << "global "
             << "variable override instead of '" << v << "'";

          if (p != string::npos)
              dr << info << "prefix variable assignment with '!'";
        }

        if (p == 1 || (p == 2 && v[1] == '+')) // '!=' or '!+=' ?
          fail (fn) << "missing variable name in '" << v << "'";
      };

      optional<string> env_ovr (get_env ("BUILD2_VAR_OVR"));
      if (env_ovr)
      {
        path_name fn ("<BUILD2_VAR_OVR>");

        auto i (r.cmd_vars.begin ());
        for (size_t b (0), e (0); next_word (*env_ovr, b, e, '\n', '\r'); )
        {
          // Extract the override from the current line, stripping the leading
          // and trailing spaces.
          //
          string s (*env_ovr, b, e - b);
          trim (s);

          // Verify and save the override, unless the line is empty.
          //
          if (!s.empty ())
          {
            verify_glb_ovr (s, fn, false /* opt */);
            i = r.cmd_vars.insert (i, move (s)) + 1;
          }
        }
      }

      // Load the default options files, unless --no-default-options is
      // specified on the command line or the BUILD2_DEF_OPT environment
      // variable is set to a value other than 'true' or '1'.
      //
      // If loaded, prepend the default global overrides to the variables
      // specified on the command line, unless BUILD2_VAR_OVR is set in which
      // case just ignore them.
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
        default_options<b_options> def_ops (
          load_default_options<b_options,
                               cli::argv_file_scanner,
                               cli::unknown_mode> (
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
            },
            "--options-file",
            args_pos,
            1024,
            true /* args */));

        // Merge the default and command line options.
        //
        ops = merge_default_options (def_ops, ops);

        // Merge the default and command line global overrides, unless
        // BUILD2_VAR_OVR is already set (in which case we assume this has
        // already been done).
        //
        // Note that the "broken down" variable assignments occupying a single
        // line are naturally supported.
        //
        if (!env_ovr)
          r.cmd_vars =
            merge_default_arguments (
              def_ops,
              r.cmd_vars,
              [&verify_glb_ovr] (const default_options_entry<b_options>& e,
                                 const strings&)
              {
                path_name fn (e.file);

                // Verify that all arguments are global overrides.
                //
                for (const string& a: e.arguments)
                  verify_glb_ovr (a, fn, true /* opt */);
              });
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

      // Verify and save the global overrides present in cmd_vars (default,
      // from the command line, etc), if any, into the BUILD2_VAR_OVR
      // environment variable.
      //
      if (!r.cmd_vars.empty ())
      {
        string ovr;
        for (const string& v: r.cmd_vars)
        {
          if (v[0] == '!')
          {
            if (v.find_first_of ("\n\r") != string::npos)
              fail << "newline in global variable override '" << v << "'";

            if (!ovr.empty ())
              ovr += '\n';

            ovr += v;
          }
        }

        // Optimize for the common case.
        //
        // Note: cmd_vars may contain non-global overrides.
        //
        if (!ovr.empty () && (!env_ovr || *env_ovr != ovr))
          set_env ("BUILD2_VAR_OVR", ovr);
      }

      // Propagate disabling of the default options files to the potential
      // nested invocations.
      //
      if (!cmd_def && (!env_def || *env_def != "0"))
        set_env ("BUILD2_DEF_OPT", "0");

      // Validate options.
      //
      if (ops.progress () && ops.no_progress ())
        fail << "both --progress and --no-progress specified";

      if (ops.diag_color () && ops.no_diag_color ())
        fail << "both --diag-color and --no-diag-color specified";

      if (ops.mtime_check () && ops.no_mtime_check ())
        fail << "both --mtime-check and --no-mtime-check specified";

      if (ops.match_only () && ops.load_only ())
        fail << "both --match-only and --load-only specified";

      if (!ops.dump_specified ())
      {
        // Note: let's allow specifying --dump-format without --dump in case
        // it comes from a default options file or some such.

        if (ops.dump_target_specified ())
          fail << "--dump-target requires --dump";

        if (ops.dump_scope_specified ())
          fail << "--dump-scope requires --dump";
      }
    }
    catch (const cli::exception& e)
    {
      fail << e;
    }

    if (ops.help () || ops.version ())
      return r;

    r.verbosity = verbosity ();

    if (ops.silent () && r.verbosity != 0)
      fail << "specified with -v, -V, or --verbose verbosity level "
           << r.verbosity << " is incompatible with --silent";

    r.progress = (ops.progress ()    ? optional<bool> (true)  :
                  ops.no_progress () ? optional<bool> (false) : nullopt);

    r.diag_color = (ops.diag_color ()    ? optional<bool> (true)  :
                    ops.no_diag_color () ? optional<bool> (false) : nullopt);

    r.mtime_check = (ops.mtime_check ()    ? optional<bool> (true)  :
                     ops.no_mtime_check () ? optional<bool> (false) : nullopt);


    r.config_sub = (ops.config_sub_specified ()
                    ? optional<path> (ops.config_sub ())
                    : nullopt);

    r.config_guess = (ops.config_guess_specified ()
                      ? optional<path> (ops.config_guess ())
                      : nullopt);

    if (ops.jobs_specified ())
      r.jobs = ops.jobs ();
    else if (ops.serial_stop ())
      r.jobs = 1;

    if (def_jobs != 0)
      r.jobs = def_jobs;
    else
    {
      if (r.jobs == 0)
        r.jobs = scheduler::hardware_concurrency ();

      if (r.jobs == 0)
      {
        warn << "unable to determine the number of hardware threads" <<
          info << "falling back to serial execution" <<
          info << "use --jobs|-j to override";

        r.jobs = 1;
      }
    }

    if (ops.max_jobs_specified ())
    {
      r.max_jobs = ops.max_jobs ();

      if (r.max_jobs != 0 && r.max_jobs < r.jobs)
        fail << "invalid --max-jobs|-J value";
    }

    r.max_stack = (ops.max_stack_specified ()
                   ? optional<size_t> (ops.max_stack () * 1024)
                   : nullopt);

    if (ops.file_cache_specified ())
    {
      const string& v (ops.file_cache ());
      if (v == "noop" || v == "none")
        r.fcache_compress = false;
      else if (v == "sync-lz4")
        r.fcache_compress = true;
      else
        fail << "invalid --file-cache value '" << v << "'";
    }

    return r;
  }
}
