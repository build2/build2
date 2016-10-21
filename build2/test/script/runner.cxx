// file      : build2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef _WIN32
#  include <sys/wait.h>  // WIFEXITED(), WEXITSTATUS()
#endif

#include <iostream> // cerr

#include <butl/filesystem> // auto_rm

#include <build2/test/script/runner>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    namespace script
    {
      // Test command output cache. The usage is as follows:
      //
      // 1. Call wopen() to open the stream in write mode and register the file
      //    for auto-removal by dtor.
      //
      // 2. Pass the file descriptor to the test command process ctor to
      //    redirect it's output to the cache file.
      //
      // 3. Close the stream.
      //
      // 4. Call ropen() to open the file in read mode to match the content to
      //    a pattern, cancel the file removal if the match fails (so the
      //    output is available for troubleshooting).
      //
      class stream_cache: public ifdstream
      {
      public:
        using path_type = butl::path;

        stream_cache (const char* n): name (n) {}

        // Open stream for writing. Return the file descriptor. Must not be
        // called multiple times.
        //
        int
        wopen ()
        {
          // Otherwise compiler gets confused with basic_ios::fail().
          //
          using build2::fail;

          assert (!exists ());

          try
          {
            // @@ Later it will make sense to create the file in the
            //    test-specific temporary directory.
            //
            path = path_type::temp_path ("build2");
          }
          catch (const system_error& e)
          {
            fail << "unable to create temporary file: " << e.what ();
          }

          try
          {
            open (path, out | trunc);
            cleanup = auto_rm<path_type> (path);
          }
          catch (const io_error& e)
          {
            fail << "unable to write " << path << ": " << e.what ();
          }

          return fd ();
        }

        // Open stream for reading. Return true if the file is not empty,
        // false otherwise. Must not be called before wopen().
        //
        bool
        ropen ()
        {
          // Otherwise compiler gets confused with basic_ios::fail().
          //
          using build2::fail;

          assert (exists ());

          try
          {
            open (path, in);
            return peek () != ifdstream::traits_type::eof ();
          }
          catch (const io_error& e)
          {
            error << "unable to read " << path << ": " << e.what ();
            throw failed ();
          }
        }

        // Return true if wopen() was called, false otherwise.
        //
        bool
        exists () const {return !path.empty ();}

        ~stream_cache () override
        {
          close (); // Close the stream prior to the file deletion.
        }

      public:
        string name;
        path_type path;
        auto_rm<path_type> cleanup;
      };

      // Check if the test command output matches the pattern (redirect value).
      //
      static void
      check_output (const process_path& program,
                    stream_cache& sc,
                    const redirect& rd)
      {
        if (rd.type == redirect_type::none)
        {
          // Check that the cache file is empty.
          //
          if (sc.ropen ())
          {
            sc.cleanup.cancel ();

            fail << program << " unexpectedly writes to " << sc.name <<
                info << sc.name << " is saved to " << sc.path;
          }
        }
        else if (rd.type == redirect_type::here_string ||
                 rd.type == redirect_type::here_document)
        {
          // Use diff utility to compare the output with the pattern.
          //
          path dp ("diff");
          process_path pp (run_search (dp, true));

          cstrings args {
            pp.recall_string (),
            "--strip-trailing-cr",
            "-u",
            "-",
            sc.path.string ().c_str (),
            nullptr};

          if (verb >= 2)
            print_process (args);

          try
          {
            // Diff utility prints the differences to stdout. But for the user
            // it is a part of the test failure diagnostics so let's redirect
            // stdout to stderr.
            //
            process pr (pp, args.data (), -1, 2);

            try
            {
              ofdstream os (pr.out_fd);

              auto write_value = [&os, &rd] ()
              {
                os << rd.value;

                // Here-document is always endline-terminated.
                //
                if (rd.type == redirect_type::here_string)
                  os << endl;

                os.close ();
              };

              write_value ();

              if (pr.wait ())
                return;

              // Output doesn't match the pattern string. Keep non-empty output
              // and save the pattern for troubleshooting.
              //
              path p (sc.path + ".pattern");

              try
              {
                os.open (p);
                write_value ();
              }
              catch (const io_error& e)
              {
                fail << "unable to write " << p << ": " << e.what ();
              }

              diag_record d (error);
              d << program << " " << sc.name
                << " doesn't match the pattern";

              if (sc.ropen ())
              {
                sc.cleanup.cancel ();
                d << info << sc.name << " is saved to " << sc.path;
              }
              else
                d << info << sc.name << " is empty";

              // Pattern is never empty (contains at least newline).
              //
              d << info << sc.name << " pattern is saved to " << p;

              // Fall through.
              //
            }
            catch (const io_error& e)
            {
              // Child exit status doesn't matter. Assume the child process
              // issued diagnostics. Just wait for the process completion.
              //
              pr.wait (); // Check throw.
            }

            // Fall through.
            //
          }
          catch (const process_error& e)
          {
            error << "unable to execute " << args[0] << ": " << e.what ();

            if (e.child ())
              exit (1);
          }

          throw failed ();
        }
      }

      void concurrent_runner::
      run (const command& c)
      {
        if (verb >= 3)
        {
          // @@ When running multiple threads will need to synchronize
          // printing the diagnostics so it don't overlap for concurrent
          // tests. Alternatively we can not bother with that and expect a
          // user to re-run test operation in the single-thread mode.
          //
          // @@ No indentation performed for here-documents. If to fix then
          // probably need to do on diag_record level in a way similar to
          // butl::pager approach.
          //
          text << c;
        }

        // Pre-search the program path so it is reflected in the failure
        // diagnostics. The user can see the original path running the test
        // operation with the verbosity level > 2.
        //
        process_path pp (run_search (c.program, true));
        cstrings args {pp.recall_string ()};

        for (const auto& a: c.arguments)
          args.push_back (a.c_str ());

        args.push_back (nullptr);

        // Normally while handling child process failures (IO errors, non-zero
        // exit status) we suppress the diagnostics supposing that the child
        // issues it's own one. While this is reasonable to expect from known
        // production-quality programs here it can result in the absense of any
        // diagnostics at all. Also the child stderr (and so diagnostics) can
        // be redirected to /dev/null and not be available for the user. This
        // why we will always issue the diagnostics despite the fact sometimes
        // it can look redundant.
        //
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
          // nice-looking stream/pattern difference would complicate things
          // further.
          //
          // So the approach is the following. Child standard stream are
          // redirected to files. When the child exits and the exit status is
          // validated we just sequentially compare each file content with the
          // corresponding pattern. The positive side-effect of this approach
          // is that the output of a faulty test command can be provided for
          // troubleshooting.
          //
          stream_cache osc ("stdout");
          int out (c.out.type == redirect_type::null ? -2 : osc.wopen ());

          stream_cache esc ("stderr");
          int err (c.err.type == redirect_type::null ? -2 : esc.wopen ());

          if (verb >= 2)
            print_process (args);

          process pr (pp, args.data (), in, out, err);

          try
          {
            osc.close ();
            esc.close ();

            if (c.in.type == redirect_type::here_string ||
                c.in.type == redirect_type::here_document)
            {
              ofdstream os (pr.out_fd);
              os << c.in.value;

              // Here-document is always endline-terminated.
              //
              if (c.in.type == redirect_type::here_string)
                os << endl;

              os.close ();
            }

            // Just wait. The program failure can mean the test success.
            //
            pr.wait ();

            // Check if the process terminated normally and obtain the status
            // if that's the case.
            //
            bool abnorm;
            process::status_type status;

            // @@ Shouldn't we incorporate means for checking for abnormal
            //    termination and getting the real exit status into
            //    libbutl::process?
            //
#ifndef _WIN32
            abnorm = !WIFEXITED (pr.status);
            status = abnorm ? 1 : WEXITSTATUS (pr.status);
#else
            // @@ Is there a reliable way to detect if the process terminated
            //    abnormally on Windows?
            //
            abnorm = false;
            status = pr.status;
#endif
            bool valid_status (!abnorm && status >= 0 && status < 256);

            bool eq (c.exit.comparison == exit_comparison::eq);

            bool correct_status (valid_status &&
                                 (status == c.exit.status) == eq);

            // If there is no correct exit status by whatever reason then dump
            // stderr (if cached), keep both stdout and stderr (those which
            // are cached) for troubleshooting, and finally fail.
            //
            if (!correct_status)
            {
              if (esc.exists () && esc.ropen ())
                cerr << esc.rdbuf ();

              // Keep non-empty cache files and fail with a proper diagnostics.
              //
              diag_record d (fail);

              if (abnorm)
                d << pp << " terminated abnormally";
              else if (!valid_status)
                d << pp << " exit status " << status << " is invalid" <<
                  info << "must be an unsigned integer < 256";
              else if (!correct_status)
                d << pp << " exit status " << status
                  << (eq ? " != " : " == ") << (int)c.exit.status; //@@
              else
                assert (false);

              auto keep_stream = [&d] (stream_cache& sc)
              {
                if (sc.exists () && sc.ropen ())
                {
                  sc.cleanup.cancel ();
                  d << info << sc.name << " is saved to " << sc.path;
                }
              };

              keep_stream (osc);
              keep_stream (esc);
            }

            check_output (pp, osc, c.out);
            check_output (pp, esc, c.err);
          }
          catch (const io_error& e)
          {
            // Child exit status doesn't matter. Just wait for the process
            // completion.
            //
            pr.wait (); // Check throw.

            fail << "IO operation failed for " << pp << ": " << e.what ();
          }
        }
        catch (const process_error& e)
        {
          error << "unable to execute " << pp << ": " << e.what ();

          if (e.child ())
            exit (1);

          throw failed ();
        }
      }
    }
  }
}
