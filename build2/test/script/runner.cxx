// file      : build2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <set>
#include <iostream> // cerr

#include <build2/filesystem>
#include <build2/test/script/runner>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    namespace script
    {
      // @@ Eventually the cleanup facility should move into the scope object.
      //
      class cleanup
      {
      public:
        void
        add (path p) {files_.emplace (move (p));}

        void
        cancel (const path& p) {files_.erase (p);}

        ~cleanup ()
        {
          for (const auto& p: files_)
            try_rmfile (p, true);
        }

      private:
        set<path> files_;
      };

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
                    cleanup& cln)
      {
        if (rd.type == redirect_type::none)
        {
          // Check that there is no output produced.
          //
          if (non_empty (op))
          {
            cln.cancel (op);

            fail (cl) << pr << " unexpectedly writes to " << nm <<
              info << nm << " is saved to " << op;
          }
        }
        else if (rd.type == redirect_type::here_string ||
                 rd.type == redirect_type::here_document)
        {
          path orp (op + ".orig");

          try
          {
            ofdstream os (orp);
            cln.add (orp);

            os << rd.value;

            // Here-document is always newline-terminated.
            //
            if (rd.type == redirect_type::here_string)
              os << endl;

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

              // Output doesn't match the expected result. Keep non-empty
              // output and the expected result for troubleshooting.
              //
              cln.cancel (orp);

              diag_record d (error (cl));
              d << pr << " " << nm << " doesn't match the expected output";

              if (non_empty (op))
              {
                cln.cancel (op);
                d << info << nm << " is saved to " << op;
              }
              else
                d << info << nm << " is empty";

              // Expected output is never empty (contains at least newline).
              //
              d << info << "expected " << nm << " is saved to " << orp;

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
      }

      void concurrent_runner::
      leave (scope& sp, const location&)
      {
        if (exists (sp.wd_path) && empty (sp.wd_path))
          rmdir (sp.wd_path, 2);
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
          // to /dev/null fails not being able to do read expected data, and so
          // the test doesn't pass through.
          //
          // @@ Obviously doesn't cover the case when the process reads
          //    whatever available.
          // @@ Another approach could be not to redirect stdin and let the
          //    process to hang which can be interpreted as a test failure.
          // @@ Both ways are quite ugly. Is there some better way to do this?
          //
          int in (c.in.type == redirect_type::null ||
                  c.in.type == redirect_type::none
                  ? -2
                  : -1);

          // Dealing with stdout and stderr redirect types other than 'null'
          // using pipes is tricky in the general case. Going this path we
          // would need to read both streams in a non-blocking manner which we
          // can't (easily) do in a portable way. Using diff utility to get a
          // nice-looking actual/expected output difference would complicate
          // things further.
          //
          // So the approach is the following. Child standard stream are
          // redirected to files. When the child exits and the exit status is
          // validated we just sequentially compare each file content with the
          // expected output. The positive side-effect of this approach is that
          // the output of a faulty test command can be provided for
          // troubleshooting.
          //
          cleanup cln;

          auto opath = [sp, ci] (const char* nm) -> path
          {
            path r (sp.wd_path / path (nm));

            if (ci > 0)
              r += "-" + to_string (ci);

            return r;
          };

          auto open = [&cln] (ofdstream& os, const path& p) -> int
          {
            try
            {
              os.open (p);
              cln.add (p);
            }
            catch (const io_error& e)
            {
              fail << "unable to write " << p << ": " << e.what ();
            }

            return os.fd ();
          };

          ofdstream so;
          path stdout (opath ("stdout"));

          int out (c.out.type == redirect_type::null
                   ? -2
                   : open (so, stdout));

          ofdstream se;
          path stderr (opath ("stderr"));

          int err (c.err.type == redirect_type::null
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

              // Here-document is always newline-terminated.
              //
              if (c.in.type == redirect_type::here_string)
                os << endl;

              os.close ();
            }

            // Just wait. The program failure can mean the test success.
            //
            pr.wait ();

            // If there is no correct exit status by whatever reason then dump
            // stderr (if cached), keep both stdout and stderr (those which
            // are cached) for troubleshooting, print the proper diagnostics
            // and finally fail.
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

              // Keep non-empty cache files and fail with a proper diagnostics.
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

              auto keep_output = [&d, &cln] (const char* name, const path& p)
              {
                if (non_empty (p))
                {
                  cln.cancel (p);
                  d << info << name << " is saved to " << p;
                }
              };

              keep_output ("stdout", stdout);
              keep_output ("stderr", stderr);
            }

            // Check if the standard outputs match expectations.
            //
            check_output (pp, "stdout", stdout, c.out, cl, cln);
            check_output (pp, "stderr", stderr, c.err, cl, cln);
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
