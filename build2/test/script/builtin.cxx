// file      : build2/test/script/builtin.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/builtin>

#ifndef _WIN32
#  include <utime.h>
#else
#  include <sys/utime.h>
#endif

#include <thread>

#include <butl/path-io>    // use default operator<< implementation
#include <butl/fdstream>   // fdopen_mode, fdstream_mode
#include <butl/filesystem> // mkdir_status

#include <build2/test/script/script>

// Strictly speaking a builtin which reads/writes from/to standard streams
// must be asynchronous so that the caller can communicate with it through
// pipes without being blocked on I/O operations. However, as an optimization,
// we allow builtins that only print diagnostics to STDERR to be synchronous
// assuming that their output will always fit the pipe buffer. Synchronous
// builtins must not read from STDIN and write to STDOUT. Later we may relax
// this rule to allow a "short" output for such builtins.
//
using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    namespace script
    {
      using builtin_impl = uint8_t (scope&,
                                    const strings& args,
                                    auto_fd in, auto_fd out, auto_fd err);
      static future<uint8_t>
      to_future (uint8_t status)
      {
        promise<uint8_t> p;
        future<uint8_t> f (p.get_future ());
        p.set_value (status);
        return f;
      }

      // Operation failed, diagnostics has already been issued.
      //
      struct failed {};

      // Parse and normalize a path. Also, unless it is already absolute, make
      // the path absolute using the specified directory. Throw invalid_path
      // if the path is empty, and on parsing and normalization failures.
      //
      static path
      parse_path (string s, const dir_path& d)
      {
        path p (move (s));

        if (p.empty ())
          throw invalid_path ("");

        if (p.relative ())
          p = d / move (p);

        p.normalize ();
        return p;
      }

      // Builtin commands functions.
      //

      // cat <file>...
      //
      // Read files in sequence and write their contents to STDOUT in the same
      // sequence. Read from STDIN if no argumements provided or '-' is
      // specified as a file path. STDIN, STDOUT and file streams are set to
      // binary mode prior to I/O operations.
      //
      // Note that POSIX doesn't specify if after I/O operation failure the
      // command should proceed with the rest of the arguments. The current
      // implementation exits immediatelly in such a case.
      //
      // @@ Shouldn't we check that we don't print a nonempty regular file to
      //    itself, as that would merely exhaust the output device? POSIX
      //    allows (but not requires) such a check and some implementations do
      //    this. That would require to fstat() file descriptors and complicate
      //    the code a bit. Was able to reproduce on a big file (should be
      //    bigger than the stream buffer size) with the test
      //    'cat file >>>&file'.
      //
      // Note: must be executed asynchronously.
      //
      static uint8_t
      cat (scope& sp,
           const strings& args,
           auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        try
        {
          ifdstream cin  (move (in),  fdstream_mode::binary);
          ofdstream cout (move (out), fdstream_mode::binary);

          // Copy input stream to STDOUT.
          //
          auto copy = [&cout] (istream& is)
          {
            if (is.peek () != ifdstream::traits_type::eof ())
              cout << is.rdbuf ();

            is.clear (istream::eofbit); // Sets eofbit.
          };

          // Path of a file being printed to STDOUT. An empty path represents
          // STDIN. Used in diagnostics.
          //
          path p;

          try
          {
            // Print STDIN.
            //
            if (args.empty ())
              copy (cin);

            // Print files.
            //
            for (auto i (args.begin ()); i != args.end (); ++i)
            {
              if (*i == "-")
              {
                if (!cin.eof ())
                {
                  p.clear ();
                  copy (cin);
                }

                continue;
              }

              p = parse_path (*i, sp.wd_path);

              ifdstream is (p, ifdstream::binary);
              copy (is);
              is.close ();
            }
          }
          catch (const io_error& e)
          {
            cerr << "cat: unable to print ";

            if (p.empty ())
              cerr << "stdin";
            else
              cerr << "'" << p << "'";

            cerr << ": " << e.what () << endl;
            throw failed ();
          }

          cin.close ();
          cout.close ();
          r = 0;
        }
        catch (const invalid_path& e)
        {
          cerr << "cat: invalid path '" << e.path << "'" << endl;
        }
        // Can be thrown while closing cin, cout or writing to cerr (that's
        // why need to check its state before writing).
        //
        catch (const io_error& e)
        {
          if (cerr.good ())
            cerr << "cat: " << e.what () << endl;
        }
        catch (const failed&)
        {
          // Diagnostics has already been issued.
        }

        cerr.close ();
        return r;
      }
      catch (const std::exception&)
      {
        return 1;
      }

      // echo <string>...
      //
      // Note: must be executed asynchronously.
      //
      static uint8_t
      echo (scope&,
            const strings& args,
            auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        try
        {
          in.close ();
          ofdstream cout (move (out));

          for (auto b (args.begin ()), i (b), e (args.end ()); i != e; ++i)
            cout << (i != b ? " " : "") << *i;

          cout << endl;

          cout.close ();
          r = 0;
        }
        catch (const std::exception& e)
        {
          cerr << "echo: " << e.what () << endl;
        }

        cerr.close ();
        return r;
      }
      catch (const std::exception&)
      {
        return 1;
      }

      // false
      //
      // Return 1. Failure to close the file descriptors is silently ignored.
      //
      static future<uint8_t>
      false_ (scope&, const strings&, auto_fd, auto_fd, auto_fd)
      {
        return to_future (1);
      }

      // true
      //
      // Return 0. Failure to close the file descriptors is silently ignored.
      //
      static future<uint8_t>
      true_ (scope&, const strings&, auto_fd, auto_fd, auto_fd)
      {
        return to_future (0);
      }

      // Create a directory if not exist and its parent directories if
      // necessary. Throw system_error on failure. Register created
      // directories for cleanup. The directory path must be absolute.
      //
      static void
      mkdir_p (scope& sp, const dir_path& p)
      {
        if (!dir_exists (p))
        {
          if (!p.root ())
            mkdir_p (sp, p.directory ());

          try_mkdir (p); // Returns success or throws.
          sp.clean ({cleanup_type::always, p}, true);
        }
      }

      // mkdir [-p] <dir>...
      //
      // -p
      //    Create any missing intermediate pathname components. Each argument
      //    that names an existing directory must be ignored without error.
      //
      // Note that POSIX doesn't specify if after a directory creation failure
      // the command should proceed with the rest of the arguments. The current
      // implementation exits immediatelly in such a case.
      //
      // Note: can be executed synchronously.
      //
      static uint8_t
      mkdir (scope& sp,
             const strings& args,
             auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        try
        {
          in.close ();
          out.close ();

          auto i (args.begin ());

          // Process options.
          //
          bool parent (false);
          for (; i != args.end (); ++i)
          {
            if (*i == "-p")
              parent = true;
            else
            {
              if (*i == "--")
                ++i;

              break;
            }
          }

          // Create directories.
          //
          if (i == args.end ())
          {
            cerr << "mkdir: missing directory" << endl;
            throw failed ();
          }

          for (; i != args.end (); ++i)
          {
            dir_path p (path_cast<dir_path> (parse_path (*i, sp.wd_path)));

            try
            {
              if (parent)
                mkdir_p (sp, p);
              else if (try_mkdir (p) == mkdir_status::success)
                sp.clean ({cleanup_type::always, p}, true);
              else //                == mkdir_status::already_exists
                throw system_error (EEXIST, system_category ());
            }
            catch (const system_error& e)
            {
              cerr << "mkdir: unable to create directory '" << p << "': "
                   << e.what () << endl;
              throw failed ();
            }
          }

          r = 0;
        }
        catch (const invalid_path& e)
        {
          cerr << "mkdir: invalid path '" << e.path << "'" << endl;
        }
        // Can be thrown while closing in, out or writing to cerr (that's why
        // need to check its state before writing).
        //
        catch (const io_error& e)
        {
          if (cerr.good ())
            cerr << "mkdir: " << e.what () << endl;
        }
        catch (const failed&)
        {
          // Diagnostics has already been issued.
        }

        cerr.close ();
        return r;
      }
      catch (const std::exception&)
      {
        return 1;
      }

      // touch <file>...
      //
      // Change file access and modification times to the current time. Create
      // a file if doesn't exist. Fail if a file system entry other than file
      // exists for the name specified.
      //
      // Note that POSIX doesn't specify the behavior for touching an entry
      // other than file.
      //
      // Also note that POSIX doesn't specify if after a file touch failure the
      // command should proceed with the rest of the arguments. The current
      // implementation exits immediatelly in such a case.
      //
      // Note: can be executed synchronously.
      //
      static uint8_t
      touch (scope& sp,
             const strings& args,
             auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        try
        {
          in.close ();
          out.close ();

          if (args.empty ())
          {
            cerr << "touch: missing file" << endl;
            throw failed ();
          }

          // Create files.
          //
          for (auto i (args.begin ()); i != args.end (); ++i)
          {
            path p (parse_path (*i, sp.wd_path));

            try
            {
              if (file_exists (p))
              {
                // Set the file access and modification times to the current
                // time. Note that we don't register (implicit) cleanup for an
                // existing path.
                //
#ifndef _WIN32
                if (utime  (p.string ().c_str (), nullptr) == -1)
#else
                if (_utime (p.string ().c_str (), nullptr) == -1)
#endif
                  throw system_error (errno, system_category ());
              }
              else if (!entry_exists (p))
              {
                // Create the file. Assume the file access and modification
                // times are set to the current time automatically.
                //
                try
                {
                  fdopen (p, fdopen_mode::out | fdopen_mode::create);
                }
                catch (const io_error& e)
                {
                  cerr << "touch: cannot create file '" << p << "': "
                       << e.what () << endl;
                  throw failed ();
                }

                sp.clean ({cleanup_type::always, p}, true);
              }
              else
              {
                cerr << "touch: '" << p << "' exists and is not a file"
                     << endl;
                throw failed ();
              }
            }
            catch (const system_error& e)
            {
              cerr << "touch: cannot create/update '" << p << "': "
                   << e.what () << endl;
              throw failed ();
            }
          }

          r = 0;
        }
        catch (const invalid_path& e)
        {
          cerr << "touch: invalid path '" << e.path << "'" << endl;
        }
        // Can be thrown while closing in, out or writing to cerr (that's why
        // need to check its state before writing).
        //
        catch (const io_error& e)
        {
          if (cerr.good ())
            cerr << "touch: " << e.what () << endl;
        }
        catch (const failed&)
        {
          // Diagnostics has already been issued.
        }

        cerr.close ();
        return r;
      }
      catch (const std::exception&)
      {
        return 1;
      }

      static void
      thread_thunk (builtin_impl* fn,
                    scope& sp,
                    const strings& args,
                    auto_fd in, auto_fd out, auto_fd err,
                    promise<uint8_t> p)
      {
        // The use of set_value_at_thread_exit() would be more appropriate but
        // the function is not supported by old versions of g++ (e.g., not in
        // 4.9). There could also be overhead associated with it.
        //
        p.set_value (fn (sp, args, move (in), move (out), move (err)));
      }

      // Run builtin implementation asynchronously.
      //
      static future<uint8_t>
      async_impl (builtin_impl fn,
                  scope& sp,
                  const strings& args,
                  auto_fd in, auto_fd out, auto_fd err)
      {
        promise<uint8_t> p;
        future<uint8_t> f (p.get_future ());

        thread t (thread_thunk,
                  fn,
                  ref (sp),
                  cref (args),
                  move (in), move (out), move (err),
                  move (p));

        t.detach ();
        return f;
      }

      template <builtin_impl fn>
      static future<uint8_t>
      async_impl (scope& sp,
                  const strings& args,
                  auto_fd in, auto_fd out, auto_fd err)
      {
        return async_impl (fn, sp, args, move (in), move (out), move (err));
      }

      // Run builtin implementation synchronously.
      //
      template <builtin_impl fn>
      static future<uint8_t>
      sync_impl (scope& sp,
                 const strings& args,
                 auto_fd in, auto_fd out, auto_fd err)
      {
        return to_future (fn (sp, args, move (in), move (out), move (err)));
      }

      const builtin_map builtins
      {
        {"cat",   &async_impl<&cat>},
        {"echo",  &async_impl<&echo>},
        {"false", &false_},
        {"mkdir", &sync_impl<&mkdir>},
        {"touch", &sync_impl<&touch>},
        {"true",  &true_}
      };
    }
  }
}
