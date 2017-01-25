// file      : build2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/runner>

#include <set>
#include <ios>     // streamsize
#include <cstring> // strstr()
#include <sstream>

#include <butl/fdstream> // fdopen_mode, fdnull(), fddup()

#include <build2/filesystem>

#include <build2/test/common>

#include <build2/test/script/regex>
#include <build2/test/script/builtin>

using namespace std;
using namespace butl;

namespace std
{
  // Print regex error description but only if it is meaningful (this is also
  // why we have to print leading colon here).
  //
  // Currently libstdc++ just returns the name of the exception (bug #67361).
  // So we check that the description contains at least one space character.
  //
  // While VC's description is meaningful, it has an undesired prefix that
  // resembles the following: 'regex_error(error_badrepeat): '. So we skip it.
  //
  static ostream&
  operator<< (ostream& o, const regex_error& e)
  {
    const char* d (e.what ());

#if defined(_MSC_VER) && _MSC_VER <= 1910
    const char* rd (strstr (d, "): "));
    if (rd != nullptr)
      d = rd + 3;
#endif

    ostringstream os;
    os << runtime_error (d); // Sanitize the description.

    string s (os.str ());
    if (s.find (' ') != string::npos)
      o << ": " << s;

    return o;
  }
}

namespace build2
{
  namespace test
  {
    namespace script
    {
      // Normalize a path. Also make the relative path absolute using the
      // scope's working directory unless it is already absolute.
      //
      static path
      normalize (path p, const scope& sp, const location& l)
      {
        path r (p.absolute () ? move (p) : sp.wd_path / move (p));

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
          // While there can be no fault of the test command being currently
          // executed let's add the location anyway to ease the
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
            ifdstream is (p, ifdstream::in, ifdstream::badbit);

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

                d << "\n" << buf;
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
          fail (ll) << "unable to write " << p << ": " << e;
        }
      }

      // Transform string according to here-* redirect modifiers from the {/}
      // set.
      //
      static string
      transform (const string& s,
                 bool regex,
                 const string& modifiers,
                 const script& scr)
      {
        if (modifiers.find ('/') == string::npos)
          return s;

        // For targets other than Windows leave the string intact.
        //
        if (cast<target_triplet> (scr.test_target["test.target"]).class_ !=
            "windows")
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

      // Check if the test command output matches the expected result (redirect
      // value). Noop for redirect types other than none, here_*.
      //
      static void
      check_output (const path& pr,
                    const path& op,
                    const path& ip,
                    const redirect& rd,
                    const location& ll,
                    scope& sp,
                    const char* what)
      {
        auto input_info = [&ip, &ll] (diag_record& d)
        {
          if (non_empty (ip, ll))
            d << info << "stdin: " << ip;
        };

        auto output_info = [&what, &ll] (diag_record& d,
                                         const path& p,
                                         const char* prefix = "",
                                         const char* suffix = "")
        {
          if (non_empty (p, ll))
            d << info << prefix << what << suffix << ": " << p;
          else
            d << info << prefix << what << suffix << " is empty";
        };

        if (rd.type == redirect_type::none)
        {
          assert (!op.empty ());

          // Check that there is no output produced.
          //
          if (non_empty (op, ll))
          {
            diag_record d (fail (ll));
            d << pr << " unexpectedly writes to " << what <<
              info << what << ": " << op;

            input_info (d);
          }
        }
        else if (rd.type == redirect_type::here_str_literal ||
                 rd.type == redirect_type::here_doc_literal ||
                 (rd.type == redirect_type::file &&
                  rd.file.mode == redirect_fmode::compare))
        {
          assert (!op.empty ());

          // The expected output is provided as a file or as a string. Save the
          // string to a file in the later case.
          //
          path eop;

          if (rd.type == redirect_type::file)
            eop = normalize (rd.file.path, sp, ll);
          else
          {
            eop = path (op + ".orig");
            save (eop, transform (rd.str, false, rd.modifiers, *sp.root), ll);
            sp.clean ({cleanup_type::always, eop}, true);
          }

          // Use diff utility for the comparison.
          //
          path dp ("diff");
          process_path pp (run_search (dp, true));

          cstrings args {pp.recall_string (), "-u"};

          // Ignore Windows newline fluff if that's what we are running on.
          //
          if (cast<target_triplet> (
                sp.root->test_target["test.target"]).class_ == "windows")
            args.push_back ("--strip-trailing-cr");

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
              sp.clean ({cleanup_type::always, ep}, true);
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to write " << ep << ": " << e;
            }

            // Diff utility prints the differences to stdout. But for the
            // user it is a part of the test failure diagnostics so let's
            // redirect stdout to stderr.
            //
            process p (pp, args.data (), 0, 2, efd.get ());
            efd.reset ();

            if (p.wait ())
              return;

            // Output doesn't match the expected result.
            //
            diag_record d (error (ll));
            d << pr << " " << what << " doesn't match the expected output";

            output_info (d, op);
            output_info (d, eop, "expected ");
            output_info (d, ep, "", " diff");
            input_info  (d);

            print_file (d, ep, ll);

            // Fall through.
            //
          }
          catch (const process_error& e)
          {
            error (ll) << "unable to execute " << pp << ": " << e;

            if (e.child ())
              exit (1);
          }

          throw failed ();
        }
        else if (rd.type == redirect_type::here_str_regex ||
                 rd.type == redirect_type::here_doc_regex)
        {
          assert (!op.empty ());

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
          auto line = [&rl, &rd, &sp] (const regex_line& l) -> string
          {
            string r;
            if (l.regex)                  // Regex (possibly empty),
            {
              r += rl.intro;
              r += transform (l.value, true, rd.modifiers, *sp.root);
              r += rl.intro;
              r += l.flags;
            }
            else if (!l.special.empty ()) // Special literal.
              r += rl.intro;
            else                          // Textual literal.
              r += transform (l.value, false, rd.modifiers, *sp.root);

            r += l.special;
            return r;
          };

          // Return regex line location.
          //
          // Note that we rely on the fact that the command and regex lines
          // are always belong to the same testscript file.
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
            // test/1/stdout.regex~di
            //
            if (rd.type == redirect_type::here_doc_regex &&
                !rl.flags.empty ())
              rp += "~" + rl.flags;

            // Note that if would be more efficient to directly write chunks
            // to file rather than to compose a string first. Hower we don't
            // bother (about performance) for the sake of the code as we
            // already failed.
            //
            string s;
            for (const auto& l: rl.lines)
            {
              if (!s.empty ()) s += '\n';
              s += line (l);
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
                  string s (transform (l.value, true, rd.modifiers, *sp.root));

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
              rls += line_char (
                transform (l.value, false, rd.modifiers, *sp.root), pool);
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
            // can mismatch when cross-test).
            //
            ifdstream is (op, ifdstream::in, ifdstream::badbit);
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
            return;

          // Output doesn't match the regex.
          //
          diag_record d (fail (ll));
          d << pr << " " << what << " doesn't match the regex";

          output_info (d, op);
          output_info (d, save_regex (), "", " regex");
          input_info  (d);
        }
      }

      bool default_runner::
      test (scope& s) const
      {
        return common_.test (s.root->test_target, s.id_path);
      }

      void default_runner::
      enter (scope& sp, const location&)
      {
        // Scope working directory shall be empty (the script working
        // directory is cleaned up by the test rule prior the script
        // execution).
        //
        // @@ Shouldn't we add an optional location parameter to mkdir() and
        // alike utility functions so the failure message can contain
        // location info?
        //
        if (mkdir (sp.wd_path, 2) == mkdir_status::already_exists)
          fail << "working directory " << sp.wd_path << " already exists" <<
            info << "are tests stomping on each other's feet?";

        // We don't change the current directory here but indicate that the
        // scope test commands will be executed in that directory.
        //
        if (verb >= 2)
          text << "cd " << sp.wd_path;

        sp.clean ({cleanup_type::always, sp.wd_path}, true);
      }

      void default_runner::
      leave (scope& sp, const location& ll)
      {
        // Remove files and directories in the order opposite to the order of
        // cleanup registration.
        //
        // Note that we operate with normalized paths here.
        //
        for (const auto& c: reverse_iterate (sp.cleanups))
        {
          cleanup_type t (c.type);

          // Skip whenever the path exists or not.
          //
          if (t == cleanup_type::never)
            continue;

          const path& p (c.path);

          // Remove the directory recursively if not current. Fail otherwise.
          // Recursive removal of non-existing directory is not an error for
          // 'maybe' cleanup type.
          //
          if (p.leaf ().string () == "***")
          {
            // Cast to uint16_t to avoid ambiguity with libbutl::rmdir_r().
            //
            rmdir_status r (
              rmdir_r (p.directory (), true, static_cast<uint16_t> (2)));

            if (r == rmdir_status::success ||
                (r == rmdir_status::not_exist && t == cleanup_type::maybe))
              continue;

            // The directory is unlikely to be current but let's keep for
            // completeness.
            //
            fail (ll) << "registered for cleanup wildcard " << p
                      << (r == rmdir_status::not_empty
                          ? " matches the current directory"
                          : " doesn't match a directory");
          }

          // Remove the directory if exists and empty. Fail otherwise. Removal
          // of non-existing directory is not an error for 'maybe' cleanup
          // type.
          //
          if (p.to_directory ())
          {
            dir_path d (path_cast<dir_path> (p));

            // @@ If 'd' is a file then will fail with a diagnostics having no
            //    location info. Probably need to add an optional location
            //    parameter to rmdir() function. The same problem exists for a
            //    file cleanup when try to rmfile() directory instead of file.
            //
            rmdir_status r (rmdir (d, 2));

            if (r == rmdir_status::success ||
                (r == rmdir_status::not_exist && t == cleanup_type::maybe))
              continue;

            fail (ll) << "registered for cleanup directory " << d
                      << (r == rmdir_status::not_empty
                          ? " is not empty"
                          : " does not exist");
          }

          // Remove the file if exists. Fail otherwise. Removal of non-existing
          // file is not an error for 'maybe' cleanup type.
          //
          if (rmfile (p, 2) == rmfile_status::not_exist &&
              t == cleanup_type::always)
            fail (ll) << "registered for cleanup file " << p
                      << " does not exist";
        }

        // Return to the parent scope directory or to the out_base one for the
        // script scope.
        //
        if (verb >= 2)
          text << "cd " << (sp.parent != nullptr
                            ? sp.parent->wd_path
                            : sp.wd_path.directory ());
      }

      void default_runner::
      run (scope& sp, const command_expr& expr, size_t li, const location& ll)
      {
        const command& c (expr.back ().pipe.back ()); // @@ TMP

        if (verb >= 3)
          text << c;

        // Register the command explicit cleanups. Verify that the path being
        // cleaned up is a sub-path of the testscript working directory. Fail
        // if this is not the case.
        //
        for (const auto& cl: c.cleanups)
        {
          const path& p (cl.path);
          path np (normalize (p, sp, ll));

          bool wc (np.leaf ().string () == "***");
          const path& cp (wc ? np.directory () : np);
          const dir_path& wd (sp.root->wd_path);

          if (!cp.sub (wd))
            fail (ll) << (wc
                          ? "wildcard"
                          : p.to_directory ()
                            ? "directory"
                            : "file")
                      << " cleanup " << p << " is out of working directory "
                      << wd;

          sp.clean ({cl.type, move (np)}, false);
        }

        // Create a unique path for a command standard stream cache file.
        //
        auto std_path = [&li, &sp, &ll] (const char* n) -> path
        {
          path p (n);

          // 0 if belongs to a single-line test scope, otherwise is the
          // command line number (start from one) in the test scope.
          //
          if (li > 0)
            p += "-" + to_string (li);

          return normalize (move (p), sp, ll);
        };

        // Assign file descriptors to pass as a builtin or a process standard
        // streams. Eventually the raw descriptors should gone when the process
        // is fully moved to auto_fd usage.
        //
        path isp;
        auto_fd ifd;
        int id (0); // @@ TMP
        const redirect& in (c.in.effective ());

        // Open a file for passing to the command stdin.
        //
        auto open_stdin = [&isp, &ifd, &id, &ll] ()
        {
          assert (!isp.empty ());

          try
          {
            ifd = fdopen (isp, fdopen_mode::in);
            id  = ifd.get ();
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
              ifd = fddup (id);
              id  = 0;
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to duplicate stdin: " << e;
            }

            break;
          }

        case redirect_type::none:
          // Somehow need to make sure that the child process doesn't read from
          // stdin. That is tricky to do in a portable way. Here we suppose
          // that the program which (erroneously) tries to read some data from
          // stdin being redirected to /dev/null fails not being able to read
          // the expected data, and so the test doesn't pass through.
          //
          // @@ Obviously doesn't cover the case when the process reads
          //    whatever available.
          // @@ Another approach could be not to redirect stdin and let the
          //    process to hang which can be interpreted as a test failure.
          // @@ Both ways are quite ugly. Is there some better way to do this?
          //
          // Fall through.
          //
        case redirect_type::null:
          {
            try
            {
              ifd.reset (fdnull ()); // @@ Eventually will be throwing.

              if (ifd.get () == -1) // @@ TMP
                throw io_error (
                  error_code (errno, system_category ()).message ());

              id = -2;
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to write to null device: " << e;
            }

            break;
          }

        case redirect_type::file:
          {
            isp = normalize (in.file.path, sp, ll);

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

            save (isp, transform (in.str, false, in.modifiers, *sp.root), ll);
            sp.clean ({cleanup_type::always, isp}, true);

            open_stdin ();
            break;
          }

        case redirect_type::merge:
        case redirect_type::here_str_regex:
        case redirect_type::here_doc_regex:
        case redirect_type::here_doc_ref:   assert (false); break;
        }

        // Dealing with stdout and stderr redirect types other than 'null'
        // using pipes is tricky in the general case. Going this path we would
        // need to read both streams in non-blocking manner which we can't
        // (easily) do in a portable way. Using diff utility to get a
        // nice-looking actual/expected outputs difference would complicate
        // things further.
        //
        // So the approach is the following. Child standard streams are
        // redirected to files. When the child exits and the exit status is
        // validated we just sequentially compare each file content with the
        // expected output. The positive side-effect of this approach is that
        // the output of a faulty command can be provided for troubleshooting.
        //

        // Open a file for command output redirect if requested explicitly
        // (file redirect) or for the purpose of the output validation (none,
        // here_*), register the file for cleanup, return the file descriptor.
        // Return the specified, default or -2 file descriptors for merge, pass
        // or null redirects respectively not opening a file.
        //
        auto open = [&sp, &ll, &std_path] (const redirect& r,
                                           int dfd,
                                           path& p,
                                           auto_fd& fd) -> int
          {
          assert (dfd == 1 || dfd == 2);
          const char* what (dfd == 1 ? "stdout" : "stderr");

          fdopen_mode m (fdopen_mode::out | fdopen_mode::create);

          switch (r.type)
          {
          case redirect_type::pass:
            {
              try
              {
                fd = fddup (dfd);
              }
              catch (const io_error& e)
              {
                fail (ll) << "unable to duplicate " << what << ": " << e;
              }

              return dfd;
            }

          case redirect_type::null:
            {
              try
              {
                fd.reset (fdnull ()); // @@ Eventully will be throwing.

                if (fd.get () == -1) // @@ TMP
                  throw io_error (
                    error_code (errno, system_category ()).message ());
              }
              catch (const io_error& e)
              {
                fail (ll) << "unable to write to null device: " << e;
              }

              return -2;
            }

          case redirect_type::merge:
            {
              // Duplicate the paired file descriptor later.
              //
              return r.fd;
            }

          case redirect_type::file:
            {
              // For the cmp mode the user-provided path refers a content to
              // match against, rather than a content to be produced (as for
              // overwrite and append modes). And so for cmp mode we redirect
              // the process output to a temporary file.
              //
              p = r.file.mode == redirect_fmode::compare
                ? std_path (what)
                : normalize (r.file.path, sp, ll);

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

          case redirect_type::here_doc_ref: assert (false); break;
          }

          try
          {
            fd = fdopen (p, m);

            if ((m & fdopen_mode::at_end) != fdopen_mode::at_end)
              sp.clean ({cleanup_type::always, p}, true);
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to write " << p << ": " << e;
          }

          return fd.get ();
        };

        path osp;
        auto_fd ofd;
        const redirect& out (c.out.effective ());
        int od (open (out, 1, osp, ofd));

        path esp;
        auto_fd efd;
        const redirect& err (c.err.effective ());
        int ed (open (err, 2, esp, efd));

        // Merge standard streams.
        //
        bool mo (out.type == redirect_type::merge);
        if (mo || err.type == redirect_type::merge)
        {
          auto_fd& self  (mo ? ofd : efd);
          auto_fd& other (mo ? efd : ofd);

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

        optional<process_exit> exit;
        builtin* b (builtins.find (c.program.string ()));

        if (b != nullptr)
        {
          // Execute the builtin.
          //
          try
          {
            future<uint8_t> f (
              (*b) (sp, c.arguments, move (ifd), move (ofd), move (efd)));

            exit = process_exit (f.get ());
          }
          catch (const system_error& e)
          {
            fail (ll) << "unable to execute " << c.program << " builtin: "
                      << e;
          }
        }
        else
        {
          // Execute the process.
          //
          cstrings args {c.program.string ().c_str ()};

          for (const auto& a: c.arguments)
            args.push_back (a.c_str ());

          args.push_back (nullptr);

          try
          {
            process_path pp (process::path_search (args[0]));

            if (verb >= 2)
              print_process (args);

            process pr (sp.wd_path.string ().c_str (),
                        pp,
                        args.data (),
                        id, od, ed);

            ifd.reset ();
            ofd.reset ();
            efd.reset ();

            pr.wait ();
            exit = move (pr.exit);
          }
          catch (const process_error& e)
          {
            error (ll) << "unable to execute " << args[0] << ": " << e;

            if (e.child ())
              std::exit (1);

            throw failed ();
          }
        }

        assert (exit);

        const path& p (c.program);

        // If there is no correct exit code by whatever reason then print the
        // proper diagnostics, dump stderr (if cached and not too large) and
        // fail.
        //
        bool valid (exit->normal ());

        // On Windows the exit code can be out of the valid codes range being
        // defined as uint16_t.
        //
#ifdef _WIN32
        if (valid)
          valid = exit->code () < 256;
#endif

        bool eq (c.exit.comparison == exit_comparison::eq);
        bool correct (valid && eq == (exit->code () == c.exit.status));

        if (!correct)
        {
          // Fail with a proper diagnostics.
          //
          diag_record d (fail (ll));

          if (!exit->normal ())
          {
            d << p << " terminated abnormally" <<
              info << exit->description ();

#ifndef _WIN32
            if (exit->core ())
              d << " (core dumped)";
#endif
          }
          else
          {
            uint16_t ec (exit->code ()); // Make sure is printed as integer.

            if (!valid)
              d << p << " exit status " << ec << " is invalid" <<
                info << "must be an unsigned integer < 256";
            else if (!correct)
              d << p << " exit status " << ec << (eq ? " != " : " == ")
                << static_cast<uint16_t> (c.exit.status);
            else
              assert (false);
          }

          if (non_empty (esp, ll))
            d << info << "stderr: " << esp;

          if (non_empty (osp, ll))
            d << info << "stdout: " << osp;

          if (non_empty (isp, ll))
            d << info << "stdin: " << isp;

          // Print cached stderr.
          //
          print_file (d, esp, ll);
        }

        // Exit code is correct. Check if the standard outputs match the
        // expectations.
        //
        check_output (p, osp, isp, out, ll, sp, "stdout");
        check_output (p, esp, isp, err, ll, sp, "stderr");
      }

      bool default_runner::
      run_if (scope&, const command_expr& expr, size_t, const location&)
      {
        const command& c (expr.back ().pipe.back ()); // @@ TMP
        return c.program.string () == "true"; // @@ TMP
      }
    }
  }
}
