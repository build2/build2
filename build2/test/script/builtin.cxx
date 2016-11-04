// file      : build2/test/script/builtin.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/builtin>

#ifndef _WIN32
#  include <utime.h>
#else
#  include <sys/utime.h>
#endif

#include <butl/path-io>    // use default operator<< implementation
#include <butl/fdstream>   // fdopen_mode
#include <butl/filesystem> // mkdir_status

#include <build2/test/script/script>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    namespace script
    {
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

      // echo <string>...
      //
      static int
      echo (scope&, const strings& args, auto_fd in, auto_fd out, auto_fd err)
      try
      {
        int r (1);
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
      static int
      mkdir (scope& sp,
             const strings& args,
             auto_fd in, auto_fd out, auto_fd err)
      try
      {
        // @@ Should we set a proper verbosity so paths get printed as
        //    relative? Can be inconvenient for end-user when build2 runs from
        //    a testscript.
        //
        //    No, don't think so. If this were an external program, there
        //    won't be such functionality.
        //
        int r (1);
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

      // touch <path>...
      //
      // Change file access and modification times to the current time. Create
      // a file if doesn't exist. Fail if a file system entry other than file
      // exists for the name specified.
      //
      // Note that POSIX doesn't specify the behavior for touching an entry
      // other than file.
      //
      static int
      touch (scope& sp,
             const strings& args,
             auto_fd in, auto_fd out, auto_fd err)
      try
      {
        int r (1);
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

      const builtin_map builtins
      {
        {"echo",  &echo},
        {"mkdir", &mkdir},
        {"touch", &touch}
      };
    }
  }
}
