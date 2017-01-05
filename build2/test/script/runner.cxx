// file      : build2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/runner>

#include <set>
#include <ios> // streamsize

#include <butl/fdstream> // fdopen_mode, fdnull(), fddup()

#include <build2/filesystem>
#include <build2/test/script/builtin>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    namespace script
    {
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
          fail (ll) << "unable to read " << p << ": " << e.what () << endf;
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
            fail (ll) << "unable to read " << p << ": " << e.what ();
          }
        }
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

        bool re;
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
        else if ((re = (rd.type == redirect_type::here_str_regex ||
                        rd.type == redirect_type::here_doc_regex)) ||
                 rd.type == redirect_type::here_str_literal ||
                 rd.type == redirect_type::here_doc_literal)
        {
          assert (!op.empty ());

          // While the regex file is not used for output validation we still
          // create it for troubleshooting.
          //
          path opp (op + (re ? ".regex" : ".orig"));

          try
          {
            ofdstream os (opp);
            sp.clean ({cleanup_type::always, opp}, true);
            os << (re ? rd.regex.str : rd.str);
            os.close ();
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to write " << opp << ": " << e.what ();
          }

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

          if (re)
          {
            // Match the output with the line_regex. That requires to parse the
            // output into the line_string of literals first.
            //
            using namespace regex;

            line_string ls;

            try
            {
              // Do not throw when eofbit is set (end of stream reached), and
              // when failbit is set (getline() failed to extract any
              // character).
              //
              // Note that newlines are treated as line-chars separators. That
              // in particular means that the trailing newline produces a blank
              // line-char (empty literal). Empty output produces the
              // zero-length line-string.
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
                // adds too much trailing junk to the system_error
                // descriptions, and so it can appear in programs output. For
                // example:
                //
                // ...: Invalid data.\r\r\n
                //
                while (!s.empty () && s.back () == '\r')
                  s.pop_back ();

                ls += line_char (move (s), rd.regex.regex.pool);
              }
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to read " << op << ": " << e.what ();
            }

            if (regex_match (ls, rd.regex.regex)) // Doesn't throw.
              return;

            // Output doesn't match the regex.
            //
            diag_record d (error (ll));
            d << pr << " " << what << " doesn't match the regex";

            output_info (d, op);
            output_info (d, opp, "", " regex");
            input_info  (d);

            // Fall through.
            //
          }
          else
          {
            // Use diff utility to compare the output with the expected result.
            //
            path dp ("diff");
            process_path pp (run_search (dp, true));

            cstrings args {
              pp.recall_string (),
                "--strip-trailing-cr", // Is essential for cross-testing.
                "-u",
                opp.string ().c_str (),
                op.string ().c_str (),
                nullptr};

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
                fail (ll) << "unable to write " << ep << ": " << e.what ();
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
              output_info (d, opp, "expected ");
              output_info (d, ep, "", " diff");
              input_info  (d);

              print_file (d, ep, ll);

              // Fall through.
              //
            }
            catch (const process_error& e)
            {
              error (ll) << "unable to execute " << pp << ": " << e.what ();

              if (e.child ())
                exit (1);
            }

            // Fall through.
            //
          }

          throw failed ();
        }
      }

      void default_runner::
      enter (scope& sp, const location&)
      {
        if (!exists (sp.wd_path))
          // @@ Shouldn't we add an optional location parameter to mkdir() and
          // alike utility functions so the failure message can contain
          // location info?
          //
          mkdir (sp.wd_path, 2);
        else
          // Scope working directory shall be empty (the script working
          // directory is cleaned up by the test rule prior the script
          // execution).
          //
          assert (empty (sp.wd_path));

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

        // Normalize a path. Also make the relative path absolute using the
        // scope's working directory unless it is already absolute.
        //
        auto normalize = [&sp, &ll] (path p) -> path
        {
          path r (p.absolute () ? move (p) : sp.wd_path / move (p));

          try
          {
            r.normalize ();
          }
          catch (const invalid_path& e)
          {
            fail (ll) << "invalid file path " << e.path;
          }

          return r;
        };

        // Register the command explicit cleanups. Verify that the path being
        // cleaned up is a sub-path of the testscript working directory. Fail
        // if this is not the case.
        //
        for (const auto& cl: c.cleanups)
        {
          const path& p (cl.path);
          path np (normalize (p));

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
        auto std_path = [&li, &normalize] (const char* n) -> path
        {
          path p (n);

          // 0 if belongs to a single-line test scope, otherwise is the
          // command line number (start from one) in the test scope.
          //
          if (li > 0)
            p += "-" + to_string (li);

          return normalize (move (p));
        };

        // Assign file descriptors to pass as a builtin or a process standard
        // streams. Eventually the raw descriptors should gone when the process
        // is fully moved to auto_fd usage.
        //
        path isp;
        auto_fd ifd;
        int in (0); // @@ TMP

        // Open a file for passing to the command stdin.
        //
        auto open_stdin = [&isp, &ifd, &in, &ll] ()
        {
          assert (!isp.empty ());

          try
          {
            ifd = fdopen (isp, fdopen_mode::in);
            in  = ifd.get ();
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to read " << isp << ": " << e.what ();
          }
        };

        switch (c.in.type)
        {
        case redirect_type::pass:
          {
            try
            {
              ifd = fddup (in);
              in  = 0;
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to duplicate stdin: " << e.what ();
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

              in = -2;
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to write to null device: " << e.what ();
            }

            break;
          }

        case redirect_type::file:
          {
            isp = normalize (c.in.file.path);

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

            try
            {
              ofdstream os (isp);
              sp.clean ({cleanup_type::always, isp}, true);
              os << c.in.str;
              os.close ();
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to write " << isp << ": " << e.what ();
            }

            open_stdin ();
            break;
          }

        case redirect_type::merge:
        case redirect_type::here_str_regex:
        case redirect_type::here_doc_regex: assert (false); break;
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
        auto open = [&sp, &ll, &std_path, &normalize] (const redirect& r,
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
                fail (ll) << "unable to duplicate " << what << ": "
                          << e.what ();
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
                fail (ll) << "unable to write to null device: " << e.what ();
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
              p = normalize (r.file.path);
              m |= r.file.append ? fdopen_mode::at_end : fdopen_mode::truncate;
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
          }

          try
          {
            fd = fdopen (p, m);

            if ((m & fdopen_mode::at_end) != fdopen_mode::at_end)
              sp.clean ({cleanup_type::always, p}, true);
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to write " << p << ": " << e.what ();
          }

          return fd.get ();
        };

        path osp;
        auto_fd ofd;
        int out (open (c.out, 1, osp, ofd));

        path esp;
        auto_fd efd;
        int err (open (c.err, 2, esp, efd));

        // Merge standard streams.
        //
        bool mo (c.out.type == redirect_type::merge);
        if (mo || c.err.type == redirect_type::merge)
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
                      << ": " << e.what ();
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
                      << e.what ();
          }
        }
        else
        {
          // Execute the process.
          //
          // Pre-search the program path so it is reflected in the failure
          // diagnostics. The user can see the original path running the test
          // operation with the verbosity level > 2.
          //
          process_path pp (run_search (c.program, true));
          cstrings args {pp.recall_string ()};

          for (const auto& a: c.arguments)
            args.push_back (a.c_str ());

          args.push_back (nullptr);

          try
          {
            if (verb >= 2)
              print_process (args);

            process pr (sp.wd_path.string ().c_str (),
                        pp,
                        args.data (),
                        in, out, err);

            ifd.reset ();
            ofd.reset ();
            efd.reset ();

            pr.wait ();
            exit = move (pr.exit);
          }
          catch (const process_error& e)
          {
            error (ll) << "unable to execute " << pp << ": " << e.what ();

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
        check_output (p, osp, isp, c.out, ll, sp, "stdout");
        check_output (p, esp, isp, c.err, ll, sp, "stderr");
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
