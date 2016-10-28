// file      : build2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/runner>

#include <set>
#include <iostream> // cerr

#include <build2/filesystem>

using namespace std;

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
      non_empty (const path& p, const location& cl)
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
          error (cl) << "unable to read " << p << ": " << e.what ();
          throw failed ();
        }
      }

      // Check if the test command output matches the expected result (redirect
      // value). Noop for redirect types other than none, here_string,
      // here_document.
      //
      static void
      check_output (const process_path& pr,
                    const path& op,
                    const path& ip,
                    const redirect& rd,
                    const location& cl,
                    scope& sp,
                    const char* what)
      {
        auto input_info = [&ip, &cl] (diag_record& d)
        {
          if (non_empty (ip, cl))
            d << info << "stdin: " << ip;
        };

        if (rd.type == redirect_type::none)
        {
          assert (!op.empty ());

          // Check that there is no output produced.
          //
          if (non_empty (op, cl))
          {
            diag_record d (fail (cl));
            d << pr << " unexpectedly writes to " << what <<
              info << what << ": " << op;

            input_info (d);
          }
        }
        else if (rd.type == redirect_type::here_string ||
                 rd.type == redirect_type::here_document)
        {
          assert (!op.empty ());

          path orp (op + ".orig");

          try
          {
            ofdstream os (orp);
            sp.clean (orp);

            os << (rd.type == redirect_type::here_string
                   ? rd.str
                   : rd.doc.doc);

            os.close ();
          }
          catch (const io_error& e)
          {
            fail (cl) << "unable to write " << orp << ": " << e.what ();
          }

          // Use diff utility to compare the output with the expected result.
          //
          path dp ("diff");
          process_path pp (run_search (dp, true));

          cstrings args {
            pp.recall_string (),
            "--strip-trailing-cr",
            "-u",
            orp.string ().c_str (),
            op.string ().c_str (),
            nullptr};

          if (verb >= 2)
            print_process (args);

          try
          {
            // Diff utility prints the differences to stdout. But for the user
            // it is a part of the test failure diagnostics so let's redirect
            // stdout to stderr.
            //
            process p (pp, args.data (), 0, 2);

            try
            {
              if (p.wait ())
                return;

              // Output doesn't match the expected result.
              //
              diag_record d (error (cl));
              d << pr << " " << what << " doesn't match the expected output";

              auto output_info =
                [&d, &what, &cl] (const path& p, const char* prefix)
              {
                if (non_empty (p, cl))
                  d << info << prefix << what << ": " << p;
                else
                  d << info << prefix << what << " is empty";
              };

              output_info (op, "");
              output_info (orp, "expected ");
              input_info  (d);

              // Fall through.
              //
            }
            catch (const io_error&)
            {
              // Child exit status doesn't matter. Assume the child process
              // issued diagnostics. Just wait for the process completion.
              //
              p.wait (); // Check throw.

              error (cl) << "failed to compare " << what
                         << " with the expected output";
            }

            // Fall through.
            //
          }
          catch (const process_error& e)
          {
            error (cl) << "unable to execute " << pp << ": " << e.what ();

            if (e.child ())
              exit (1);
          }

          throw failed ();
        }
      }

      void concurrent_runner::
      enter (scope& sp, const location& cl)
      {
        if (!exists (sp.wd_path))
          // @@ Shouldn't we add an optional location parameter to mkdir() and
          // alike utility functions so the failure message can contain
          // location info?
          //
          mkdir (sp.wd_path, 2);
        else if (!empty (sp.wd_path))
          // @@ Shouldn't we have --wipe or smth?
          //
          fail (cl) << "directory " << sp.wd_path << " is not empty" <<
            info << "clean it up and rerun";

        sp.clean (sp.wd_path);
      }

      void concurrent_runner::
      leave (scope& sp, const location& cl)
      {
        // Remove files and directories in the order opposite to the order of
        // cleanup registration.
        //
        // Note that we operate with normalized paths here.
        //
        for (const auto& p: reverse_iterate (sp.cleanups))
        {
          if (p.to_directory ())
          {
            dir_path d (path_cast<dir_path> (p));
            rmdir_status r (rmdir (d, 2));

            if (r != rmdir_status::success)
              fail (cl) << "registered for cleanup directory " << d
                        << (r == rmdir_status::not_empty
                            ? " is not empty"
                            : " does not exist");
          }
          else if (rmfile (p, 2) == rmfile_status::not_exist)
            fail (cl) << "registered for cleanup file " << p
                      << " does not exist";
        }
      }

      void concurrent_runner::
      run (scope& sp, const command& c, size_t ci, const location& cl)
      {
        if (verb >= 3)
          text << c;

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
          // For stdin 'none' redirect type we somehow need to make sure that
          // the child process doesn't read from stdin. That is tricky to do in
          // a portable way. Here we suppose that the program which
          // (erroneously) tries to read some data from stdin being redirected
          // to /dev/null fails not being able to read the expected data, and
          // so the test doesn't pass through.
          //
          // @@ Obviously doesn't cover the case when the process reads
          //    whatever available.
          // @@ Another approach could be not to redirect stdin and let the
          //    process to hang which can be interpreted as a test failure.
          // @@ Both ways are quite ugly. Is there some better way to do this?
          //

          // Normalize a path. Also make relative path absolute using the
          // scope's working directory unless it is already absolute.
          //
          auto normalize = [&sp, &cl] (path p) -> path
          {
            path r (p.absolute () ? move (p) : sp.wd_path / move (p));

            try
            {
              r.normalize ();
            }
            catch (const invalid_path& e)
            {
              fail (cl) << "invalid file path " << e.path;
            }

            return r;
          };

          // Create unique path for a test command standard stream cache file.
          //
          auto std_path = [&ci, &normalize] (const char* n) -> path
          {
            path p (n);

            // 0 if belongs to a single-command test scope, otherwise is the
            // command number (start from one) in the test scope.
            //
            if (ci > 0)
              p += "-" + to_string (ci);

            return normalize (move (p));
          };

          path stdin;
          ifdstream si;
          int in;

          // Open a file for passing to the test command stdin.
          //
          auto open_stdin = [&stdin, &si, &in, &cl] ()
          {
            assert (!stdin.empty ());

            try
            {
              si.open (stdin);
            }
            catch (const io_error& e)
            {
              fail (cl) << "unable to read " << stdin << ": " << e.what ();
            }

            in = si.fd ();
          };

          switch (c.in.type)
          {
          case redirect_type::pass: in =  0; break;
          case redirect_type::null:
          case redirect_type::none: in = -2; break;

          case redirect_type::file:
            {
              stdin = normalize (c.in.file.path);
              open_stdin ();
              break;
            }

          case redirect_type::here_string:
          case redirect_type::here_document:
            {
              // We could write to process stdin directly but instead will
              // cache the data for potential troubleshooting.
              //
              stdin = std_path ("stdin");

              try
              {
                ofdstream os (stdin);
                os << (c.in.type == redirect_type::here_string
                       ? c.in.str
                       : c.in.doc.doc);
                os.close ();
              }
              catch (const io_error& e)
              {
                fail (cl) << "unable to write " << stdin << ": " << e.what ();
              }

              open_stdin ();
              sp.clean (stdin);
              break;
            }

          case redirect_type::merge: assert (false); break;
          }

          // Dealing with stdout and stderr redirect types other than 'null'
          // using pipes is tricky in the general case. Going this path we
          // would need to read both streams in non-blocking manner which we
          // can't (easily) do in a portable way. Using diff utility to get a
          // nice-looking actual/expected outputs difference would complicate
          // things further.
          //
          // So the approach is the following. Child standard streams are
          // redirected to files. When the child exits and the exit status is
          // validated we just sequentially compare each file content with the
          // expected output. The positive side-effect of this approach is that
          // the output of a faulty test command can be provided for
          // troubleshooting.
          //

          // Open a file for command output redirect if requested explicitly
          // (file redirect) or for the purpose of the output validation (none,
          // here_string, here_document), register the file for cleanup, return
          // the file descriptor. Return the specified, default or -2 file
          // descriptors for merge, pass or null redirects respectively not
          // opening a file.
          //
          auto open = [&sp, &cl, &std_path, &normalize] (const redirect& r,
                                                         int dfd,
                                                         path& p,
                                                         ofdstream& os) -> int
          {
            assert (dfd == 1 || dfd == 2);

            ofdstream::openmode m (ofdstream::out);

            switch (r.type)
            {
            case redirect_type::pass:  return dfd;
            case redirect_type::null:  return -2;
            case redirect_type::merge: return r.fd;

            case redirect_type::file:
              {
                p = normalize (r.file.path);

                if (r.file.append)
                  m |= ofdstream::app;

                break;
              }

            case redirect_type::none:
            case redirect_type::here_string:
            case redirect_type::here_document:
              {
                p = std_path (dfd == 1 ? "stdout" : "stderr");
                break;
              }
            }

            try
            {
              os.open (p, m);
            }
            catch (const io_error& e)
            {
              fail (cl) << "unable to write " << p << ": " << e.what ();
            }

            sp.clean (p);
            return os.fd ();
          };

          path stdout;
          ofdstream so;
          int out (open (c.out, 1, stdout, so));

          path stderr;
          ofdstream se;
          int err (open (c.err, 2, stderr, se));

          if (verb >= 2)
            print_process (args);

          process pr (sp.wd_path.string ().c_str (),
                      pp,
                      args.data (),
                      in, out, err);

          try
          {
            si.close ();
            so.close ();
            se.close ();

            // Just wait. The program failure can mean the test success.
            //
            pr.wait ();

            // Register command-created paths for cleanup.
            //
            for (const auto& p: c.cleanups)
              sp.clean (normalize (p));

            // If there is no correct exit status by whatever reason then dump
            // stderr (if cached), print the proper diagnostics and fail.
            //
            optional<process::status_type> status (move (pr.status));
            bool valid_status (status && *status >= 0 && *status < 256);
            bool eq (c.exit.comparison == exit_comparison::eq);

            bool correct_status (valid_status &&
                                 (*status == c.exit.status) == eq);

            if (!correct_status)
            {
              // Dump cached stderr.
              //
              if (exists (stderr))
              {
                try
                {
                  ifdstream is (stderr);
                  if (is.peek () != ifdstream::traits_type::eof ())
                    cerr << is.rdbuf ();
                }
                catch (const io_error& e)
                {
                  fail (cl) << "unable to read " << stderr << ": "
                            << e.what ();
                }
              }

              // Fail with a proper diagnostics.
              //
              diag_record d (fail (cl));

              if (!status)
                d << pp << " terminated abnormally";
              else if (!valid_status)
                d << pp << " exit status " << *status << " is invalid" <<
                  info << "must be an unsigned integer < 256";
              else if (!correct_status)
                d << pp << " exit status " << *status
                  << (eq ? " != " : " == ")
                  << static_cast<uint16_t> (c.exit.status);
              else
                assert (false);

              if (non_empty (stderr, cl))
                d << info << "stderr: " << stderr;

              if (non_empty (stdout, cl))
                d << info << "stdout: " << stdout;

              if (non_empty (stdin, cl))
                d << info << "stdin: " << stdin;
            }

            // Check if the standard outputs match expectations.
            //
            check_output (pp, stdout, stdin, c.out, cl, sp, "stdout");
            check_output (pp, stderr, stdin, c.err, cl, sp, "stderr");
          }
          catch (const io_error& e)
          {
            // Child exit status doesn't matter. Just wait for the process
            // completion.
            //
            pr.wait (); // Check throw.

            fail (cl) << "IO operation failed for " << pp << ": " << e.what ();
          }
        }
        catch (const process_error& e)
        {
          error (cl) << "unable to execute " << pp << ": " << e.what ();

          if (e.child ())
            exit (1);

          throw failed ();
        }
      }
    }
  }
}
