// file      : build2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/runner>

#include <iostream> // cerr

#include <build2/filesystem>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      // Check if the file exists and is not empty.
      //
      static bool
      non_empty (const path& p)
      {
        if (!exists (p))
          return false;

        try
        {
          ifdstream is (p);
          return is.peek () != ifdstream::traits_type::eof ();
        }
        catch (const io_error& e)
        {
          error << "unable to read " << p << ": " << e.what ();
          throw failed ();
        }
      }

      // Check if the test command output matches the expected result (redirect
      // value).
      //
      static void
      check_output (const process_path& pr,
                    const char* nm,
                    const path& op,
                    const redirect& rd,
                    const location& cl,
                    scope& sp)
      {
        if (rd.type == redirect_type::none)
        {
          // Check that there is no output produced.
          //
          if (non_empty (op))
            fail (cl) << pr << " unexpectedly writes to " << nm <<
              info << nm << " is saved to " << op;
        }
        else if (rd.type == redirect_type::here_string ||
                 rd.type == redirect_type::here_document)
        {
          path orp (op + ".orig");

          try
          {
            ofdstream os (orp);
            sp.rm_paths.emplace_back (orp);
            os << rd.value;
            os.close ();
          }
          catch (const io_error& e)
          {
            fail << "unable to write " << orp << ": " << e.what ();
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
              d << pr << " " << nm << " doesn't match the expected output";

              auto output_info = [&d, &nm] (const path& p, const char* prefix)
              {
                if (non_empty (p))
                  d << info << prefix << nm << " is saved to " << p;
                else
                  d << info << prefix << nm << " is empty";
              };

              output_info (op, "");
              output_info (orp, "expected ");

              // Fall through.
              //
            }
            catch (const io_error&)
            {
              // Child exit status doesn't matter. Assume the child process
              // issued diagnostics. Just wait for the process completion.
              //
              p.wait (); // Check throw.

              error (cl) << "failed to compare " << nm
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
      enter (scope& sp, const location&)
      {
        if (!exists (sp.wd_path))
          mkdir (sp.wd_path, 2);
        else if (!empty (sp.wd_path))
          // @@ Shouldn't we have --wipe or smth?
          //
          fail << "directory " << sp.wd_path << " is not empty" <<
            info << "clean it up and rerun";

        sp.rm_paths.emplace_back (sp.wd_path);
      }

      void concurrent_runner::
      leave (scope& sp, const location& cl)
      {
        for (const auto& p: reverse_iterate (sp.rm_paths))
        {
          if (p.to_directory ())
          {
            dir_path d (path_cast<dir_path> (p));
            rmdir_status r (rmdir (d, 2));

            if (r != rmdir_status::success)
              fail (cl) << "registered for cleanup directory " << d
                        << (r == rmdir_status::not_empty
                            ? " not empty"
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
          int in;

          switch (c.in.type)
          {
          case redirect_type::pass:          in =  0; break;

          case redirect_type::here_string:
          case redirect_type::here_document: in = -1; break;

          case redirect_type::null:
          case redirect_type::none:          in = -2; break;
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
          auto opath = [sp, ci] (const char* nm) -> path
          {
            path r (sp.wd_path / path (nm));

            if (ci > 0)
              r += "-" + to_string (ci + 1); // Start from first line.

            return r;
          };

          auto open = [&sp] (ofdstream& os, const path& p) -> int
          {
            try
            {
              os.open (p);
              sp.rm_paths.emplace_back (p);
            }
            catch (const io_error& e)
            {
              fail << "unable to write " << p << ": " << e.what ();
            }

            return os.fd ();
          };

          ofdstream so;
          path stdout (opath ("stdout"));

          int out (c.out.type == redirect_type::pass
                   ? 1
                   : c.out.type == redirect_type::null
                     ? -2
                     : open (so, stdout));

          ofdstream se;
          path stderr (opath ("stderr"));

          int err (c.err.type == redirect_type::pass
                   ? 2
                   : c.err.type == redirect_type::null
                     ? -2
                     : open (se, stderr));

          if (verb >= 2)
            print_process (args);

          process pr (sp.wd_path.string ().c_str (),
                      pp,
                      args.data (),
                      in, out, err);

          try
          {
            so.close ();
            se.close ();

            if (c.in.type == redirect_type::here_string ||
                c.in.type == redirect_type::here_document)
            {
              ofdstream os (pr.out_fd);
              os << c.in.value;
              os.close ();
            }

            // Just wait. The program failure can mean the test success.
            //
            pr.wait ();

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
                  fail << "unable to read " << stderr << ": " << e.what ();
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

              if (non_empty (stdout))
                d << info << "stdout is saved to " << stdout;

              if (non_empty (stderr))
                d << info << "stderr is saved to " << stderr;
            }

            // Check if the standard outputs match expectations.
            //
            check_output (pp, "stdout", stdout, c.out, cl, sp);
            check_output (pp, "stderr", stderr, c.err, cl, sp);
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
