// file      : build2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/runner>

#include <set>
#include <iostream> // cerr

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

      // Check if the test command output matches the expected result (redirect
      // value). Noop for redirect types other than none, here_string,
      // here_document.
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
        else if (rd.type == redirect_type::here_string ||
                 rd.type == redirect_type::here_document)
        {
          assert (!op.empty ());

          path orp (op + ".orig");

          try
          {
            ofdstream os (orp);
            sp.clean ({cleanup_type::always, orp}, true);

            os << (rd.type == redirect_type::here_string
                   ? rd.str
                   : rd.doc.doc);

            os.close ();
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to write " << orp << ": " << e.what ();
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
              diag_record d (error (ll));
              d << pr << " " << what << " doesn't match the expected output";

              auto output_info =
                [&d, &what, &ll] (const path& p, const char* prefix)
              {
                if (non_empty (p, ll))
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

              error (ll) << "failed to compare " << what
                         << " with the expected output";
            }

            // Fall through.
            //
          }
          catch (const process_error& e)
          {
            error (ll) << "unable to execute " << pp << ": " << e.what ();

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
          // @@ Shouldn't we add an optional location parameter to mkdir() and
          // alike utility functions so the failure message can contain
          // location info?
          //
          mkdir (sp.wd_path, 2);
        else
          // The working directory is cleaned up by the test rule prior the
          // script execution.
          //
          assert (empty (sp.wd_path));

        // We don't change the current directory here but indicate that the
        // scope test commands will be executed in that directory.
        //
        if (verb >= 2)
          text << "cd " << sp.wd_path;

        sp.clean ({cleanup_type::always, sp.wd_path}, true);
      }

      void concurrent_runner::
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

      void concurrent_runner::
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
        // cleaned up is a sub-path of the root test scope working directory.
        // Fail if this is not the case.
        //
        for (const auto& cl: c.cleanups)
        {
          const path& p (cl.path);
          path np (normalize (p));

          bool wc (np.leaf ().string () == "***");
          const path& cp (wc ? np.directory () : np);
          const path& wd (sp.root->wd_path);

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

        case redirect_type::here_string:
        case redirect_type::here_document:
          {
            // We could write to the command stdin directly but instead will
            // cache the data for potential troubleshooting.
            //
            isp = std_path ("stdin");

            try
            {
              ofdstream os (isp);
              sp.clean ({cleanup_type::always, isp}, true);

              os << (c.in.type == redirect_type::here_string
                     ? c.in.str
                     : c.in.doc.doc);

              os.close ();
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to write " << isp << ": " << e.what ();
            }

            open_stdin ();
            break;
          }

        case redirect_type::merge: assert (false); break;
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
        // here_string, here_document), register the file for cleanup, return
        // the file descriptor. Return the specified, default or -2 file
        // descriptors for merge, pass or null redirects respectively not
        // opening a file.
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
          case redirect_type::here_string:
          case redirect_type::here_document:
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

        optional<process::status_type> status;
        builtin* b (builtins.find (c.program.string ()));

        if (b != nullptr)
        {
          // Execute the builtin.
          //
          try
          {
            future<uint8_t> f (
              (*b) (sp, c.arguments, move (ifd), move (ofd), move (efd)));

            status = f.get ();
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
            status = move (pr.status);
          }
          catch (const process_error& e)
          {
            error (ll) << "unable to execute " << pp << ": " << e.what ();

            if (e.child ())
              exit (1);

            throw failed ();
          }
        }

        const path& p (c.program);

        // If there is no correct exit status by whatever reason then dump
        // stderr (if cached), print the proper diagnostics and fail.
        //
        // Comparison *status >= 0 causes "always true" warning on Windows
        // where process::status_type is defined as uint32_t.
        //
        bool valid_status (status && *status < 256 && *status + 1 > 0);
        bool eq (c.exit.comparison == exit_comparison::eq);
        bool correct_status (valid_status && eq == (*status == c.exit.status));

        if (!correct_status)
        {
          // Dump cached stderr.
          //
          if (exists (esp))
          {
            try
            {
              ifdstream is (esp);
              if (is.peek () != ifdstream::traits_type::eof ())
                cerr << is.rdbuf ();
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to read " << esp << ": " << e.what ();
            }
          }

          // Fail with a proper diagnostics.
          //
          diag_record d (fail (ll));

          if (!status)
            d << p << " terminated abnormally";
          else if (!valid_status)
            d << p << " exit status " << *status << " is invalid" <<
              info << "must be an unsigned integer < 256";
          else if (!correct_status)
            d << p << " exit status " << *status << (eq ? " != " : " == ")
              << static_cast<uint16_t> (c.exit.status);
          else
            assert (false);

          if (non_empty (esp, ll))
            d << info << "stderr: " << esp;

          if (non_empty (osp, ll))
            d << info << "stdout: " << osp;

          if (non_empty (isp, ll))
            d << info << "stdin: " << isp;
        }

        // Check if the standard outputs match expectations.
        //
        check_output (p, osp, isp, c.out, ll, sp, "stdout");
        check_output (p, esp, isp, c.err, ll, sp, "stderr");
      }

      bool concurrent_runner::
      run_if (scope&, const command_expr& expr, size_t, const location&)
      {
        const command& c (expr.back ().pipe.back ()); // @@ TMP
        return c.program.string () == "true"; // @@ TMP
      }
    }
  }
}
