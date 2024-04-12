// file      : libbuild2/script/run.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/script/run.hxx>

#ifndef _WIN32
#  include <signal.h>                  // SIG*
#else
#  include <libbutl/win32-utility.hxx> // DBG_TERMINATE_PROCESS
#endif

#include <ios>     // streamsize
#include <cstring> // strchr()

#include <libbutl/regex.hxx>
#include <libbutl/builtin.hxx>
#include <libbutl/fdstream.hxx>     // fdopen_mode, fddup()
#include <libbutl/filesystem.hxx>   // path_search()

#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/script/regex.hxx>
#include <libbuild2/script/timeout.hxx>
#include <libbuild2/script/builtin-options.hxx>

using namespace std;
using namespace butl;

namespace cli = build2::build::cli;

namespace build2
{
  namespace script
  {
    string
    diag_path (const path& d)
    {
      string r ("'");

      r += stream_verb_map ().path < 1
           ? diag_relative (d)
           : d.representation ();

      r += '\'';
      return r;
    }

    string
    diag_path (const dir_name_view& dn)
    {
      string r;
      if (dn.name != nullptr && *dn.name)
      {
        r += **dn.name;
        r += ' ';
      }

      assert (dn.path != nullptr);

      r += diag_path (*dn.path);
      return r;
    }

    // Return the environment temporary directory, creating it if it doesn't
    // exist.
    //
    static inline const dir_path&
    temp_dir (environment& env)
    {
      if (env.temp_dir.empty ())
        env.create_temp_dir ();

      return env.temp_dir;
    }

    // Normalize a path. Also make the relative path absolute using the
    // specified directory unless it is already absolute.
    //
    static path
    normalize (path p, const dir_path& d, const location& l)
    {
      path r (p.absolute () ? move (p) : d / move (p));

      try
      {
        r.normalize ();
      }
      catch (const invalid_path& e)
      {
        fail (l) << "invalid file path " << e.path;
      }

      return r;
    }

    // Check if a path is not empty, the referenced file exists and is not
    // empty.
    //
    static bool
    non_empty (const path& p, const location& ll)
    {
      if (p.empty () || !exists (p))
        return false;

      try
      {
        ifdstream is (p);
        return is.peek () != ifdstream::traits_type::eof ();
      }
      catch (const io_error& e)
      {
        // While there can be no fault of the script command being currently
        // executed let's add the location anyway to help with
        // troubleshooting. And let's stick to that principle down the road.
        //
        fail (ll) << "unable to read " << p << ": " << e << endf;
      }
    }

    // If the file exists, not empty and not larger than 4KB print it to the
    // diag record. The file content goes from the new line and is not
    // indented.
    //
    static void
    print_file (diag_record& d, const path& p, const location& ll)
    {
      if (exists (p))
      {
        try
        {
          ifdstream is (p, ifdstream::badbit);

          if (is.peek () != ifdstream::traits_type::eof ())
          {
            char buf[4096 + 1]; // Extra byte is for terminating '\0'.

            // Note that the string is always '\0'-terminated with a maximum
            // sizeof (buf) - 1 bytes read.
            //
            is.getline (buf, sizeof (buf), '\0');

            // Print if the file fits 4KB-size buffer. Note that if it
            // doesn't the failbit is set.
            //
            if (is.eof ())
            {
              // Suppress the trailing newline character as the diag record
              // adds it's own one when flush.
              //
              streamsize n (is.gcount ());
              assert (n > 0);

              // Note that if the file contains '\0' it will also be counted
              // by gcount(). But even in the worst case we will stay in the
              // buffer boundaries (and so not crash).
              //
              if (buf[n - 1] == '\n')
                buf[n - 1] = '\0';

              d << '\n' << buf;
            }
          }
        }
        catch (const io_error& e)
        {
          fail (ll) << "unable to read " << p << ": " << e;
        }
      }
    }

    // Save a string to the file. Fail if exception is thrown by underlying
    // operations.
    //
    static void
    save (const path& p, const string& s, const location& ll)
    {
      try
      {
        ofdstream os (p);
        os << s;
        os.close ();
      }
      catch (const io_error& e)
      {
        fail (ll) << "unable to write to " << p << ": " << e;
      }
    }

    // Transform string according to here-* redirect modifiers from the {/}
    // set.
    //
    static string
    transform (const string& s,
               bool regex,
               const string& modifiers,
               environment& env)
    {
      if (modifiers.find ('/') == string::npos)
        return s;

      // For targets other than Windows leave the string intact.
      //
      if (env.host.class_ != "windows")
        return s;

      // Convert forward slashes to Windows path separators (escape for
      // regex).
      //
      string r;
      for (size_t p (0);;)
      {
        size_t sp (s.find ('/', p));

        if (sp != string::npos)
        {
          r.append (s, p, sp - p);
          r.append (regex ? "\\\\" : "\\");
          p = sp + 1;
        }
        else
        {
          r.append (s, p, sp);
          break;
        }
      }

      return r;
    }

    // Return true if the script temporary directory is not created yet (and
    // so cannot contain any path), a path is not under the temporary
    // directory or this directory will not be removed on failure.
    //
    static inline bool
    avail_on_failure (const path& p, const environment& env)
    {
      return env.temp_dir.empty () ||
             env.temp_dir_keep     ||
             !p.sub (env.temp_dir);
    }

    // Check if the script command output matches the expected result
    // (redirect value). Noop for redirect types other than none, here_*.
    //
    static bool
    check_output (const path& pr,
                  const path& op,
                  const path& ip,
                  const redirect& rd,
                  const location& ll,
                  environment& env,
                  bool diag,
                  const char* what)
    {
      auto input_info = [&ip, &ll, &env] (diag_record& d)
      {
        if (non_empty (ip, ll) && avail_on_failure (ip, env))
          d << info << "stdin: " << ip;
      };

      auto output_info = [&what, &ll, &env] (diag_record& d,
                                             const path& p,
                                             const char* prefix = "",
                                             const char* suffix = "")
      {
        if (non_empty (p, ll))
        {
          if (avail_on_failure (p, env))
            d << info << prefix << what << suffix << ": " << p;
        }
        else
          d << info << prefix << what << suffix << " is empty";
      };

      if (rd.type == redirect_type::none)
      {
        // Check that there is no output produced.
        //
        assert (!op.empty ());

        if (!non_empty (op, ll))
          return true;

        if (diag)
        {
          diag_record d (error (ll));
          d << pr << " unexpectedly writes to " << what;

          if (avail_on_failure (op, env))
            d << info << what << ": " << op;

          input_info (d);

          // Print cached output.
          //
          print_file (d, op, ll);
        }

        // Fall through (to return false).
        //
      }
      else if (rd.type == redirect_type::here_str_literal ||
               rd.type == redirect_type::here_doc_literal ||
               (rd.type == redirect_type::file &&
                rd.file.mode == redirect_fmode::compare))
      {
        // The expected output is provided as a file or as a string. Save the
        // string to a file in the later case.
        //
        assert (!op.empty ());

        path eop;

        if (rd.type == redirect_type::file)
          eop = normalize (rd.file.path, *env.work_dir.path, ll);
        else
        {
          eop = path (op + ".orig");

          save (eop,
                transform (rd.str, false /* regex */, rd.modifiers (), env),
                ll);

          env.clean_special (eop);
        }

        // Use the diff utility for comparison.
        //
        path dp ("diff");
        process_path pp (run_search (dp, true));

        cstrings args {pp.recall_string (), "-u"};

        // Ignore Windows newline fluff if that's what we are running on.
        //
        if (env.host.class_ == "windows")
          args.push_back ("--strip-trailing-cr");

        // Instruct diff not to print the file paths that won't be available
        // on failure.
        //
        // It seems that the only portable way to achieve this is to abandon
        // the output unified format in the favor of the minimal output.
        // However, the FreeBSD's, OpenBSD's and GNU's (used on Linux, MacOS,
        // Windows, and NetBSD) diff utilities support the -L option that
        // allows to replace the compared file path(s) with custom string(s)
        // in the utility output. We will use this option for both files if
        // any of them won't be available on failure (note that we can't
        // assign a label only for the second file).
        //
        // Add the -L option using the file name as its value if it won't be
        // available on failure and its full path otherwise.
        //
        auto add_label = [&args, &env] (const path& p)
        {
          const char* s (p.string ().c_str ());

          args.push_back ("-L");
          args.push_back (avail_on_failure (p, env)
                          ? s
                          : path::traits_type::find_leaf (s));
        };

        if (!avail_on_failure (eop, env) || !avail_on_failure (op, env))
        {
          add_label (eop);
          add_label (op);
        }

        args.push_back (eop.string ().c_str ());
        args.push_back (op.string ().c_str ());
        args.push_back (nullptr);

        if (verb >= 2)
          print_process (args);

        try
        {
          // Save diff's stdout to a file for troubleshooting and for the
          // optional (if not too large) printing (at the end of
          // diagnostics).
          //
          path ep (op + ".diff");
          auto_fd efd;

          try
          {
            efd = fdopen (ep, fdopen_mode::out | fdopen_mode::create);
            env.clean_special (ep);
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to write to " << ep << ": " << e;
          }

          // Diff utility prints the differences to stdout. But for the
          // user it is a part of the script failure diagnostics so let's
          // redirect stdout to stderr.
          //
          process p (pp, args.data (), 0, 2, efd.get ());
          efd.reset ();

          if (p.wait ())
            return true;

          assert (p.exit);
          const process_exit& pe (*p.exit);

          // Note that both POSIX and GNU diff report error by exiting with
          // the code > 1.
          //
          if (!pe.normal () || pe.code () > 1)
          {
            diag_record d (fail (ll));
            print_process (d, args);
            d << " " << pe;

            print_file (d, ep, ll);
          }

          // Output doesn't match the expected result.
          //
          if (diag)
          {
            diag_record d (error (ll));
            d << pr << " " << what << " doesn't match expected";

            output_info (d, op);
            output_info (d, eop, "expected ");
            output_info (d, ep, "", " diff");
            input_info  (d);

            print_file (d, ep, ll);
          }

          // Fall through (to return false).
          //
        }
        catch (const process_error& e)
        {
          error (ll) << "unable to execute " << pp << ": " << e;

          if (e.child)
            exit (1);

          throw failed ();
        }
      }
      else if (rd.type == redirect_type::here_str_regex ||
               rd.type == redirect_type::here_doc_regex)
      {
        // The overall plan is:
        //
        // 1. Create regex line string. While creating it's line characters
        //    transform regex lines according to the redirect modifiers.
        //
        // 2. Create line regex using the line string. If creation fails
        //    then save the (transformed) regex redirect to a file for
        //    troubleshooting.
        //
        // 3. Parse the output into the literal line string.
        //
        // 4. Match the output line string with the line regex.
        //
        // 5. If match fails save the (transformed) regex redirect to a file
        //    for troubleshooting.
        //
        using namespace regex;

        assert (!op.empty ());

        // Create regex line string.
        //
        line_pool pool;
        line_string rls;
        const regex_lines rl (rd.regex);

        // Parse regex flags.
        //
        // When add support for new flags don't forget to update
        // parse_regex().
        //
        auto parse_flags = [] (const string& f) -> char_flags
        {
          char_flags r (char_flags::none);

          for (char c: f)
          {
            switch (c)
            {
            case 'd': r |= char_flags::idot;  break;
            case 'i': r |= char_flags::icase; break;
            default: assert (false); // Error so should have been checked.
            }
          }

          return r;
        };

        // Return original regex line with the transformation applied.
        //
        auto line = [&rl, &rd, &env] (const regex_line& l) -> string
        {
          string r;
          if (l.regex)                  // Regex (possibly empty),
          {
            r += rl.intro;
            r += transform (l.value, true /* regex */, rd.modifiers (), env);
            r += rl.intro;
            r += l.flags;
          }
          else if (!l.special.empty ()) // Special literal.
            r += rl.intro;
          else                          // Textual literal.
            r += transform (l.value, false /* regex */, rd.modifiers (), env);

          r += l.special;
          return r;
        };

        // Return regex line location.
        //
        // Note that we rely on the fact that the command and regex lines
        // are always belong to the same file.
        //
        auto loc = [&ll] (uint64_t line, uint64_t column) -> location
        {
          location r (ll);
          r.line = line;
          r.column = column;
          return r;
        };

        // Save the regex to file for troubleshooting, return the file path
        // it has been saved to.
        //
        // Note that we save the regex on line regex creation failure or if
        // the program output doesn't match.
        //
        auto save_regex = [&op, &rl, &rd, &ll, &line] () -> path
        {
          path rp (op + ".regex");

          // Encode here-document regex global flags if present as a file
          // name suffix. For example if icase and idot flags are specified
          // the name will look like:
          //
          // stdout.regex-di
          //
          if (rd.type == redirect_type::here_doc_regex && !rl.flags.empty ())
            rp += '-' + rl.flags;

          // Note that if would be more efficient to directly write chunks
          // to file rather than to compose a string first. Hower we don't
          // bother (about performance) for the sake of the code as we
          // already failed.
          //
          string s;
          for (auto b (rl.lines.cbegin ()), i (b), e (rl.lines.cend ());
               i != e; ++i)
          {
            if (i != b) s += '\n';
            s += line (*i);
          }

          save (rp, s, ll);
          return rp;
        };

        // Finally create regex line string.
        //
        // Note that diagnostics doesn't refer to the program path as it is
        // irrelevant to failures at this stage.
        //
        char_flags gf (parse_flags (rl.flags)); // Regex global flags.

        for (const auto& l: rl.lines)
        {
          if (l.regex) // Regex (with optional special characters).
          {
            line_char c;

            // Empty regex is a special case repesenting the blank line.
            //
            if (l.value.empty ())
              c = line_char ("", pool);
            else
            {
              try
              {
                string s (transform (l.value,
                                     true /* regex */,
                                     rd.modifiers (),
                                     env));

                c = line_char (
                  char_regex (s, gf | parse_flags (l.flags)), pool);
              }
              catch (const regex_error& e)
              {
                // Print regex_error description if meaningful.
                //
                diag_record d (fail (loc (l.line, l.column)));

                if (rd.type == redirect_type::here_str_regex)
                  d << "invalid " << what << " regex redirect" << e <<
                    info << "regex: '" << line (l) << "'";
                else
                  d << "invalid char-regex in " << what << " regex redirect"
                    << e <<
                    info << "regex line: '" << line (l) << "'";

                d << endf;
              }
            }

            rls += c; // Append blank literal or regex line char.
          }
          else if (!l.special.empty ()) // Special literal.
          {
            // Literal can not be followed by special characters in the same
            // line.
            //
            assert (l.value.empty ());
          }
          else // Textual literal.
          {
            // Append literal line char.
            //
            rls += line_char (transform (l.value,
                                         false /* regex */,
                                         rd.modifiers (),
                                         env),
                              pool);
          }

          for (char c: l.special)
          {
            if (line_char::syntax (c))
              rls += line_char (c); // Append special line char.
            else
              fail (loc (l.line, l.column))
                << "invalid syntax character '" << c << "' in " << what
                << " regex redirect" <<
                info << "regex line: '" << line (l) << "'";
          }
        }

        // Issue regex error diagnostics and fail.
        //
        auto fail_regex = [&rl, &rd, &loc, &env, &output_info, &save_regex]
                          (const regex_error& e, const string& what)
        {
          const auto& ls (rl.lines);

          // Note that the parser treats both empty here-string (for example
          // >:~'') and empty here-document redirects as an error and so there
          // should be at least one line in the list.
          //
          assert (!ls.empty ());

          diag_record d (fail (rd.type == redirect_type::here_doc_regex
                               ? loc (rd.end_line, rd.end_column)
                               : loc (ls[0].line, ls[0].column)));

          // Print regex_error description if meaningful.
          //
          d << what << " regex redirect" << e;

          // It would be a waste to save the regex into the file just to
          // remove it.
          //
          if (env.temp_dir_keep)
            output_info (d, save_regex (), "", " regex");
        };

        // Create line regex.
        //
        line_regex regex;

        try
        {
          regex = line_regex (move (rls), move (pool));
        }
        catch (const regex_error& e)
        {
          fail_regex (e, string ("invalid ") + what);
        }

        // Parse the output into the literal line string.
        //
        line_string ls;

        try
        {
          // Do not throw when eofbit is set (end of stream reached), and
          // when failbit is set (getline() failed to extract any character).
          //
          // Note that newlines are treated as line-chars separators. That
          // in particular means that the trailing newline produces a blank
          // line-char (empty literal). Empty output produces the zero-length
          // line-string.
          //
          // Also note that we strip the trailing CR characters (otherwise
          // can mismatch when, for example, cross-testing).
          //
          ifdstream is (op, ifdstream::badbit);
          is.peek (); // Sets eofbit for an empty stream.

          while (!is.eof ())
          {
            string s;
            getline (is, s);

            // It is safer to strip CRs in cycle, as msvcrt unexplainably
            // adds too much trailing junk to the system_error descriptions,
            // and so it can appear in programs output. For example:
            //
            // ...: Invalid data.\r\r\n
            //
            // Note that our custom operator<<(ostream&, const exception&)
            // removes this junk.
            //
            while (!s.empty () && s.back () == '\r')
              s.pop_back ();

            // Some regex implementations (e.g., libstdc++, MSVC) are unable
            // to match long strings which they "signal" by running out of
            // stack or otherwise crashing instead of throwing an exception.
            // So we impose some sensible limit that all of them are able to
            // handle for basic expressions (e.g., [ab]+; GCC's limits are the
            // lowest, see bug 86164). See also another check (for the lines
            // number) below.
            //
            // BTW, if we ever need to overcome this limitation (along with
            // various hacks for the two-dimensional regex support), one way
            // would be to factor libc++'s implementation (which doesn't seem
            // to have any stack-related limits) and use it everywhere.
            //
            if (s.size () > 16384)
            {
              diag_record d (fail (ll));
              d << pr << " " << what << " lines too long to match with regex";
              output_info (d, op);
            }

            ls += line_char (move (s), regex.pool);
          }
        }
        catch (const io_error& e)
        {
          fail (ll) << "unable to read " << op << ": " << e;
        }

        if (ls.size () > 12288)
        {
          diag_record d (fail (ll));
          d << pr << " " << what << " has too many lines to match with regex";
          output_info (d, op);
        }

        // Note that a here-document regex without ':' modifier can never
        // match an empty output since it always contains the trailing empty
        // line-char. This can be confusing, as for example while testing a
        // program which can print some line or nothing with the following
        // test:
        //
        // $* >>~%EOO%
        //   %(
        //   Hello, World!
        //   %)?
        //   EOO
        //
        // Note that the above line-regex contains 4 line-chars and will never
        // match empty output.
        //
        // Thus, let's complete an empty output with an empty line-char for
        // such a regex, so it may potentially match.
        //
        if (ls.empty ()                              &&
            rd.type == redirect_type::here_doc_regex &&
            rd.modifiers ().find (':') == string::npos)
        {
          ls += line_char (string (), regex.pool);
        }

        // Match the output with the regex.
        //
        // Note that we don't distinguish between the line_regex and
        // char_regex match failures. While it would be convenient for the
        // user if we provide additional information in the latter case (regex
        // line number, etc), the implementation feels too hairy for now
        // (would require to pull additional information into char_regex,
        // etc). Though, we may want to implement it in the future.
        //
        try
        {
          if (regex_match (ls, regex))
            return true;
        }
        catch (const regex_error& e)
        {
          fail_regex (e, string ("unable to match ") + what);
        }

        // Output doesn't match the regex.
        //
        // Unless the temporary directory is removed on failure, we save the
        // regex to file for troubleshooting regardless of whether we print
        // the diagnostics or not. We, however, register it for cleanup in the
        // later case (the expression may still succeed, we can be evaluating
        // the flow control construct condition, etc).
        //
        optional<path> rp;
        if (env.temp_dir_keep)
          rp = save_regex ();

        if (diag)
        {
          diag_record d (error (ll));
          d << pr << " " << what << " doesn't match regex";

          output_info (d, op);

          if (rp)
            output_info (d, *rp, "", " regex");

          input_info  (d);

          // Print cached output.
          //
          print_file (d, op, ll);
        }
        else if (rp)
          env.clean_special (*rp);

        // Fall through (to return false).
        //
      }
      else // Noop.
        return true;

      return false;
    }

    // The export pseudo-builtin: add/remove the variables to/from the script
    // commands execution environment and/or clear the previous additions/
    // removals.
    //
    // export [-c|--clear <name>]... [-u|--unset <name>]... [<name>=<value>]...
    //
    static void
    export_builtin (environment& env, const strings& args, const location& ll)
    {
      try
      {
        cli::vector_scanner scan (args);
        export_options ops (scan);

        // Validate a variable name.
        //
        auto verify_name = [&ll] (const string& name, const char* opt)
        {
          verify_environment_var_name (name, "export: ", ll, opt);
        };

        // Parse options (variable set/unset cleanups and unsets).
        //
        for (const string& v: ops.clear ())
        {
          verify_name (v, "-c|--clear");

          environment_vars::iterator i (env.exported_vars.find (v));

          if (i != env.exported_vars.end ())
            env.exported_vars.erase (i);
        }

        for (string& v: ops.unset ())
        {
          verify_name (v, "-u|--unset");

          env.exported_vars.add (move (v));
        }

        // Parse arguments (variable sets).
        //
        while (scan.more ())
        {
          string a (scan.next ());
          verify_environment_var_assignment (a, "export: ", ll);

          env.exported_vars.add (move (a));
        }
      }
      catch (const cli::exception& e)
      {
        fail (ll) << "export: " << e;
      }
    }

    // The timeout pseudo-builtin: set the script timeout. See the script-
    // specific set_timeout() implementations for the exact semantics.
    //
    // timeout [-s|--success] <timeout>
    //
    static void
    timeout_builtin (environment& env,
                     const strings& args,
                     const location& ll)
    {
      try
      {
        // Parse arguments.
        //
        cli::vector_scanner scan (args);
        timeout_options ops (scan);

        if (!scan.more ())
          fail (ll) << "timeout: missing timeout";

        string a (scan.next ());

        if (scan.more ())
          fail (ll) << "timeout: unexpected argument '" << scan.next () << "'";

        env.set_timeout (a, ops.success (), ll);
      }
      catch (const cli::exception& e)
      {
        fail (ll) << "timeout: " << e;
      }
    }

    // The exit pseudo-builtin: exit the script successfully, or print the
    // diagnostics and exit the script unsuccessfully. Always throw exit
    // exception.
    //
    // exit [<diagnostics>]
    //
    [[noreturn]] static void
    exit_builtin (const strings& args, const location& ll)
    {
      auto i (args.begin ());
      auto e (args.end ());

      // Process arguments.
      //
      // If no argument is specified, then exit successfully. Otherwise,
      // print the diagnostics and exit unsuccessfully.
      //
      if (i == e)
        throw exit (true);

      const string& s (*i++);

      if (i != e)
        fail (ll) << "exit: unexpected argument '" << *i << "'";

      error (ll) << s;
      throw exit (false);
    }

    // Return the command program path for diagnostics.
    //
    static inline path
    cmd_path (const command& c)
    {
      return c.program.initial == nullptr         // Not pre-searched?
             ? c.program.recall
             : path (c.program.recall_string ());
    }

    // Read the stream content into a string, optionally splitting the input
    // data at whitespaces or newlines in which case return one, potentially
    // incomplete, substring at a time (see the set builtin options for the
    // splitting semantics). Throw io_error on the underlying OS error.
    //
    // On POSIX expects the stream to be non-blocking and its exception mask
    // to have at least badbit. On Windows can also handle a blocking stream.
    //
    // Note that on Windows we can only turn pipe file descriptors into the
    // non-blocking mode. Thus, we have no choice but to read from descriptors
    // of other types synchronously there. That implies that we can
    // potentially block indefinitely reading a file and missing a deadline on
    // Windows. Note though, that the user can normally rewrite the command,
    // for example, `set foo <<<file` with `cat file | set foo` to avoid this
    // problem.
    //
    class stream_reader
    {
    public:
      stream_reader (ifdstream&, bool whitespace, bool newline, bool exact);

      // Read next substring. Return true if the substring has been read or
      // false if it should be called again once the stream has more data to
      // read. Also return true on eof (in which case no substring is read).
      // The string must be empty on the first call. Throw ios::failure on the
      // underlying OS error.
      //
      // Note that there could still be data to read in the stream's buffer
      // (as opposed to file descriptor) after this function returns true and
      // you should be careful not to block on fdselect() in this case. The
      // recommended usage pattern is similar to that of
      // butl::getline_non_blocking(). The only difference is that
      // ifdstream::eof() needs to be used instead of butl::eof() since this
      // function doesn't set failbit and only sets eofbit after the last
      // substring is returned.
      //
      bool
      next (string&);

    private:
      ifdstream& is_;
      bool whitespace_;
      bool newline_;
      bool exact_;

      bool empty_ = true; // Set to false after the first character is read.
    };

    stream_reader::
    stream_reader (ifdstream& is, bool ws, bool nl, bool ex)
        : is_ (is),
          whitespace_ (ws),
          newline_ (nl),
          exact_ (ex)
    {
    }

    bool stream_reader::
    next (string& ss)
    {
#ifndef _WIN32
      assert ((is_.exceptions () & ifdstream::badbit) != 0 && !is_.blocking ());
#else
      assert ((is_.exceptions () & ifdstream::badbit) != 0);
#endif

      fdstreambuf& sb (*static_cast<fdstreambuf*> (is_.rdbuf ()));

      // Return the number of characters available in the stream buffer's get
      // area, which can be:
      //
      // -1 -- EOF.
      //  0 -- no data since blocked before encountering more data/EOF.
      // >0 -- there is some data.
      //
      // Note that on Windows if the stream is blocking, then the lambda calls
      // underflow() instead of returning 0.
      //
      // @@ Probably we can call underflow() only once per the next() call,
      //    emulating the 'no data' case. This will allow the caller to
      //    perform some housekeeping (reading other streams, checking for the
      //    deadline, etc). But let's keep it simple for now.
      //
      auto avail = [&sb] () -> streamsize
      {
        // Note that here we reasonably assume that any failure in in_avail()
        // will lead to badbit and thus an exception (see showmanyc()).
        //
        streamsize r (sb.in_avail ());

#ifdef _WIN32
        if (r == 0 && sb.blocking ())
        {
          if (sb.underflow () == ifdstream::traits_type::eof ())
            return -1;

          r = sb.in_avail ();

          assert (r != 0); // We wouldn't be here otherwise.
        }
#endif

        return r;
      };

      // Read until blocked (0), EOF (-1) or encounter the delimiter.
      //
      streamsize s;
      while ((s = avail ()) > 0)
      {
        if (empty_)
          empty_ = false;

        const char* p (sb.gptr ());
        size_t n (sb.egptr () - p);

        // We move p and bump by the number of consumed characters.
        //
        auto bump = [&sb, &p] () {sb.gbump (static_cast<int> (p - sb.gptr ()));};

        if (whitespace_) // The whitespace mode.
        {
          const char* sep (" \n\r\t");

          // Skip the whitespaces.
          //
          for (; n != 0 && strchr (sep, *p) != nullptr; ++p, --n) ;

          // If there are any non-whitespace characters in the get area, then
          // append them to the resulting substring until a whitespace
          // character is encountered.
          //
          if (n != 0)
          {
            // Append the non-whitespace characters.
            //
            for (char c; n != 0 && strchr (sep, c = *p) == nullptr; ++p, --n)
              ss += c;

            // If a separator is encountered, then consume it, bump, and
            // return the substring.
            //
            if (n != 0)
            {
              ++p; --n; // Consume the separator character.

              bump ();
              return true;
            }

            // Fall through.
          }

          bump (); // Bump and continue reading.
        }
        else             // The newline or no-split mode.
        {
          // Note that we don't collapse multiple consecutive newlines.
          //
          // Note also that we always sanitize CRs, so in the no-split mode we
          // need to loop rather than consume the whole get area at once.
          //
          while (n != 0)
          {
            // Append the characters until the newline character or the end of
            // the get area is encountered.
            //
            char c;
            for (; n != 0 && (c = *p) != '\n'; ++p, --n)
              ss += c;

            // If the newline character is encountered, then sanitize CRs and
            // return the substring in the newline mode and continue
            // parsing/reading otherwise.
            //
            if (n != 0)
            {
              // Strip the trailing CRs that can appear while, for example,
              // cross-testing Windows target or as a part of msvcrt junk
              // production (see above).
              //
              while (!ss.empty () && ss.back () == '\r')
                ss.pop_back ();

              assert (c == '\n');

              ++p; --n; // Consume the newline character.

              if (newline_)
              {
                bump ();
                return true;
              }

              ss += c; // Append newline to the resulting string.

              // Fall through.
            }

            bump (); // Bump and continue parsing/reading.
          }
        }
      }

      // Here s can be:
      //
      // -1 -- EOF.
      //  0 -- blocked before encountering delimiter/EOF.
      //
      // Note: >0 (encountered the delimiter) case is handled in-place.
      //
      assert (s == -1 || s == 0);

      if (s == -1)
      {
        // Return the last substring if it is not empty or it is the trailing
        // "blank" in the exact mode. Otherwise, set eofbit for the stream
        // indicating that we are done.
        //
        if (!ss.empty () || (exact_ && !empty_))
        {
          // Also, strip the trailing newline character, if present, in the
          // no-split no-exact mode.
          //
          if (!ss.empty () && ss.back () == '\n' && // Trailing newline.
              !newline_ && !whitespace_ && !exact_) // No-split no-exact mode.
          {
            ss.pop_back ();
          }

          exact_ = false; // Make sure we will set eofbit on the next call.
        }
        else
          is_.setstate (ifdstream::eofbit);
      }

      return s == -1;
    }

    // Stack-allocated linked list of information about the running pipeline
    // processes and builtins.
    //
    // Note: constructed incrementally.
    //
    struct pipe_command
    {
      // Initially NULL. Set to the address of the process or builtin object
      // when it is created. Reset back to NULL when the respective
      // process/builtin is executed and its exit status is collected (see
      // complete_pipe() for details).
      //
      // We could probably use a union here, but let's keep it simple for now
      // (at least one is NULL).
      //
      process* proc = nullptr;
      builtin* bltn = nullptr;

      const command&            cmd;
      const cstrings*           args = nullptr;
      const optional<deadline>& dl;

      diag_buffer dbuf;

      bool terminated = false; // True if this command has been terminated.

      // True if this command has been terminated but we failed to read out
      // its stdout and/or stderr streams in the reasonable timeframe (2
      // seconds) after the termination.
      //
      // Note that this may happen if there is a still running child process
      // of the terminated command which has inherited the parent's stdout and
      // stderr file descriptors.
      //
      bool unread_stdout = false;
      bool unread_stderr = false;

      // Only for diagnostics.
      //
      const location& loc;
      const path*     isp = nullptr; // stdin  cache.
      const path*     osp = nullptr; // stdout cache.
      const path*     esp = nullptr; // stderr cache.

      pipe_command* prev; // NULL for the left-most command.
      pipe_command* next; // Left-most command for the right-most command.

      pipe_command (context& x,
                    const command& c,
                    const optional<deadline>& d,
                    const location& l,
                    pipe_command* p,
                    pipe_command* f)
          : cmd (c), dl (d), dbuf (x), loc (l), prev (p), next (f) {}
    };

    // Wait for a process/builtin to complete until the deadline is reached
    // and return the underlying wait function result (optional<something>).
    //
    template<typename P>
    static auto
    timed_wait (P& p, const timestamp& deadline) -> decltype(p.try_wait ())
    {
      timestamp now (system_clock::now ());
      return deadline > now ? p.timed_wait (deadline - now) : p.try_wait ();
    }

    // Terminate the pipeline processes starting from the specified one and up
    // to the leftmost one and then kill those which didn't terminate after 2
    // seconds.
    //
    // After that wait for the pipeline builtins completion. Since their
    // standard streams should no longer be written to or read from by any
    // process, that shouldn't take long. If, however, they won't be able to
    // complete in 2 seconds, then some of them have probably stuck while
    // communicating with a slow filesystem device or similar, and since we
    // currently have no way to terminate asynchronous builtins, we have no
    // choice but to abort.
    //
    // Issue diagnostics and fail if something goes wrong, but still try to
    // terminate/kill all the pipe processes.
    //
    static void
    term_pipe (pipe_command* pc, tracer& trace)
    {
      auto prog = [] (pipe_command* c) {return cmd_path (c->cmd);};

      // Terminate processes gracefully and set the terminate flag for the
      // pipe commands.
      //
      diag_record dr;
      for (pipe_command* c (pc); c != nullptr; c = c->prev)
      {
        if (process* p = c->proc)
        try
        {
          l5 ([&]{trace (c->loc) << "terminating: " << c->cmd;});

          p->term ();
        }
        catch (const process_error& e)
        {
          // If unable to terminate the process for any reason (the process is
          // exiting on Windows, etc) then just ignore this, postponing the
          // potential failure till the kill() call.
          //
          l5 ([&]{trace (c->loc) << "unable to terminate " << prog (c)
                                 << ": " << e;});
        }

        c->terminated = true;
      }

      // Wait a bit for the processes to terminate and kill the remaining
      // ones.
      //
      timestamp dl (system_clock::now () + chrono::seconds (2));

      for (pipe_command* c (pc); c != nullptr; c = c->prev)
      {
        if (process* p = c->proc)
        try
        {
          l5 ([&]{trace (c->loc) << "waiting: " << c->cmd;});

          if (!timed_wait (*p, dl))
          {
            l5 ([&]{trace (c->loc) << "killing: " << c->cmd;});

            p->kill ();
            p->wait ();
          }
        }
        catch (const process_error& e)
        {
          dr << fail (c->loc) << "unable to wait/kill " << prog (c) << ": "
             << e;
        }
      }

      // Wait a bit for the builtins to complete and abort if any remain
      // running.
      //
      dl = system_clock::now () + chrono::seconds (2);

      for (pipe_command* c (pc); c != nullptr; c = c->prev)
      {
        if (builtin* b = c->bltn)
        try
        {
          l5 ([&]{trace (c->loc) << "waiting: " << c->cmd;});

          if (!timed_wait (*b, dl))
          {
            error (c->loc) << prog (c) << " builtin hanged, aborting";
            terminate (false /* trace */);
          }
        }
        catch (const system_error& e)
        {
          dr << fail (c->loc) << "unable to wait for " << prog (c) << ": "
             << e;
        }
      }
    }

    void
    read (auto_fd&& in,
          bool whitespace, bool newline, bool exact,
          const function<void (string&&)>& cf,
          pipe_command* pipeline,
          const optional<deadline>& dl,
          const location& ll,
          const char* what)
    {
      tracer trace ("script::stream_read");

      // Note: stays blocking on Windows if the descriptor is not of the pipe
      // type.
      //
#ifndef _WIN32
      fdstream_mode m (fdstream_mode::non_blocking);
#else
      fdstream_mode m (pipeline != nullptr
                       ? fdstream_mode::non_blocking
                       : fdstream_mode::blocking);
#endif

      ifdstream is (move (in), m, ifdstream::badbit);
      stream_reader sr (is, whitespace, newline, exact);

      fdselect_set fds;
      for (pipe_command* c (pipeline); c != nullptr; c = c->prev)
      {
        diag_buffer& b (c->dbuf);

        if (b.is.is_open ())
          fds.emplace_back (b.is.fd (), c);
      }

      fds.emplace_back (is.fd ());
      fdselect_state& ist (fds.back ());
      size_t unread (fds.size ());

      optional<timestamp> dlt (dl ? dl->value : optional<timestamp> ());

      // If there are some left-hand side processes/builtins running, then
      // terminate them and, if there are unread stdout/stderr file
      // descriptors, then increase the deadline by another 2 seconds and
      // return true. In this case the term() should be called again upon
      // reaching the timeout. Otherwise return false. If there are no
      // left-hand side processes/builtins running, then fail straight away.
      //
      // Note that in the former case the further reading will be performed
      // with the adjusted timeout. We assume that this timeout is normally
      // sufficient to read out the buffered data written by the already
      // terminated processes. If, however, that's not the case (see
      // pipe_command for the possible reasons), then term() needs to be
      // called for the second time and the reading should be interrupted
      // afterwards.
      //
      auto term = [&dlt, pipeline, &fds, &ist, &is, &unread,
                   &trace, &ll, what, terminated = false] () mutable -> bool
      {
        // Can only be called if the deadline is specified.
        //
        assert (dlt);

        if (pipeline == nullptr)
          fail (ll) << what << " terminated: execution timeout expired";

        if (!terminated)
        {
          // Terminate the pipeline and adjust the deadline.
          //

          // Note that if we are still reading the stream and it's a builtin
          // stdout, then we need to close it before terminating the pipeline.
          // Not doing so can result in blocking this builtin on the write
          // operation and thus aborting the build2 process (see term_pipe()
          // for details).
          //
          // Should we do the same for all the pipeline builtins' stderr
          // streams? No we don't, since the builtin diagnostics is assumed to
          // always fit the pipe buffer (see libbutl/builtin.cxx for details).
          // Thus, we will leave them open to fully read out the diagnostics.
          //
          if (ist.fd != nullfd && pipeline->bltn != nullptr)
          {
            try
            {
              is.close ();
            }
            catch (const io_error&)
            {
              // Not much we can do here.
            }

            ist.fd = nullfd;
            --unread;
          }

          term_pipe (pipeline, trace);
          terminated = true;

          if (unread != 0)
            dlt = system_clock::now () + chrono::seconds (2);

          return unread != 0;
        }
        else
        {
          // Set the unread_{stderr,stdout} flags to true for the commands
          // whose streams are not fully read yet.
          //

          // Can only be called after the first call of term() which would
          // throw failed if pipeline is NULL.
          //
          assert (pipeline != nullptr);

          for (fdselect_state& s: fds)
          {
            if (s.fd != nullfd)
            {
              if (s.data != nullptr) // stderr.
              {
                pipe_command* c (static_cast<pipe_command*> (s.data));

                c->unread_stderr = true;

                // Let's also close the stderr stream not to confuse
                // diag_buffer::close() with a not fully read stream (eof is
                // not reached, etc).
                //
                try
                {
                  c->dbuf.is.close ();
                }
                catch (const io_error&)
                {
                  // Not much we can do here. Anyway the diagnostics will be
                  // issued by complete_pipe().
                }
              }
              else                   // stdout.
                pipeline->unread_stdout = true;
            }
          }

          return false;
        }
      };

      // Note that on Windows if the file descriptor is not a pipe, then
      // ifdstream assumes the blocking mode for which ifdselect() would throw
      // invalid_argument. Such a descriptor can, however, only appear for the
      // first command in the pipeline and so fds will only contain the input
      // stream's descriptor. That all means that this descriptor will be read
      // out by a series of the stream_reader::next() calls which can only
      // return true and thus no ifdselect() calls will ever be made.
      //
      string s;
      while (unread != 0)
      {
        // Read any pending data from the input stream.
        //
        if (ist.fd != nullfd)
        {
          // Prior to reading let's check that the deadline, if specified, is
          // not reached. This way we handle the (hypothetical) case when we
          // are continuously fed with the data without delays and thus can
          // never get to ifdselect() which watches for the deadline. Also
          // this check is the only way to bail out early on Windows for a
          // blocking file descriptor.
          //
          if (dlt && *dlt <= system_clock::now ())
          {
            if (!term ())
              break;
          }

          if (sr.next (s))
          {
            if (!is.eof ())
            {
              // Consume the substring.
              //
              cf (move (s));
              s.clear ();
            }
            else
            {
              ist.fd = nullfd;
              --unread;
            }

            continue;
          }
        }

        try
        {
          // Wait until the data appear in any of the streams. If a deadline
          // is specified, then pass the timeout to fdselect().
          //
          if (dlt)
          {
            timestamp now (system_clock::now ());

            if (*dlt <= now || ifdselect (fds, *dlt - now) == 0)
            {
              if (term ())
                continue;
              else
                break;
            }
          }
          else
            ifdselect (fds);

          // Read out the pending data from the stderr streams.
          //
          for (fdselect_state& s: fds)
          {
            if (s.ready           &&
                s.data != nullptr &&
                !static_cast<pipe_command*> (s.data)->dbuf.read ())
            {
              s.fd = nullfd;
              --unread;
            }
          }
        }
        catch (const io_error& e)
        {
          fail (ll) << "io error reading pipeline streams: " << e;
        }
      }
    }

    // The set pseudo-builtin: set variable from the stdin input.
    //
    // set [-e|--exact] [(-n|--newline)|(-w|--whitespace)] <var> [<attr>]
    //
    static void
    set_builtin (environment& env,
                 const strings& args,
                 auto_fd in,
                 pipe_command* pipeline,
                 const optional<deadline>& dl,
                 const location& ll)
    {
      tracer trace ("script::set_builtin");

      try
      {
        // Parse arguments.
        //
        cli::vector_scanner scan (args);
        set_options ops (scan);

        if (ops.whitespace () && ops.newline ())
          fail (ll) << "set: both -n|--newline and -w|--whitespace specified";

        if (!scan.more ())
          fail (ll) << "set: missing variable name";

        string vname (scan.next ());
        if (vname.empty ())
          fail (ll) << "set: empty variable name";

        // Detect patterns analogous to parser::parse_variable_name() (so we
        // diagnose `set x[string]`).
        //
        if (vname.find_first_of ("[*?") != string::npos)
          fail (ll) << "set: expected variable name instead of " << vname;

        string attrs;
        if (scan.more ())
        {
          attrs = scan.next ();

          if (attrs.empty ())
            fail (ll) << "set: empty variable attributes";

          if (scan.more ())
            fail (ll) << "set: unexpected argument '" << scan.next () << "'";
        }

        // Parse the stream content into the variable value.
        //
        names ns;

        read (move (in),
              ops.whitespace (), ops.newline (), ops.exact (),
              [&ns] (string&& s) {ns.emplace_back (move (s));},
              pipeline,
              dl,
              ll,
              "set");

        env.set_variable (move (vname), move (ns), attrs, ll);
      }
      catch (const io_error& e)
      {
        fail (ll) << "set: unable to read from stdin: " << e;
      }
      catch (const cli::exception& e)
      {
        fail (ll) << "set: " << e;
      }
    }

    // Sorted array of builtins that support filesystem entries cleanup.
    //
    static const char* cleanup_builtins[] = {
      "cp", "ln", "mkdir", "mv", "touch"};

    static inline bool
    cleanup_builtin (const string& name)
    {
      return binary_search (
        cleanup_builtins,
        cleanup_builtins +
        sizeof (cleanup_builtins) / sizeof (*cleanup_builtins),
        name);
    }

    static bool
    run_pipe (environment& env,
              command_pipe::const_iterator bc,
              command_pipe::const_iterator ec,
              auto_fd ifd,
              const iteration_index* ii, size_t li, size_t ci,
              const location& ll,
              bool diag,
              const function<command_function>& cf, bool last_cmd,
              optional<deadline> dl = nullopt,
              pipe_command* prev_cmd = nullptr)
    {
      tracer trace ("script::run_pipe");

      // At the end of the pipeline read out its stdout, if requested.
      //
      if (bc == ec)
      {
        if (cf != nullptr)
        {
          assert (!last_cmd); // Otherwise we wouldn't be here.

          // The pipeline can't be empty.
          //
          assert (ifd != nullfd && prev_cmd != nullptr);

          const command& c (prev_cmd->cmd);

          try
          {
            cf (env, strings () /* arguments */,
                move (ifd), prev_cmd,
                dl,
                ll);
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to read from " << cmd_path (c) << " stdout: "
                      << e;
          }
        }

        return true;
      }

      // The overall plan is to run the first command in the pipe, reading its
      // input from the file descriptor passed (or, for the first command,
      // according to stdin redirect specification) and redirecting its output
      // to the right-hand part of the pipe recursively. Fail if the
      // right-hand part fails. Otherwise check the process exit code, match
      // stderr (and stdout for the last command in the pipe) according to
      // redirect specification(s) and fail if any of the above fails.
      //
      // If the command has a deadline, then terminate the whole pipeline when
      // the deadline is reached. This way the pipeline processes get a chance
      // to terminate gracefully, which in particular may require to interrupt
      // their IO operations, closing their standard streams readers and
      // writers.
      //
      const command& c (*bc);

      const dir_path& wdir (*env.work_dir.path);

      // Register the command explicit cleanups. Verify that the path being
      // cleaned up is a sub-path of the script working directory. Fail if
      // this is not the case.
      //
      for (const auto& cl: c.cleanups)
      {
        const path& p (cl.path);
        path np (normalize (p, wdir, ll));

        const string& ls (np.leaf ().string ());
        bool wc (ls == "*" || ls == "**" || ls == "***");
        const path& cp (wc ? np.directory () : np);
        const dir_path* sd (env.sandbox_dir.path);

        if (sd != nullptr && !cp.sub (*sd))
          fail (ll) << (wc                ? "wildcard"  :
                        p.to_directory () ? "directory" :
                                            "file")
                    << " cleanup " << p << " is out of "
                    << diag_path (env.sandbox_dir);

        env.clean ({cl.type, move (np)}, false);
      }

      // If stdin file descriptor is not open then this is the first pipeline
      // command.
      //
      bool first (ifd.get () == -1);

      command_pipe::const_iterator nc (bc + 1);
      bool last (nc == ec);

      // Make sure that stdout is not redirected if meant to be read (last_cmd
      // is false) or cannot not be produced (last_cmd is true).
      //
      if (last && c.out && cf != nullptr)
        fail (ll) << "stdout cannot be redirected";

      // True if the process path is not pre-searched and the program path
      // still needs to be resolved.
      //
      bool resolve (c.program.initial == nullptr);

      // Program name that may require resolution.
      //
      const string& program (c.program.recall.string ());

      const redirect& in ((c.in ? *c.in : env.in).effective ());

      const redirect* out (!last || (cf != nullptr && !last_cmd)
                           ? nullptr // stdout is piped.
                           : &(c.out ? *c.out : env.out).effective ());

      const redirect& err ((c.err ? *c.err : env.err).effective ());

      auto process_args = [&c] () -> cstrings
      {
        return build2::process_args (c.program.recall_string (), c.arguments);
      };

      // Prior to opening file descriptors for command input/output redirects
      // let's check if the command is the exit, export, or timeout
      // builtin. Being a builtin syntactically they differ from the regular
      // ones in a number of ways. They don't communicate with standard
      // streams, so redirecting them is meaningless. They may appear only as
      // a single command in a pipeline. They don't return any value, so
      // checking their exit status is meaningless as well. That all means we
      // can short-circuit here calling the builtin and bailing out right
      // after that. Checking that the user didn't specify any variables,
      // timeout, redirects, or exit code check sounds like a right thing to
      // do.
      //
      if (resolve &&
          (program == "exit" || program == "export" || program == "timeout"))
      {
        // In case the builtin is erroneously pipelined from the other
        // command, we will close stdin gracefully (reading out the stream
        // content), to make sure that the command doesn't print any unwanted
        // diagnostics about IO operation failure.
        //
        if (ifd != nullfd)
        {
          // Note that we can't use ifdstream dtor in the skip mode here since
          // it turns the stream into the blocking mode and we won't be able
          // to read out the potentially buffered stderr for the
          // pipeline. Using read() is also not ideal since it performs
          // parsing and allocations needlessly. This, however, is probably ok
          // for such an uncommon case.
          //
          //ifdstream (move (ifd), fdstream_mode::skip);

          // Let's try to minimize the allocation size splitting the input
          // data at whitespaces.
          //
          read (move (ifd),
                true /* whitespace */,
                false /* newline */,
                false /* exact */,
                [] (string&&) {}, // Just drop the string.
                prev_cmd,
                dl,
                ll,
                program.c_str ());
        }

        if (!first || !last)
          fail (ll) << program << " builtin must be the only pipe command";

        if (c.cwd)
          fail (ll) << "current working directory cannot be specified for "
                    << program << " builtin";

        if (!c.variables.empty ())
          fail (ll) << "environment variables cannot be (un)set for "
                    << program << " builtin";

        if (c.timeout)
          fail (ll) << "timeout cannot be specified for " << program
                    << " builtin";

        if (c.in)
          fail (ll) << program << " builtin stdin cannot be redirected";

        if (c.out)
          fail (ll) << program << " builtin stdout cannot be redirected";

        if (cf != nullptr && !last_cmd)
          fail (ll) << program << " builtin stdout cannot be read";

        if (c.err)
          fail (ll) << program << " builtin stderr cannot be redirected";

        if (c.exit)
          fail (ll) << program << " builtin exit code cannot be checked";

        if (verb >= 2)
          print_process (process_args ());

        if (program == "exit")
        {
          exit_builtin (c.arguments, ll); // Throws exit exception.
        }
        else if (program == "export")
        {
          export_builtin (env, c.arguments, ll);
          return true;
        }
        else if (program == "timeout")
        {
          timeout_builtin (env, c.arguments, ll);
          return true;
        }
        else
          assert (false);
      }

      // Create a unique path for a command standard stream cache file.
      //
      auto std_path = [&env, ii, &li, &ci, &ll] (const char* nm) -> path
      {
        using std::to_string;

        string s (nm);
        size_t n (s.size ());

        if (ii != nullptr)
        {
          // Note: reverse order (outermost to innermost).
          //
          for (const iteration_index* i (ii); i != nullptr; i = i->prev)
            s.insert (n, "-i" + to_string (i->index));
        }

        // 0 if belongs to a single-line script, otherwise is the command line
        // number (start from one) in the script.
        //
        if (li != 0)
        {
          s += "-n";
          s += to_string (li);
        }

        // 0 if belongs to a single-command expression, otherwise is the
        // command number (start from one) in the expression.
        //
        // Note that the name like stdin-N can relate to N-th command of a
        // single-line script or to N-th single-command line of multi-line
        // script. These cases are mutually exclusive and so are unambiguous.
        //
        if (ci != 0)
        {
          s += "-c";
          s += to_string (ci);
        }

        return normalize (path (move (s)), temp_dir (env), ll);
      };

      // If this is the first pipeline command, then open stdin descriptor
      // according to the redirect specified.
      //
      path isp;

      if (!first)
        assert (!c.in); // No redirect expected.
      else
      {
        // Open a file for passing to the command stdin.
        //
        auto open_stdin = [&isp, &ifd, &ll] ()
        {
          assert (!isp.empty ());

          try
          {
            ifd = fdopen (isp, fdopen_mode::in);
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to read " << isp << ": " << e;
          }
        };

        switch (in.type)
        {
        case redirect_type::pass:
          {
            try
            {
              ifd = fddup (0);
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to duplicate stdin: " << e;
            }

            break;
          }
        case redirect_type::none:
          // Somehow need to make sure that the child process doesn't read
          // from stdin. That is tricky to do in a portable way. Here we
          // suppose that the program which (erroneously) tries to read some
          // data from stdin being redirected to /dev/null fails not being
          // able to read the expected data, and so the command doesn't pass
          // through.
          //
          // @@ Obviously doesn't cover the case when the process reads
          //    whatever available.
          // @@ Another approach could be not to redirect stdin and let the
          //    process to hang which can be interpreted as a command failure.
          // @@ Both ways are quite ugly. Is there some better way to do
          //    this?
          // @@ Maybe we can create a pipe, write a byte into it, close the
          //    writing end, and after the process terminates make sure we can
          //    still read this byte out?
          //
          // Fall through.
          //
        case redirect_type::null:
          {
            ifd = open_null ();
            break;
          }
        case redirect_type::file:
          {
            isp = normalize (in.file.path, wdir, ll);

            open_stdin ();
            break;
          }
        case redirect_type::here_str_literal:
        case redirect_type::here_doc_literal:
          {
            // We could write to the command stdin directly but instead will
            // cache the data for potential troubleshooting.
            //
            isp = std_path ("stdin");

            save (isp,
                  transform (in.str, false /* regex */, in.modifiers (), env),
                  ll);

            env.clean_special (isp);

            open_stdin ();
            break;
          }
        case redirect_type::trace:
        case redirect_type::merge:
        case redirect_type::here_str_regex:
        case redirect_type::here_doc_regex:
        case redirect_type::here_doc_ref:   assert (false); break;
        }
      }

      assert (ifd.get () != -1);

      // Calculate the process/builtin execution deadline. Note that we should
      // also consider the left-hand side processes deadlines, not to keep
      // them waiting for us and allow them to terminate not later than their
      // deadlines.
      //
      dl = earlier (dl, env.effective_deadline ());

      if (c.timeout)
      {
        deadline d (system_clock::now () + *c.timeout, c.timeout_success);
        if (!dl || d < *dl)
          dl = d;
      }

      // Prior to opening file descriptors for command outputs redirects
      // let's check if the command is the set builtin. Being a builtin
      // syntactically it differs from the regular ones in a number of ways.
      // It either succeeds or terminates abnormally, so redirecting stderr
      // is meaningless. It also never produces any output and may appear
      // only as a terminal command in a pipeline. That means we can
      // short-circuit here calling the builtin and returning right after
      // that. Checking that the user didn't specify any meaningless
      // redirects or exit code check sounds as a right thing to do.
      //
      if (resolve && program == "set")
      {
        if (!last)
          fail (ll) << "set builtin must be the last pipe command";

        if (c.out)
          fail (ll) << "set builtin stdout cannot be redirected";

        if (cf != nullptr && !last_cmd)
          fail (ll) << "set builtin stdout cannot be read";

        if (c.err)
          fail (ll) << "set builtin stderr cannot be redirected";

        if (c.exit)
          fail (ll) << "set builtin exit code cannot be checked";

        if (verb >= 2)
          print_process (process_args ());

        set_builtin (env, c.arguments, move (ifd), prev_cmd, dl, ll);
        return true;
      }

      // If this is the last command in the pipe and the command function is
      // specified for it, then call it.
      //
      if (last && cf != nullptr && last_cmd)
      {
        // Must be enforced by the caller.
        //
        assert (!c.out && !c.err && !c.exit);

        try
        {
          cf (env, c.arguments, move (ifd), prev_cmd, dl, ll);
        }
        catch (const io_error& e)
        {
          diag_record dr (fail (ll));

          dr << cmd_path (c) << ": unable to read from ";

          if (prev_cmd != nullptr)
            dr << cmd_path (prev_cmd->cmd) << " output";
          else
            dr << "stdin";

          dr << ": " << e;
        }

        return true;
      }

      // Propagate the pointer to the left-most command.
      //
      pipe_command pc (env.context,
                       c,
                       dl,
                       ll,
                       prev_cmd,
                       prev_cmd != nullptr ? prev_cmd->next : nullptr);

      if (prev_cmd != nullptr)
        prev_cmd->next = &pc;
      else
        pc.next = &pc; // Points to itself.

      // Open a file for command output redirect if requested explicitly
      // (file overwrite/append redirects) or for the purpose of the output
      // validation (none, here_*, file comparison redirects), register the
      // file for cleanup, return the file descriptor. Interpret trace
      // redirect according to the verbosity level (as null if below 2, as
      // pass otherwise). Return nullfd, standard stream descriptor duplicate
      // or null-device descriptor for merge, pass or null redirects
      // respectively (not opening any file).
      //
      auto open = [&env, &wdir, &ll, &std_path, &c, &pc] (const redirect& r,
                                                          int dfd,
                                                          path& p) -> auto_fd
      {
        assert (dfd == 1 || dfd == 2);
        const char* what (dfd == 1 ? "stdout" : "stderr");

        fdopen_mode m (fdopen_mode::out | fdopen_mode::create);

        redirect_type rt (r.type != redirect_type::trace
                          ? r.type
                          : verb < 2
                          ? redirect_type::null
                          : redirect_type::pass);
        switch (rt)
        {
        case redirect_type::pass:
          {
            try
            {
              if (dfd == 2) // stderr?
              {
                fdpipe p;
                if (diag_buffer::pipe (env.context) == -1) // Are we buffering?
                  p = fdopen_pipe ();

                // Deduce the args0 argument similar to cmd_path().
                //
                // Note that we must open the diag buffer regardless of the
                // diag_buffer::pipe() result.
                //
                pc.dbuf.open ((c.program.initial == nullptr
                               ? c.program.recall.string ().c_str ()
                               : c.program.recall_string ()),
                              move (p.in),
                              fdstream_mode::non_blocking);

                if (p.out != nullfd)
                  return move (p.out);

                // Fall through.
              }

              return fddup (dfd);
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to redirect " << what << ": " << e;
            }
          }

        case redirect_type::null: return open_null ();

          // Duplicate the paired file descriptor later.
          //
        case redirect_type::merge: return nullfd;

        case redirect_type::file:
          {
            // For the cmp mode the user-provided path refers a content to
            // match against, rather than a content to be produced (as for
            // overwrite and append modes). And so for cmp mode we redirect
            // the process output to a temporary file.
            //
            p = r.file.mode == redirect_fmode::compare
              ? std_path (what)
              : normalize (r.file.path, wdir, ll);

            m |= r.file.mode == redirect_fmode::append
              ? fdopen_mode::at_end
              : fdopen_mode::truncate;

            break;
          }

        case redirect_type::none:
        case redirect_type::here_str_literal:
        case redirect_type::here_doc_literal:
        case redirect_type::here_str_regex:
        case redirect_type::here_doc_regex:
          {
            p = std_path (what);
            m |= fdopen_mode::truncate;
            break;
          }

        case redirect_type::trace:
        case redirect_type::here_doc_ref: assert (false); break;
        }

        auto_fd fd;

        try
        {
          fd = fdopen (p, m);

          if ((m & fdopen_mode::at_end) != fdopen_mode::at_end)
          {
            if (rt == redirect_type::file)
              env.clean ({cleanup_type::always, p}, true);
            else
              env.clean_special (p);
          }
        }
        catch (const io_error& e)
        {
          fail (ll) << "unable to write to " << p << ": " << e;
        }

        return fd;
      };

      path osp;
      fdpipe ofd;

      // If this is the last command in the pipeline than redirect the
      // command process stdout to a file. Otherwise create a pipe and
      // redirect the stdout to the write-end of the pipe. The read-end will
      // be passed as stdin for the next command in the pipeline.
      //
      // @@ Shouldn't we allow the here-* and file output redirects for a
      //    command with pipelined output? Say if such redirect is present
      //    then the process output is redirected to a file first (as it is
      //    when no output pipelined), and only after the process exit code
      //    and the output are validated the next command in the pipeline is
      //    executed taking the file as an input. This could be usefull for
      //    script failures investigation and, for example, for validation
      //    "tightening".
      //
      if (last && out != nullptr)
        ofd.out = open (*out, 1, osp);
      else
      {
        assert (!c.out); // No redirect expected.
        ofd = open_pipe ();
      }

      path esp;
      auto_fd efd (open (err, 2, esp));

      // Merge standard streams.
      //
      bool mo (out != nullptr && out->type == redirect_type::merge);
      bool me (err.type == redirect_type::merge);

      if (mo || me)
      {
        // Note that while the parser verifies that there is no stdout/stderr
        // mutual redirects specified on the command line, we can still end up
        // with mutual redirects here since one of such redirects can be
        // provided as a default by the script environment implementation
        // which the parser is not aware of at the time of parsing the command
        // line.
        //
        if (mo && me)
          fail (ll) << "stdout and stderr redirected to each other";

        auto_fd& self  (mo ? ofd.out : efd);
        auto_fd& other (mo ? efd     : ofd.out);

        try
        {
          assert (self.get () == -1 && other.get () != -1);
          self = fddup (other.get ());
        }
        catch (const io_error& e)
        {
          fail (ll) << "unable to duplicate " << (mo ? "stderr" : "stdout")
                    << ": " << e;
        }
      }

      // By now all descriptors should be open.
      //
      assert (ofd.out != nullfd && efd != nullfd);

      pc.isp = &isp;
      pc.osp = &osp;
      pc.esp = &esp;

      // Read out all the pipeline's buffered strerr streams watching for the
      // deadline, if specified. If the deadline is reached, then terminate
      // the whole pipeline, move the deadline by another 2 seconds, and
      // continue reading.
      //
      // Note that we assume that this timeout increment is normally
      // sufficient to read out the buffered data written by the already
      // terminated processes. If, however, that's not the case (see
      // pipe_command for the possible reasons), then we just set
      // unread_stderr flag to true for such commands and bail out.
      //
      // Also note that this is a reduced version of the above read() function.
      //
      auto read_pipe = [&pc, &ll, &trace] ()
      {
        fdselect_set fds;
        for (pipe_command* c (&pc); c != nullptr; c = c->prev)
        {
          diag_buffer& b (c->dbuf);

          if (b.is.is_open ())
            fds.emplace_back (b.is.fd (), c);
        }

        // Note that the current command deadline is the earliest (see above).
        //
        optional<timestamp> dlt (pc.dl ? pc.dl->value : optional<timestamp> ());

        bool terminated (false);

        for (size_t unread (fds.size ()); unread != 0;)
        {
          try
          {
            // If a deadline is specified, then pass the timeout to fdselect().
            //
            if (dlt)
            {
              timestamp now (system_clock::now ());

              if (*dlt <= now || ifdselect (fds, *dlt - now) == 0)
              {
                if (!terminated)
                {
                  term_pipe (&pc, trace);
                  terminated = true;

                  dlt = system_clock::now () + chrono::seconds (2);
                  continue;
                }
                else
                {
                  for (fdselect_state& s: fds)
                  {
                    if (s.fd != nullfd)
                    {
                      pipe_command* c (static_cast<pipe_command*> (s.data));

                      c->unread_stderr = true;

                      // Let's also close the stderr stream not to confuse
                      // diag_buffer::close() (see read() for details).
                      //
                      try
                      {
                        c->dbuf.is.close ();
                      }
                      catch (const io_error&) {}
                    }
                  }

                  break;
                }
              }
            }
            else
              ifdselect (fds);

            for (fdselect_state& s: fds)
            {
              if (s.ready &&
                  !static_cast<pipe_command*> (s.data)->dbuf.read ())
              {
                s.fd = nullfd;
                --unread;
              }
            }
          }
          catch (const io_error& e)
          {
            fail (ll) << "io error reading pipeline streams: " << e;
          }
        }
      };

      // Wait for the pipeline processes and builtins to complete, watching
      // for their deadlines if present. If a deadline is reached for any of
      // them, then terminate the whole pipeline.
      //
      // Note: must be called after read_pipe().
      //
      auto wait_pipe = [&pc, &dl, &trace] ()
      {
        for (pipe_command* c (&pc); c != nullptr; c = c->prev)
        {
          try
          {
            if (process* p = c->proc)
            {
              if (!dl)
                p->wait ();
              else if (!timed_wait (*p, dl->value))
                term_pipe (c, trace);
            }
            else
            {
              builtin* b (c->bltn);

              if (!dl)
                b->wait ();
              else if (!timed_wait (*b, dl->value))
                term_pipe (c, trace);
            }
          }
          catch (const process_error& e)
          {
            fail (c->loc) << "unable to wait " << cmd_path (c->cmd) << ": "
                          << e;
          }
        }
      };

      // Iterate over the pipeline processes and builtins left to right,
      // printing their stderr if buffered and issuing the diagnostics if the
      // exit code is not available (terminated abnormally or due to a
      // deadline), is unexpected, or stdout and/or stderr was not fully
      // read. Throw failed at the end if the exit code for any of them is not
      // available or stdout and/or stderr was not fully read. Return false if
      // exit code for any of them is unexpected (the return is used, for
      // example, in the if-conditions).
      //
      // Note: must be called after wait_pipe() and only once.
      //
      auto complete_pipe = [&pc, &env, diag] ()
      {
        bool r (true);
        bool fail (false);

        pipe_command* c (pc.next); // Left-most command.
        assert (c != nullptr);     // Since the lambda must be called once.

        for (pc.next = nullptr; c != nullptr; c = c->next)
        {
          // Collect the exit status, if present.
          //
          // Absent if the process/builtin misses the "unsuccessful" deadline.
          //
          optional<process_exit> exit;

          const char* w (c->bltn != nullptr ? "builtin" : "process");

          if (c->bltn != nullptr)
          {
            // Note that this also handles ad hoc termination (without the
            // call to term_pipe()) by the sleep builtin.
            //
            if (c->terminated)
            {
              if (c->dl && c->dl->success)
                exit = process_exit (0);
            }
            else
              exit = process_exit (c->bltn->wait ());

            c->bltn = nullptr;
          }
          else if (c->proc != nullptr)
          {
            const process& pr (*c->proc);

#ifndef _WIN32
            if (c->terminated       &&
                !pr.exit->normal () &&
                pr.exit->signal () == SIGTERM)
#else
            if (c->terminated       &&
                !pr.exit->normal () &&
                pr.exit->status == DBG_TERMINATE_PROCESS)
#endif
            {
              if (c->dl && c->dl->success)
                exit = process_exit (0);
            }
            else
              exit = pr.exit;

            c->proc = nullptr;
          }
          else
            assert (false); // The lambda can only be called once.

          const command& cmd (c->cmd);
          const location& ll (c->loc);

          // Verify the exit status and issue the diagnostics on failure.
          //
          diag_record dr;

          path pr (cmd_path (cmd));

          // Print the diagnostics if the command stdout and/or stderr are not
          // fully read.
          //
          auto unread_output_diag = [&dr, c, w, &pr] (bool main_error)
          {
            if (main_error)
              dr << error (c->loc) << w << ' ' << pr << ' ';
            else
              dr << error;

            if (c->unread_stdout)
            {
              dr << "stdout ";

              if (c->unread_stderr)
                dr << "and ";
            }

            if (c->unread_stderr)
              dr << "stderr ";

            dr << "not closed after exit";
          };

          // Fail if the process is terminated due to reaching the deadline.
          //
          if (!exit)
          {
            dr << error (ll) << w << ' ' << pr
               << " terminated: execution timeout expired";

            if (c->unread_stdout || c->unread_stderr)
              unread_output_diag (false /* main_error */);

            if (verb == 1)
            {
              dr << info << "command line: ";
              print_process (dr, *c->args);
            }

            fail = true;
          }
          else
          {
            // If there is no valid exit code available by whatever reason
            // then we print the proper diagnostics, dump stderr (if cached
            // and not too large) and fail the whole script. Otherwise if the
            // exit code is not correct then we print diagnostics if requested
            // and fail the pipeline.
            //
            bool valid (exit->normal ());

            // On Windows the exit code can be out of the valid codes range
            // being defined as uint16_t.
            //
#ifdef _WIN32
            if (valid)
              valid = exit->code () < 256;
#endif

            // In the presense of a valid exit code and given stdout and
            // stderr are fully read out we print the diagnostics and return
            // false rather than throw.
            //
            // Note that there can be a race, so that the process we have
            // terminated due to reaching the deadline has in fact exited
            // normally. Thus, the 'unread stderr' situation can also happen
            // to a successfully terminated process. If that's the case, we
            // report this problem as the main error and the secondary error
            // otherwise.
            //
            if (!valid || c->unread_stdout || c->unread_stderr)
              fail = true;

            exit_comparison cmp (cmd.exit
                                 ? cmd.exit->comparison
                                 : exit_comparison::eq);

            uint16_t exc (cmd.exit ? cmd.exit->code : 0);

            bool success (valid &&
                          (cmp == exit_comparison::eq) ==
                          (exc == exit->code ()));

            if (!success)
              r = false;

            if (!valid || (!success && diag))
            {
              dr << error (ll) << w << ' ' << pr << ' ';

              if (!exit->normal ())
                dr << *exit;
              else
              {
                uint16_t ec (exit->code ()); // Make sure printed as integer.

                if (!valid)
                {
                  dr << "exit code " << ec << " out of 0-255 range";
                }
                else
                {
                  if (cmd.exit)
                    dr << "exit code " << ec
                       << (cmp == exit_comparison::eq ? " != " : " == ")
                       << exc;
                  else
                    dr << "exited with code " << ec;
                }
              }

              if (c->unread_stdout || c->unread_stderr)
                unread_output_diag (false /* main_error */);

              if (verb == 1)
              {
                dr << info << "command line: ";
                print_process (dr, *c->args);
              }

              if (non_empty (*c->esp, ll) && avail_on_failure (*c->esp, env))
                dr << info << "stderr: " << *c->esp;

              if (non_empty (*c->osp, ll) && avail_on_failure (*c->osp, env))
                dr << info << "stdout: " << *c->osp;

              if (non_empty (*c->isp, ll) && avail_on_failure (*c->isp, env))
                dr << info << "stdin: " << *c->isp;

              // Print cached stderr.
              //
              print_file (dr, *c->esp, ll);
            }
            else if (c->unread_stdout || c->unread_stderr)
              unread_output_diag (true /* main_error */);
          }

          // Now print the buffered stderr, if present, and/or flush the
          // diagnostics, if issued.
          //
          if (c->dbuf.is_open ())
            c->dbuf.close (move (dr));
        }

        // Fail if required.
        //
        if (fail)
          throw failed ();

        return r;
      };

      // Close all buffered pipeline stderr streams ignoring io_error
      // exceptions.
      //
      auto close_pipe = [&pc] ()
      {
        for (pipe_command* c (&pc); c != nullptr; c = c->prev)
        {
          if (c->dbuf.is.is_open ())
          try
          {
            c->dbuf.is.close();
          }
          catch (const io_error&) {}
        }
      };

      // Derive the process/builtin CWD.
      //
      // If the process/builtin CWD is specified via the env pseudo-builtin,
      // then use that, completing it relative to the script environment work
      // directory, if it is relative. Otherwise, use the script environment
      // work directory.
      //
      dir_path completed_cwd;
      if (c.cwd && c.cwd->relative ())
        completed_cwd = wdir / *c.cwd;

      const dir_path& cwd (!completed_cwd.empty () ? completed_cwd :
                           c.cwd                   ? *c.cwd        :
                                                     wdir);

      // Unless CWD is the script environment work directory (which always
      // exists), verify that it exists and fail if it doesn't.
      //
      if (&cwd != &wdir && !exists (cwd))
        fail (ll) << "specified working directory " << cwd
                  << " does not exist";

      cstrings args (process_args ());
      pc.args = &args;

      const builtin_info* bi (resolve ? builtins.find (program) : nullptr);

      bool success;

      if (bi != nullptr && bi->function != nullptr)
      {
        // Execute the builtin.
        //
        // Don't print the true and false builtins, since they are normally
        // used for the commands execution flow control.
        //
        if (verb >= 2 && program != "true" && program != "false")
          print_process (args);

        // Some of the script builtins (cp, mkdir, etc) extend libbutl
        // builtins (via callbacks) registering/moving cleanups for the
        // filesystem entries they create/move, unless explicitly requested
        // not to do so via the --no-cleanup option.
        //
        // Let's "wrap up" the cleanup-related flags into the single object
        // to rely on "small function object" optimization.
        //
        struct cleanup
        {
          // Whether the cleanups are enabled for the builtin. Can be set to
          // false by the parse_option callback if --no-cleanup is
          // encountered.
          //
          bool enabled = true;

          // Whether to register cleanup for a filesystem entry being
          // created/updated depending on its existence. Calculated by the
          // create pre-hook and used by the subsequent post-hook.
          //
          bool add;

          // Whether to move existing cleanups for the filesystem entry
          // being moved, rather than to erase them. Calculated by the move
          // pre-hook and used by the subsequent post-hook.
          //
          bool move;
        };

        // nullopt if the builtin doesn't support cleanups.
        //
        optional<cleanup> cln;

        if (cleanup_builtin (program))
          cln = cleanup ();

        // We also extend the sleep builtin, deactivating the thread before
        // going to sleep and waking up before the deadline is reached.
        //
        builtin_callbacks bcs {

          // create
          //
          // Unless cleanups are suppressed, test that the filesystem entry
          // doesn't exist (pre-hook) and, if that's the case, register the
          // cleanup for the newly created filesystem entry (post-hook).
          //
          [&env, &cln] (const path& p, bool pre)
          {
            // Cleanups must be supported by a filesystem entry-creating
            // builtin.
            //
            assert (cln);

            if (cln->enabled)
            {
              if (pre)
                cln->add = !butl::entry_exists (p);
              else if (cln->add)
                env.clean ({cleanup_type::always, p}, true /* implicit */);
            }
          },

          // move
          //
          // Validate the source and destination paths (pre-hook) and,
          // unless suppressed, adjust the cleanups that are sub-paths of
          // the source path (post-hook).
          //
          [&env, &cln] (const path& from, const path& to, bool force, bool pre)
          {
            // Cleanups must be supported by a filesystem entry-moving
            // builtin.
            //
            assert (cln);

            if (pre)
            {
              const dir_path& wd (*env.work_dir.path);
              const dir_path* sd (env.sandbox_dir.path);

              auto fail = [] (const string& d) {throw runtime_error (d);};

              if (sd != nullptr && !from.sub (*sd) && !force)
                fail (diag_path (from) + " is out of " +
                      diag_path (env.sandbox_dir));

              auto check_wd = [&wd, &env, fail] (const path& p)
              {
                if (wd.sub (path_cast<dir_path> (p)))
                  fail (diag_path (p) + " contains " +
                        diag_path (env.work_dir));
              };

              check_wd (from);
              check_wd (to);

              // Unless cleanups are disabled, "move" the matching cleanups
              // if the destination path doesn't exist and it is a sub-path
              // of the working directory and just remove them otherwise.
              //
              if (cln->enabled)
                cln->move = !butl::entry_exists (to) &&
                            (sd == nullptr || to.sub (*sd));
            }
            else if (cln->enabled)
            {
              // Move or remove the matching cleanups (see above).
              //
              // Note that it's not enough to just change the cleanup paths.
              // We also need to make sure that these cleanups happen before
              // the destination directory (or any of its parents) cleanup,
              // that is potentially registered. To achieve that we can just
              // relocate these cleanup entries to the end of the list,
              // preserving their mutual order. Remember that cleanups in
              // the list are executed in the reversed order.
              //
              cleanups cs;

              // Remove the source path sub-path cleanups from the list,
              // adjusting/caching them if required (see above).
              //
              for (auto i (env.cleanups.begin ()); i != env.cleanups.end (); )
              {
                script::cleanup& c (*i);
                path& p (c.path);

                if (p.sub (from))
                {
                  if (cln->move)
                  {
                    // Note that we need to preserve the cleanup path
                    // trailing separator which indicates the removal
                    // method. Also note that leaf(), in particular, does
                    // that.
                    //
                    p = p != from
                      ? to / p.leaf (path_cast<dir_path> (from))
                      : p.to_directory ()
                        ? path_cast<dir_path> (to)
                        : to;

                    cs.push_back (move (c));
                  }

                  i = env.cleanups.erase (i);
                }
                else
                  ++i;
              }

              // Re-insert the adjusted cleanups at the end of the list.
              //
              env.cleanups.insert (env.cleanups.end (),
                                   make_move_iterator (cs.begin ()),
                                   make_move_iterator (cs.end ()));

            }
          },

          // remove
          //
          // Validate the filesystem entry path (pre-hook).
          //
          [&env] (const path& p, bool force, bool pre)
          {
            if (pre)
            {
              const dir_path& wd (*env.work_dir.path);
              const dir_path* sd (env.sandbox_dir.path);

              auto fail = [] (const string& d) {throw runtime_error (d);};

              if (sd != nullptr && !p.sub (*sd) && !force)
                fail (diag_path (p) + " is out of " +
                      diag_path (env.sandbox_dir));

              if (wd.sub (path_cast<dir_path> (p)))
                fail (diag_path (p) + " contains " +
                      diag_path (env.work_dir));
            }
          },

          // parse_option
          //
          [&cln] (const strings& args, size_t i)
          {
            // Parse --no-cleanup, if it is supported by the builtin.
            //
            if (cln && args[i] == "--no-cleanup")
            {
              cln->enabled = false;
              return 1;
            }

            return 0;
          },

          // sleep
          //
          [&env, &pc] (const duration& d)
          {
            duration t (d);
            const optional<timestamp>& dl (pc.dl
                                           ? pc.dl->value
                                           : optional<timestamp> ());

            if (dl)
            {
              timestamp now (system_clock::now ());

              if (now + t > *dl)
                pc.terminated = true;

              if (*dl <= now)
                return;

              duration d (*dl - now);
              if (t > d)
                t = d;
            }

            // If/when required we could probably support the precise sleep
            // mode (e.g., via an option).
            //
            env.context.sched->sleep (t);
          }
        };

        try
        {
          uint8_t r; // Storage.
          builtin b (bi->function (r,
                                   c.arguments,
                                   move (ifd), move (ofd.out), move (efd),
                                   cwd,
                                   bcs));
          pc.bltn = &b;

          // If the right-hand part of the pipe fails, then make sure we don't
          // wait indefinitely in the process destructor if the deadlines are
          // specified or just because a process is blocked on stderr.
          //
          auto g (make_exception_guard ([&pc, &close_pipe, &trace] ()
          {
            if (pc.bltn != nullptr)
            try
            {
              close_pipe ();
              term_pipe (&pc, trace);
            }
            catch (const failed&)
            {
              // We can't do much here.
            }
          }));

          success = run_pipe (env,
                              nc, ec,
                              move (ofd.in),
                              ii, li, ci + 1, ll, diag,
                              cf, last_cmd,
                              dl,
                              &pc);

          // Complete the pipeline execution, if not done yet.
          //
          if (pc.bltn != nullptr)
          {
            read_pipe ();
            wait_pipe ();

            if (!complete_pipe ())
              success = false;
          }
        }
        catch (const system_error& e)
        {
          fail (ll) << "unable to execute " << c.program << " builtin: "
                    << e << endf;
        }
      }
      else
      {
        // Execute the process.
        //
        // If the process path is not pre-searched then resolve the relative
        // non-simple program path against the script's working directory. The
        // simple one will be left for the process path search machinery. Also
        // strip the potential leading `^` (indicates that this is an external
        // program rather than a builtin).
        //
        path p;

        if (resolve)
        try
        {
          p = path (args[0]);

          if (p.relative ())
          {
            auto program = [&p, &args] (path pp)
            {
              p = move (pp);
              args[0] = p.string ().c_str ();
            };

            if (p.simple ())
            {
              const string& s (p.string ());

              // Don't end up with an empty path.
              //
              if (s.size () > 1 && s[0] == '^')
                program (path (s, 1, s.size () - 1));
            }
            else
              program (wdir / p);
          }
        }
        catch (const invalid_path& e)
        {
          fail (ll) << "invalid program path " << e.path;
        }

        try
        {
          process_path pp (resolve
                           ? process::path_search (args[0])
                           : process_path ());

          environment_vars vss;
          const environment_vars& vs (
            env.merge_exported_variables (c.variables, vss));

          // Note that CWD and builtin-escaping character '^' are not printed.
          //
          const small_vector<string, 4>& evars (vs);
          process_env pe (resolve ? pp : c.program, evars);

          if (verb >= 2)
            print_process (pe, args);

          // Note that stderr can only be a pipe if we are buffering the
          // diagnostics. In this case also pass the reading end so it can be
          // "probed" on Windows (see butl::process::pipe for details).
          //
          process pr (
            *pe.path,
            args.data (),
            {ifd.get (), -1},
            process::pipe (ofd),
            {pc.dbuf.is.fd (), efd.get ()},
            cwd.string ().c_str (),
            pe.vars);

          // Can't throw.
          //
          ifd.reset ();
          ofd.out.reset ();
          efd.reset ();

          pc.proc = &pr;

          // If the right-hand part of the pipe fails, then make sure we don't
          // wait indefinitely in the process destructor (see above for
          // details).
          //
          auto g (make_exception_guard ([&pc, &close_pipe, &trace] ()
          {
            if (pc.proc != nullptr)
            try
            {
              close_pipe ();
              term_pipe (&pc, trace);
            }
            catch (const failed&)
            {
              // We can't do much here.
            }
          }));

          success = run_pipe (env,
                              nc, ec,
                              move (ofd.in),
                              ii, li, ci + 1, ll, diag,
                              cf, last_cmd,
                              dl,
                              &pc);

          // Complete the pipeline execution, if not done yet.
          //
          if (pc.proc != nullptr)
          {
            read_pipe ();
            wait_pipe ();

            if (!complete_pipe ())
              success = false;
          }
        }
        catch (const process_error& e)
        {
          error (ll) << "unable to execute " << args[0] << ": " << e;

          if (e.child)
            std::exit (1);

          throw failed ();
        }
      }

      // If the pipeline or the righ-hand side outputs check failed, then no
      // further checks are required. Otherwise, check if the standard outputs
      // match the expectations. Note that stdout can only be redirected to
      // file for the last command in the pipeline.
      //
      // The thinking behind matching stderr first is that if it mismatches,
      // then the program probably misbehaves (executes wrong functionality,
      // etc) in which case its stdout doesn't really matter.
      //
      if (success)
      {
        path pr (cmd_path (c));

        success = check_output (pr, esp, isp, err, ll, env, diag, "stderr") &&
                  (out == nullptr ||
                   check_output (pr, osp, isp, *out, ll, env, diag, "stdout"));
      }

      return success;
    }

    static bool
    run_expr (environment& env,
              const command_expr& expr,
              const iteration_index* ii, size_t li,
              const location& ll,
              bool diag,
              const function<command_function>& cf, bool last_cmd)
    {
      // Commands are numbered sequentially throughout the expression
      // starting with 1. Number 0 means the command is a single one.
      //
      size_t ci (expr.size () == 1 && expr.back ().pipe.size () == 1
                 ? 0
                 : 1);

      // If there is no ORs to the right of a pipe then the pipe failure is
      // fatal for the whole expression. In particular, the pipe must print
      // the diagnostics on failure (if generally allowed). So we find the
      // pipe that "switches on" the diagnostics potential printing.
      //
      command_expr::const_iterator trailing_ands; // Undefined if diag is
                                                  // disallowed.
      if (diag)
      {
        auto i (expr.crbegin ());
        for (; i != expr.crend () && i->op == expr_operator::log_and; ++i) ;
        trailing_ands = i.base ();
      }

      bool r (false);
      bool print (false);

      for (auto b (expr.cbegin ()), i (b), e (expr.cend ()); i != e; ++i)
      {
        if (diag && i + 1 == trailing_ands)
          print = true;

        const command_pipe& p (i->pipe);
        bool or_op (i->op == expr_operator::log_or);

        // Short-circuit if the pipe result must be OR-ed with true or AND-ed
        // with false.
        //
        if (!((or_op && r) || (!or_op && !r)))
        {
          assert (!p.empty ());

          r = run_pipe (env,
                        p.begin (), p.end (),
                        auto_fd (),
                        ii, li, ci, ll, print,
                        cf, last_cmd);
        }

        ci += p.size ();
      }

      return r;
    }

    void
    run (environment& env,
         const command_expr& expr,
         const iteration_index* ii, size_t li,
         const location& ll,
         const function<command_function>& cf,
         bool last_cmd)
    {
      // Note that we don't print the expression at any verbosity level
      // assuming that the caller does this, potentially providing some
      // additional information (command type, etc).
      //
      if (!run_expr (env,
                     expr,
                     ii, li, ll,
                     true /* diag */,
                     cf, last_cmd))
        throw failed (); // Assume diagnostics is already printed.
    }

    bool
    run_cond (environment& env,
              const command_expr& expr,
              const iteration_index* ii, size_t li,
              const location& ll,
              const function<command_function>& cf, bool last_cmd)
    {
      // Note that we don't print the expression here (see above).
      //
      return run_expr (env,
                       expr,
                       ii, li, ll,
                       false /* diag */,
                       cf, last_cmd);
    }

    void
    clean (environment& env, const location& ll)
    {
      // We don't use the build2 filesystem utilities here in order to remove
      // the filesystem entries regardless of the dry-run mode and also to add
      // the location info to diagnostics. Other than that, these lambdas
      // implement the respective utility functions semantics.
      //
      auto rmfile = [&ll] (const path& f)
      {
        try
        {
          rmfile_status r (try_rmfile (f));

          if (r == rmfile_status::success && verb >= 3)
            text << "rm " << f;

          return r;
        }
        catch (const system_error& e)
        {
          fail (ll) << "unable to remove file " << f << ": " << e << endf;
        }
      };

      auto rmdir = [&ll] (const dir_path& d)
      {
        try
        {
          rmdir_status r (!work.sub (d)
                          ? try_rmdir (d)
                          : rmdir_status::not_empty);

          if (r == rmdir_status::success && verb >= 3)
            text << "rmdir " << d;

          return r;
        }
        catch (const system_error& e)
        {
          fail (ll) << "unable to remove directory " << d << ": " << e << endf;
        }
      };

      auto rmdir_r = [&ll] (const dir_path& d, bool dir)
      {
        if (work.sub (d)) // Don't try to remove working directory.
          return rmdir_status::not_empty;

        if (!build2::entry_exists (d))
          return rmdir_status::not_exist;

        try
        {
          butl::rmdir_r (d, dir);
        }
        catch (const system_error& e)
        {
          fail (ll) << "unable to remove directory " << d << ": " << e << endf;
        }

        if (verb >= 3)
          text << "rmdir -r " << d;

        return rmdir_status::success;
      };

      const dir_path& wdir (*env.work_dir.path);

      // Note that we operate with normalized paths here.
      //
      // Remove special files. The order is not important as we don't expect
      // directories here.
      //
      for (const path& p: env.special_cleanups)
      {
        // Remove the file if exists. Fail otherwise.
        //
        if (rmfile (p) == rmfile_status::not_exist)
          fail (ll) << "registered for cleanup special file " << p
                    << " does not exist";
      }

      // Remove files and directories in the order opposite to the order of
      // cleanup registration.
      //
      for (const auto& c: reverse_iterate (env.cleanups))
      {
        cleanup_type t (c.type);

        // Skip whenever the path exists or not.
        //
        if (t == cleanup_type::never)
          continue;

        const path& cp (c.path);

        // Wildcard with the last component being '***' (without trailing
        // separator) matches all files and sub-directories recursively as
        // well as the start directories itself. So we will recursively remove
        // the directories that match the parent (for the original path)
        // directory wildcard.
        //
        bool recursive (cp.leaf ().representation () == "***");
        const path& p (!recursive ? cp : cp.directory ());

        // Remove files or directories using wildcard.
        //
        if (path_pattern (p))
        {
          bool removed (false);

          auto rm = [&cp,
                     recursive,
                     &removed,
                     &ll,
                     &wdir,
                     &rmfile, &rmdir, &rmdir_r]
                    (path&& pe, const string&, bool interm)
          {
            if (!interm)
            {
              // While removing the entry we can get not_exist due to racing
              // conditions, but that's ok if somebody did our job. Note that
              // we still set the removed flag to true in this case.
              //
              removed = true; // Will be meaningless on failure.

              if (pe.to_directory ())
              {
                dir_path d (path_cast<dir_path> (pe));

                if (!recursive)
                {
                  rmdir_status r (rmdir (d));

                  if (r != rmdir_status::not_empty)
                    return true;

                  diag_record dr (fail (ll));
                  dr << "registered for cleanup directory " << d
                     << " is not empty";

                  print_dir (dr, d, ll);
                  dr << info << "wildcard: '" << cp << "'";
                }
                else
                {
                  // Don't remove the working directory (it will be removed by
                  // the dedicated cleanup).
                  //
                  rmdir_status r (rmdir_r (d, d != wdir));

                  if (r != rmdir_status::not_empty)
                    return true;

                  // The directory is unlikely to be current but let's keep
                  // for completeness.
                  //
                  fail (ll) << "registered for cleanup wildcard " << cp
                            << " matches the current directory";
                }
              }
              else
                rmfile (pe);
            }

            return true;
          };

          // Note that here we rely on the fact that recursive iterating goes
          // depth-first (which make sense for the cleanup).
          //
          try
          {
            // Doesn't follow symlinks.
            //
            path_search (p,
                         rm,
                         dir_path () /* start */,
                         path_match_flags::none);
          }
          catch (const system_error& e)
          {
            fail (ll) << "unable to cleanup wildcard " << cp << ": " << e;
          }

          // Removal of no filesystem entries is not an error for 'maybe'
          // cleanup type.
          //
          if (removed || t == cleanup_type::maybe)
            continue;

          fail (ll) << "registered for cleanup wildcard " << cp
                    << " doesn't match any "
                    << (recursive
                        ? "path"
                        : p.to_directory ()
                        ? "directory"
                        : "file");
        }

        // Remove the directory if exists and empty. Fail otherwise. Removal
        // of non-existing directory is not an error for 'maybe' cleanup type.
        //
        if (p.to_directory ())
        {
          dir_path d (path_cast<dir_path> (p));
          bool wd (d == wdir);

          // Don't remove the working directory for the recursive cleanup
          // since it needs to be removed by the caller (can contain
          // .buildignore file, etc).
          //
          rmdir_status r (recursive ? rmdir_r (d, !wd) : rmdir (d));

          if (r == rmdir_status::success ||
              (r == rmdir_status::not_exist && t == cleanup_type::maybe))
            continue;

          diag_record dr (fail (ll));
          dr << "registered for cleanup directory " << d
             << (r == rmdir_status::not_exist ? " does not exist" :
                 !recursive                   ? " is not empty"
                                              : " is current");

          if (r == rmdir_status::not_empty)
            print_dir (dr, d, ll);
        }

        // Remove the file if exists. Fail otherwise. Removal of non-existing
        // file is not an error for 'maybe' cleanup type.
        //
        if (rmfile (p) == rmfile_status::not_exist &&
            t == cleanup_type::always)
          fail (ll) << "registered for cleanup file " << p
                    << " does not exist";
      }
    }

    void
    print_dir (diag_record& dr, const dir_path& p, const location& ll)
    {
      try
      {
        size_t n (0);
        for (const dir_entry& de: dir_iterator (p, dir_iterator::no_follow))
        {
          if (n++ < 10)
            dr << '\n' << (de.ltype () == entry_type::directory
                           ? path_cast<dir_path> (de.path ())
                           : de.path ());
        }

        if (n > 10)
          dr << "\nand " << n - 10 << " more file(s)";
      }
      catch (const system_error& e)
      {
        fail (ll) << "unable to iterate over " << p << ": " << e;
      }
    }
  }
}
