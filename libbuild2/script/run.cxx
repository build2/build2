// file      : libbuild2/script/run.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/script/run.hxx>

#ifndef _WIN32
#  include <signal.h>                  // SIG*
#else
#  include <libbutl/win32-utility.hxx> // DBG_TERMINATE_PROCESS
#endif

#include <ios> // streamsize

#include <libbutl/regex.mxx>
#include <libbutl/builtin.mxx>
#include <libbutl/fdstream.mxx>     // fdopen_mode, fddup()
#include <libbutl/filesystem.mxx>   // path_search()
#include <libbutl/path-pattern.mxx>

#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/script/regex.hxx>
#include <libbuild2/script/timeout.hxx>
#include <libbuild2/script/builtin-options.hxx>

using namespace std;
using namespace butl;

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
        // it have been saved to.
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

        // Create line regex.
        //
        line_regex regex;

        try
        {
          regex = line_regex (move (rls), move (pool));
        }
        catch (const regex_error& e)
        {
          // Note that line regex creation can not fail for here-string
          // redirect as it doesn't have syntax line chars. That in
          // particular means that end_line and end_column are meaningful.
          //
          assert (rd.type == redirect_type::here_doc_regex);

          diag_record d (fail (loc (rd.end_line, rd.end_column)));

          // Print regex_error description if meaningful.
          //
          d << "invalid " << what << " regex redirect" << e;

          // It would be a waste to save the regex into the file just to
          // remove it.
          //
          if (env.temp_dir_keep)
            output_info (d, save_regex (), "", " regex");
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

            ls += line_char (move (s), regex.pool);
          }
        }
        catch (const io_error& e)
        {
          fail (ll) << "unable to read " << op << ": " << e;
        }

        // Match the output with the regex.
        //
        if (regex_match (ls, regex)) // Doesn't throw.
          return true;

        // Output doesn't match the regex.
        //
        // Unless the temporary directory is removed on failure, we save the
        // regex to file for troubleshooting regardless of whether we print
        // the diagnostics or not. We, however, register it for cleanup in the
        // later case (the expression may still succeed, we can be evaluating
        // the if condition, etc).
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

    // The timeout pseudo-builtin: set the script timeout. See the script-
    // specific set_timeout() implementations for the exact semantics.
    //
    // timeout [--success|-s] <timeout>
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
          fail (ll) << "missing timeout";

        string a (scan.next ());

        if (scan.more ())
          fail (ll) << "unexpected argument '" << scan.next () << "'";

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
        fail (ll) << "unexpected argument '" << *i << "'";

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

    // The set pseudo-builtin: set variable from the stdin input.
    //
    // set [-e|--exact] [(-n|--newline)|(-w|--whitespace)] [<attr>] <var>
    //
    static void
    set_builtin (environment& env,
                 const strings& args,
                 auto_fd in,
                 const optional<deadline>& dl,
                 const command& deadline_cmd,
                 const location& ll)
    {
      try
      {
        // Parse arguments.
        //
        cli::vector_scanner scan (args);
        set_options ops (scan);

        if (ops.whitespace () && ops.newline ())
          fail (ll) << "both -n|--newline and -w|--whitespace specified";

        if (!scan.more ())
          fail (ll) << "missing variable name";

        string a (scan.next ()); // Either attributes or variable name.
        const string* ats (!scan.more () ? nullptr : &a);
        string vname (!scan.more () ? move (a) : scan.next ());

        if (scan.more ())
          fail (ll) << "unexpected argument '" << scan.next () << "'";

        if (ats != nullptr && ats->empty ())
          fail (ll) << "empty variable attributes";

        if (vname.empty ())
          fail (ll) << "empty variable name";

        // Read out the stream content into a string while keeping an eye on
        // the deadline. Then parse it according to the split mode.
        //
        string s;
        {
          ifdstream cin;

          // If the execution deadline is specified, then turn the stream into
          // the non-blocking mode reading its content in chunks and with a
          // single operation otherwise. If the specified deadline is reached
          // while reading the stream, then bail out for the successful
          // deadline and fail otherwise. Note that in the former case the
          // variable value will be incomplete, but we leave it to the caller
          // to handle that.
          //
          if (dl)
          {
            fdselect_set fds {in.get ()};
            cin.open (move (in), fdstream_mode::non_blocking);

            const timestamp& dlt (dl->value);

            for (char buf[4096];; )
            {
              timestamp now (system_clock::now ());

              if (dlt <= now || ifdselect (fds, dlt - now) == 0)
              {
                if (!dl->success)
                  fail (ll) << cmd_path (deadline_cmd)
                            << " terminated: execution timeout expired";
                else
                  break;
              }

              streamsize n (cin.readsome (buf, sizeof (buf)));

              // Bail out if eos is reached.
              //
              if (n == 0)
                break;

              s.append (buf, n);
            }
          }
          else
          {
            cin.open (move (in));
            s = cin.read_text ();
          }

          cin.close ();
        }

        // Parse the stream content into the variable value.
        //
        names ns;

        if (!s.empty ())
        {
          if (ops.whitespace ()) // The whitespace mode.
          {
            // Note that we collapse multiple consecutive whitespaces.
            //
            for (size_t p (0); p != string::npos; )
            {
              // Skip the whitespaces.
              //
              const char* sep (" \n\r\t");
              size_t b (s.find_first_not_of (sep, p));

              if (b != string::npos) // Word beginning.
              {
                size_t e (s.find_first_of (sep, b)); // Find the word end.
                ns.emplace_back (string (s, b, e != string::npos ? e - b : e));

                p = e;
              }
              else // Trailings whitespaces.
              {
                // Append the trailing "blank" after the trailing whitespaces
                // in the exact mode.
                //
                if (ops.exact ())
                  ns.emplace_back (empty_string);

                // Bail out since the end of the string is reached.
                //
                break;
              }
            }
          }
          else // The newline or no-split mode.
          {
            // Note that we don't collapse multiple consecutive newlines.
            //
            // Note also that we always sanitize CRs so this loop is always
            // needed.
            //
            for (size_t p (0); p != string::npos; )
            {
              size_t e (s.find ('\n', p));
              string l (s, p, e != string::npos ? e - p : e);

              // Strip the trailing CRs that can appear while, for example,
              // cross-testing Windows target or as a part of msvcrt junk
              // production (see above).
              //
              while (!l.empty () && l.back () == '\r')
                l.pop_back ();

              // Append the line.
              //
              if (!l.empty ()       || // Non-empty.
                  e != string::npos || // Empty, non-trailing.
                  ops.exact ())        // Empty, trailing, in the exact mode.
              {
                if (ops.newline () || ns.empty ())
                  ns.emplace_back (move (l));
                else
                {
                  ns[0].value += '\n';
                  ns[0].value += l;
                }
              }

              p = e != string::npos ? e + 1 : e;
            }
          }
        }

        env.set_variable (move (vname),
                          move (ns),
                          ats != nullptr ? *ats : empty_string,
                          ll);
      }
      catch (const io_error& e)
      {
        fail (ll) << "set: " << e;
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

    // Stack-allocated linked list of information about the running pipeline
    // processes and builtins.
    //
    struct pipe_command
    {
      // We could probably use a union here, but let's keep it simple for now
      // (one is NULL).
      //
      process* proc;
      builtin* bltn;

      // True if this command has been terminated.
      //
      bool terminated = false;

      // Only for diagnostics.
      //
      const command& cmd;
      const location& loc;

      pipe_command* prev; // NULL for the left-most command.

      pipe_command (process& p,
                    const command& c,
                    const location& l,
                    pipe_command* v)
          : proc (&p), bltn (nullptr), cmd (c), loc (l), prev (v) {}

      pipe_command (builtin& b,
                    const command& c,
                    const location& l,
                    pipe_command* v)
          : proc (nullptr), bltn (&b), cmd (c), loc (l), prev (v) {}
    };

    static bool
    run_pipe (environment& env,
              command_pipe::const_iterator bc,
              command_pipe::const_iterator ec,
              auto_fd ifd,
              size_t ci, size_t li, const location& ll,
              bool diag,
              optional<deadline> dl = nullopt,
              const command* dl_cmd = nullptr, // env -t <cmd>
              pipe_command* prev_cmd = nullptr)
    {
      tracer trace ("script::run_pipe");

      if (bc == ec) // End of the pipeline.
        return true;

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

      // Register the command explicit cleanups. Verify that the path being
      // cleaned up is a sub-path of the script working directory. Fail if
      // this is not the case.
      //
      for (const auto& cl: c.cleanups)
      {
        const path& p (cl.path);
        path np (normalize (p, *env.work_dir.path, ll));

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

      // True if the process path is not pre-searched and the program path
      // still needs to be resolved.
      //
      bool resolve (c.program.initial == nullptr);

      // Program name that may require resolution.
      //
      const string& program (c.program.recall.string ());

      const redirect& in ((c.in ? *c.in : env.in).effective ());

      const redirect* out (!last
                           ? nullptr // stdout is piped.
                           : &(c.out ? *c.out : env.out).effective ());

      const redirect& err ((c.err ? *c.err : env.err).effective ());

      auto process_args = [&c] () -> cstrings
      {
        cstrings args {c.program.recall_string ()};

        for (const auto& a: c.arguments)
          args.push_back (a.c_str ());

        args.push_back (nullptr);
        return args;
      };

      // Prior to opening file descriptors for command input/output redirects
      // let's check if the command is the timeout or exit builtin. Being a
      // builtin syntactically they differ from the regular ones in a number
      // of ways. They don't communicate with standard streams, so redirecting
      // them is meaningless. They may appear only as a single command in a
      // pipeline. They don't return any value, so checking their exit status
      // is meaningless as well. That all means we can short-circuit here
      // calling the builtin and bailing out right after that. Checking that
      // the user didn't specify any variables, timeout, redirects, or exit
      // code check sounds like a right thing to do.
      //
      if (resolve && (program == "timeout" || program == "exit"))
      {
        // In case the builtin is erroneously pipelined from the other
        // command, we will close stdin gracefully (reading out the stream
        // content), to make sure that the command doesn't print any unwanted
        // diagnostics about IO operation failure.
        //
        // Note though, that doing so would be a bad idea if the deadline is
        // specified, since we can block on read and miss the deadline.
        //
        if (!dl)
        {
          // Note that dtor will ignore any errors (which is what we want).
          //
          ifdstream (move (ifd), fdstream_mode::skip);
        }

        if (!first || !last)
          fail (ll) << program << " builtin must be the only pipe command";

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

        if (c.err)
          fail (ll) << program << " builtin stderr cannot be redirected";

        if (c.exit)
          fail (ll) << program << " builtin exit code cannot be checked";

        if (verb >= 2)
          print_process (process_args ());

        if (program == "timeout")
        {
          timeout_builtin (env, c.arguments, ll);
          return true;
        }
        else if (program == "exit")
          exit_builtin (c.arguments, ll); // Throws exit exception.
        else
          assert (false);
      }

      // Create a unique path for a command standard stream cache file.
      //
      auto std_path = [&env, &ci, &li, &ll] (const char* n) -> path
      {
        using std::to_string;

        path p (n);

        // 0 if belongs to a single-line script, otherwise is the command line
        // number (start from one) in the script.
        //
        if (li > 0)
          p += "-" + to_string (li);

        // 0 if belongs to a single-command expression, otherwise is the
        // command number (start from one) in the expression.
        //
        // Note that the name like stdin-N can relate to N-th command of a
        // single-line script or to N-th single-command line of multi-line
        // script. These cases are mutually exclusive and so are unambiguous.
        //
        if (ci > 0)
          p += "-" + to_string (ci);

        return normalize (move (p), temp_dir (env), ll);
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
            isp = normalize (in.file.path, *env.work_dir.path, ll);

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
      // deadlines. Thus, let's also track which command has introduced the
      // deadline, so we can report it if the deadline is missed.
      //
      dl = earlier (dl, env.effective_deadline ());

      if (c.timeout)
      {
        deadline d (system_clock::now () + *c.timeout, false /* success */);
        if (!dl || d < *dl)
        {
          dl = d;
          dl_cmd = &c;
        }
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

        if (c.err)
          fail (ll) << "set builtin stderr cannot be redirected";

        if (c.exit)
          fail (ll) << "set builtin exit code cannot be checked";

        if (verb >= 2)
          print_process (process_args ());

        set_builtin (env, c.arguments, move (ifd),
                     dl, dl_cmd != nullptr ? *dl_cmd : c,
                     ll);

        return true;
      }

      // Open a file for command output redirect if requested explicitly
      // (file overwrite/append redirects) or for the purpose of the output
      // validation (none, here_*, file comparison redirects), register the
      // file for cleanup, return the file descriptor. Interpret trace
      // redirect according to the verbosity level (as null if below 2, as
      // pass otherwise). Return nullfd, standard stream descriptor duplicate
      // or null-device descriptor for merge, pass or null redirects
      // respectively (not opening any file).
      //
      auto open = [&env, &ll, &std_path] (const redirect& r,
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
              return fddup (dfd);
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to duplicate " << what << ": " << e;
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
              : normalize (r.file.path, *env.work_dir.path, ll);

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
      if (last)
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
      if (mo || err.type == redirect_type::merge)
      {
        auto_fd& self  (mo ? ofd.out : efd);
        auto_fd& other (mo ? efd : ofd.out);

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

      // All descriptors should be open to the date.
      //
      assert (ofd.out.get () != -1 && efd.get () != -1);

      // Wait for a process/builtin to complete until the deadline is reached
      // and return the underlying wait function result (optional<something>).
      //
      auto timed_wait = [] (auto& p, const timestamp& deadline)
      {
        timestamp now (system_clock::now ());
        return deadline > now ? p.timed_wait (deadline - now) : p.try_wait ();
      };

      // Terminate the pipeline processes starting from the specified one and
      // up to the leftmost one and then kill those which didn't terminate
      // after 1 second.
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
      auto term_pipe = [&timed_wait, &trace] (pipe_command* pc)
      {
        diag_record dr;

        auto prog = [] (pipe_command* c) {return cmd_path (c->cmd);};

        // Terminate processes gracefully and set the terminate flag for the
        // pipe commands.
        //
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
            // If unable to terminate the process for any reason (the process
            // is exiting on Windows, etc) then just ignore this, postponing
            // the potential failure till the kill() call.
            //
            l5 ([&]{trace (c->loc) <<"unable to terminate " << prog (c)
                                   << ": " << e;});
          }

          c->terminated = true;
        }

        // Wait a bit for the processes to terminate and kill the remaining
        // ones.
        //
        timestamp dl (system_clock::now () + chrono::seconds (1));

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
      };

      // Absent if the process/builtin misses the "unsuccessful" deadline.
      //
      optional<process_exit> exit;

      const builtin_info* bi (resolve ? builtins.find (program) : nullptr);

      bool success;

      if (bi != nullptr && bi->function != nullptr)
      {
        // Execute the builtin.
        //
        if (verb >= 2)
          print_process (process_args ());

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
        // Let's "wrap up" the sleep-related values into the single object to
        // rely on "small function object" optimization.
        //
        struct sleep
        {
          optional<timestamp> deadline;
          bool terminated = false;

          sleep (const optional<timestamp>& d): deadline (d) {}
        };
        sleep slp (dl ? dl->value : optional<timestamp> ());

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
          [&env, &slp] (const duration& d)
          {
            duration t (d);
            const optional<timestamp>& dl (slp.deadline);

            if (dl)
            {
              timestamp now (system_clock::now ());

              slp.terminated = now + t > *dl;

              if (*dl <= now)
                return;

              duration d (*dl - now);
              if (t > d)
                t = d;
            }

            // If/when required we could probably support the precise sleep
            // mode (e.g., via an option).
            //
            env.context.sched.sleep (t);
          }
        };

        try
        {
          uint8_t r; // Storage.
          builtin b (bi->function (r,
                                   c.arguments,
                                   move (ifd), move (ofd.out), move (efd),
                                   *env.work_dir.path,
                                   bcs));

          pipe_command pc (b, c, ll, prev_cmd);

          // If the deadline is specified, then make sure we don't miss it
          // waiting indefinitely in the builtin destructor on the right-hand
          // side of the pipe failure.
          //
          auto g (make_exception_guard ([&dl, &pc, &term_pipe] ()
          {
            if (dl)
            try
            {
              term_pipe (&pc);
            }
            catch (const failed&)
            {
              // We can't do much here.
            }
          }));

          success = run_pipe (env,
                              nc, ec,
                              move (ofd.in),
                              ci + 1, li, ll, diag,
                              dl, dl_cmd,
                              &pc);

          if (!dl)
            b.wait ();
          else if (!timed_wait (b, dl->value))
            term_pipe (&pc);

          // Note that this also handles ad hoc termination (without the call
          // to term_pipe()) by the sleep builtin (see above).
          //
          if (pc.terminated || slp.terminated)
          {
            assert (dl);

            if (dl->success)
              exit = process_exit (0);
          }
          else
            exit = process_exit (r);
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
        cstrings args (process_args ());

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
              program (*env.work_dir.path / p);
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

          // Note that CWD and builtin-escaping character '^' are not printed.
          //
          process_env pe (resolve ? pp : c.program, c.variables);

          if (verb >= 2)
            print_process (pe, args);

          process pr (
            *pe.path,
            args.data (),
            {ifd.get (), -1}, process::pipe (ofd), {-1, efd.get ()},
            env.work_dir.path->string ().c_str (),
            pe.vars);

          // Can't throw.
          //
          ifd.reset ();
          ofd.out.reset ();
          efd.reset ();

          pipe_command pc (pr, c, ll, prev_cmd);

          // If the deadline is specified, then make sure we don't miss it
          // waiting indefinitely in the process destructor on the right-hand
          // part of the pipe failure.
          //
          auto g (make_exception_guard ([&dl, &pc, &term_pipe] ()
          {
            if (dl)
            try
            {
              term_pipe (&pc);
            }
            catch (const failed&)
            {
              // We can't do much here.
            }
          }));

          success = run_pipe (env,
                              nc, ec,
                              move (ofd.in),
                              ci + 1, li, ll, diag,
                              dl, dl_cmd,
                              &pc);

          if (!dl)
            pr.wait ();
          else if (!timed_wait (pr, dl->value))
            term_pipe (&pc);

#ifndef _WIN32
          if (pc.terminated       &&
              !pr.exit->normal () &&
              pr.exit->signal () == SIGTERM)
#else
          if (pc.terminated       &&
              !pr.exit->normal () &&
              pr.exit->status == DBG_TERMINATE_PROCESS)
#endif
          {
            assert (dl);

            if (dl->success)
              exit = process_exit (0);
          }
          else
            exit = pr.exit;
        }
        catch (const process_error& e)
        {
          error (ll) << "unable to execute " << args[0] << ": " << e;

          if (e.child)
            std::exit (1);

          throw failed ();
        }
      }

      // If the righ-hand side pipeline failed than the whole pipeline fails,
      // and no further checks are required.
      //
      if (!success)
        return false;

      // Fail if the process is terminated due to reaching the deadline.
      //
      if (!exit)
        fail (ll) << cmd_path (dl_cmd != nullptr ? *dl_cmd : c)
                  << " terminated: execution timeout expired";

      path pr (cmd_path (c));

      // If there is no valid exit code available by whatever reason then we
      // print the proper diagnostics, dump stderr (if cached and not too
      // large) and fail the whole script. Otherwise if the exit code is not
      // correct then we print diagnostics if requested and fail the pipeline.
      //
      bool valid (exit->normal ());

      // On Windows the exit code can be out of the valid codes range being
      // defined as uint16_t.
      //
#ifdef _WIN32
      if (valid)
        valid = exit->code () < 256;
#endif

      exit_comparison cmp (c.exit ? c.exit->comparison : exit_comparison::eq);
      uint16_t        exc (c.exit ? c.exit->code       : 0);

      success = valid &&
                (cmp == exit_comparison::eq) == (exc == exit->code ());

      if (!valid || (!success && diag))
      {
        // In the presense of a valid exit code we print the diagnostics and
        // return false rather than throw.
        //
        diag_record d (valid ? error (ll) : fail (ll));

        if (!exit->normal ())
          d << pr << " " << *exit;
        else
        {
          uint16_t ec (exit->code ()); // Make sure is printed as integer.

          if (!valid)
            d << pr << " exit code " << ec << " out of 0-255 range";
          else if (!success)
          {
            if (diag)
            {
              if (c.exit)
                d << pr << " exit code " << ec
                  << (cmp == exit_comparison::eq ? " != " : " == ") << exc;
              else
                d << pr << " exited with code " << ec;
            }
          }
          else
            assert (false);
        }

        if (non_empty (esp, ll) && avail_on_failure (esp, env))
          d << info << "stderr: " << esp;

        if (non_empty (osp, ll) && avail_on_failure (osp, env))
          d << info << "stdout: " << osp;

        if (non_empty (isp, ll) && avail_on_failure (isp, env))
          d << info << "stdin: " << isp;

        // Print cached stderr.
        //
        print_file (d, esp, ll);
      }

      // If exit code is correct then check if the standard outputs match the
      // expectations. Note that stdout is only redirected to file for the
      // last command in the pipeline.
      //
      // The thinking behind matching stderr first is that if it mismatches,
      // then the program probably misbehaves (executes wrong functionality,
      // etc) in which case its stdout doesn't really matter.
      //
      if (success)
        success =
          check_output (pr, esp, isp, err, ll, env, diag, "stderr") &&
          (!last ||
           check_output (pr, osp, isp, *out, ll, env, diag, "stdout"));

      return success;
    }

    static bool
    run_expr (environment& env,
              const command_expr& expr,
              size_t li, const location& ll,
              bool diag)
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
          r = run_pipe (env,
                        p.begin (), p.end (),
                        auto_fd (),
                        ci, li, ll, print);

        ci += p.size ();
      }

      return r;
    }

    void
    run (environment& env,
         const command_expr& expr,
         size_t li, const location& ll)
    {
      // Note that we don't print the expression at any verbosity level
      // assuming that the caller does this, potentially providing some
      // additional information (command type, etc).
      //
      if (!run_expr (env, expr, li, ll, true /* diag */))
        throw failed (); // Assume diagnostics is already printed.
    }

    bool
    run_if (environment& env,
            const command_expr& expr,
            size_t li, const location& ll)
    {
      // Note that we don't print the expression here (see above).
      //
      return run_expr (env, expr, li, ll, false /* diag */);
    }

    void
    clean (environment& env, const location& ll)
    {
      context& ctx (env.context);
      const dir_path& wdir (*env.work_dir.path);

      // Note that we operate with normalized paths here.
      //
      // Remove special files. The order is not important as we don't
      // expect directories here.
      //
      for (const path& p: env.special_cleanups)
      {
        // Remove the file if exists. Fail otherwise.
        //
        if (rmfile (ctx, p, 3) == rmfile_status::not_exist)
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
        // well as the start directories itself. So we will recursively
        // remove the directories that match the parent (for the original
        // path) directory wildcard.
        //
        bool recursive (cp.leaf ().representation () == "***");
        const path& p (!recursive ? cp : cp.directory ());

        // Remove files or directories using wildcard.
        //
        if (path_pattern (p))
        {
          bool removed (false);

          auto rm = [&cp, recursive, &removed, &ll, &ctx, &wdir]
                    (path&& pe, const string&, bool interm)
          {
            if (!interm)
            {
              // While removing the entry we can get not_exist due to
              // racing conditions, but that's ok if somebody did our job.
              // Note that we still set the removed flag to true in this
              // case.
              //
              removed = true; // Will be meaningless on failure.

              if (pe.to_directory ())
              {
                dir_path d (path_cast<dir_path> (pe));

                if (!recursive)
                {
                  rmdir_status r (rmdir (ctx, d, 3));

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
                  // Don't remove the working directory (it will be removed
                  // by the dedicated cleanup).
                  //
                  // Cast to uint16_t to avoid ambiguity with
                  // libbutl::rmdir_r().
                  //
                  rmdir_status r (rmdir_r (ctx, d, d != wdir, 3));

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
                rmfile (ctx, pe, 3);
            }

            return true;
          };

          // Note that here we rely on the fact that recursive iterating
          // goes depth-first (which make sense for the cleanup).
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

        // Remove the directory if exists and empty. Fail otherwise.
        // Removal of non-existing directory is not an error for 'maybe'
        // cleanup type.
        //
        if (p.to_directory ())
        {
          dir_path d (path_cast<dir_path> (p));
          bool wd (d == wdir);

          // Don't remove the working directory for the recursive cleanup
          // (it will be removed by the dedicated one).
          //
          // Note that the root working directory contains the
          // .buildignore file (see above).
          //
          // @@ If 'd' is a file then will fail with a diagnostics having
          //    no location info. Probably need to add an optional location
          //    parameter to rmdir() function. The same problem exists for
          //    a file cleanup when try to rmfile() directory instead of
          //    file.
          //
          rmdir_status r (recursive
                          ? rmdir_r (ctx, d, !wd, static_cast <uint16_t> (3))
                          : rmdir (ctx, d, 3));

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

        // Remove the file if exists. Fail otherwise. Removal of
        // non-existing file is not an error for 'maybe' cleanup type.
        //
        if (rmfile (ctx, p, 3) == rmfile_status::not_exist &&
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
        for (const dir_entry& de: dir_iterator (p,
                                                false /* ignore_dangling */))
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
