// file      : build2/test/script/builtin.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/builtin.hxx>

#include <chrono>
#include <locale>
#include <ostream>
#include <sstream>
#include <cstdlib> // strtoull()

#include <libbutl/regex.mxx>
#include <libbutl/path-io.mxx>    // use default operator<< implementation
#include <libbutl/fdstream.mxx>   // fdopen_mode, fdstream_mode
#include <libbutl/filesystem.mxx>

#include <libbuild2/context.hxx> // sched

#include <build2/test/script/script.hxx>

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

      // Operation failed, diagnostics has already been issued.
      //
      struct failed {};

      // Accumulate an error message, print it atomically in dtor to the
      // provided stream and throw failed afterwards if requested. Prefixes
      // the message with the builtin name.
      //
      // Move constructible-only, not assignable (based to diag_record).
      //
      class error_record
      {
      public:
        template <typename T>
        friend const error_record&
        operator<< (const error_record& r, const T& x)
        {
          r.ss_ << x;
          return r;
        }

        error_record (ostream& o, bool fail, const char* name)
            : os_ (o), fail_ (fail), empty_ (false)
        {
          ss_ << name << ": ";
        }

        // Older versions of libstdc++ don't have the ostringstream move
        // support. Luckily, GCC doesn't seem to be actually needing move due
        // to copy/move elision.
        //
#ifdef __GLIBCXX__
        error_record (error_record&&);
#else
        error_record (error_record&& r)
            : os_ (r.os_),
              ss_ (move (r.ss_)),
              fail_ (r.fail_),
              empty_ (r.empty_)
        {
          r.empty_ = true;
        }
#endif

        ~error_record () noexcept (false)
        {
          if (!empty_)
          {
            // The output stream can be in a bad state (for example as a
            // result of unsuccessful attempt to report a previous error), so
            // we check it.
            //
            if (os_.good ())
            {
              ss_.put ('\n');
              os_ << ss_.str ();
              os_.flush ();
            }

            if (fail_)
              throw failed ();
          }
        }

      private:
        ostream& os_;
        mutable ostringstream ss_;

        bool fail_;
        bool empty_;
      };

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
      //    'cat file >+file'.
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

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "cat");
        };

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
            error_record d (error ());
            d << "unable to print ";

            if (p.empty ())
              d << "stdin";
            else
              d << "'" << p << "'";

            d << ": " << e;
          }

          cin.close ();
          cout.close ();
          r = 0;
        }
        catch (const invalid_path& e)
        {
          error (false) << "invalid path '" << e.path << "'";
        }
        // Can be thrown while creating/closing cin, cout or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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

      // Make a copy of a file at the specified path, preserving permissions,
      // and registering a cleanup for a newly created file. The file paths
      // must be absolute. Fail if an exception is thrown by the underlying
      // copy operation.
      //
      static void
      cpfile (scope& sp,
              const path& from, const path& to,
              bool overwrite,
              bool attrs,
              bool cleanup,
              const function<error_record()>& fail)
      {
        try
        {
          bool exists (file_exists (to));

          cpflags f (
            overwrite
            ? cpflags::overwrite_permissions | cpflags::overwrite_content
            : cpflags::none);

          if (attrs)
            f |= cpflags::overwrite_permissions | cpflags::copy_timestamps;

          cpfile (from, to, f);

          if (!exists && cleanup)
            sp.clean ({cleanup_type::always, to}, true);
        }
        catch (const system_error& e)
        {
          fail () << "unable to copy file '" << from << "' to '" << to
                  << "': " << e;
        }
      }

      // Make a copy of a directory at the specified path, registering a
      // cleanup for the created directory. The directory paths must be
      // absolute. Fail if the destination directory already exists or
      // an exception is thrown by the underlying copy operation.
      //
      static void
      cpdir (scope& sp,
             const dir_path& from, const dir_path& to,
             bool attrs,
             bool cleanup,
             const function<error_record()>& fail)
      {
        try
        {
          if (try_mkdir (to) == mkdir_status::already_exists)
            throw_generic_error (EEXIST);

          if (cleanup)
            sp.clean ({cleanup_type::always, to}, true);

          for (const auto& de: dir_iterator (from,
                                             false /* ignore_dangling */))
          {
            path f (from / de.path ());
            path t (to / de.path ());

            if (de.type () == entry_type::directory)
              cpdir (sp,
                     path_cast<dir_path> (move (f)),
                     path_cast<dir_path> (move (t)),
                     attrs,
                     cleanup,
                     fail);
            else
              cpfile (sp, f, t, false /* overwrite */, attrs, cleanup, fail);
          }

          // Note that it is essential to copy timestamps and permissions after
          // the directory content is copied.
          //
          if (attrs)
          {
            path_permissions (to, path_permissions (from));
            dir_time (to, dir_time (from));
          }
        }
        catch (const system_error& e)
        {
          fail () << "unable to copy directory '" << from << "' to '" << to
                  << "': " << e;
        }
      }

      // cp [-p] [--no-cleanup]        <src-file>     <dst-file>
      // cp [-p] [--no-cleanup] -R|-r  <src-dir>      <dst-dir>
      // cp [-p] [--no-cleanup]        <src-file>...  <dst-dir>/
      // cp [-p] [--no-cleanup] -R|-r  <src-path>...  <dst-dir>/
      //
      // Note: can be executed synchronously.
      //
      static uint8_t
      cp (scope& sp,
          const strings& args,
          auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "cp");
        };

        try
        {
          in.close ();
          out.close ();

          auto i (args.begin ());
          auto e (args.end ());

          // Process options.
          //
          bool recursive (false);
          bool attrs (false);
          bool cleanup (true);
          for (; i != e; ++i)
          {
            const string& o (*i);

            if (o == "-R" || o == "-r")
              recursive = true;
            else if (o == "-p")
              attrs = true;
            else if (o == "--no-cleanup")
              cleanup = false;
            else
            {
              if (o == "--")
                ++i;

              break;
            }
          }

          // Copy files or directories.
          //
          if (i == e)
            error () << "missing arguments";

          const dir_path& wd (sp.wd_path);

          auto j (args.rbegin ());
          path dst (parse_path (*j++, wd));
          e = j.base ();

          if (i == e)
            error () << "missing source path";

          auto fail = [&error] () {return error (true);};

          // If destination is not a directory path (no trailing separator)
          // then make a copy of the filesystem entry at the specified path
          // (the only source path is allowed in such a case). Otherwise copy
          // the source filesystem entries into the destination directory.
          //
          if (!dst.to_directory ())
          {
            path src (parse_path (*i++, wd));

            // If there are multiple sources but no trailing separator for the
            // destination, then, most likelly, it is missing.
            //
            if (i != e)
              error () << "multiple source paths without trailing separator "
                       << "for destination directory";

            if (!recursive)
              // Synopsis 1: make a file copy at the specified path.
              //
              cpfile (sp,
                      src,
                      dst,
                      true /* overwrite */,
                      attrs,
                      cleanup,
                      fail);
            else
              // Synopsis 2: make a directory copy at the specified path.
              //
              cpdir (sp,
                     path_cast<dir_path> (src), path_cast<dir_path> (dst),
                     attrs,
                     cleanup,
                     fail);
          }
          else
          {
            for (; i != e; ++i)
            {
              path src (parse_path (*i, wd));

              if (recursive && dir_exists (src))
                // Synopsis 4: copy a filesystem entry into the specified
                // directory. Note that we handle only source directories here.
                // Source files are handled below.
                //
                cpdir (sp,
                       path_cast<dir_path> (src),
                       path_cast<dir_path> (dst / src.leaf ()),
                       attrs,
                       cleanup,
                       fail);
              else
                // Synopsis 3: copy a file into the specified directory. Also,
                // here we cover synopsis 4 for the source path being a file.
                //
                cpfile (sp,
                        src,
                        dst / src.leaf (),
                        true /* overwrite */,
                        attrs,
                        cleanup,
                        fail);
            }
          }

          r = 0;
        }
        catch (const invalid_path& e)
        {
          error (false) << "invalid path '" << e.path << "'";
        }
        // Can be thrown while closing in, out or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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

          cout << '\n';
          cout.close ();
          r = 0;
        }
        catch (const std::exception& e)
        {
          cerr << "echo: " << e << endl;
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
      // Failure to close the file descriptors is silently ignored.
      //
      // Note: can be executed synchronously.
      //
      static builtin
      false_ (scope&, uint8_t& r, const strings&, auto_fd, auto_fd, auto_fd)
      {
        return builtin (r = 1);
      }

      // true
      //
      // Failure to close the file descriptors is silently ignored.
      //
      // Note: can be executed synchronously.
      //
      static builtin
      true_ (scope&, uint8_t& r, const strings&, auto_fd, auto_fd, auto_fd)
      {
        return builtin (r = 0);
      }

      // Create a symlink to a file or directory at the specified path. The
      // paths must be absolute. Fall back to creating a hardlink, if symlink
      // creation is not supported for the link path. If hardlink creation is
      // not supported either, then fall back to copies. If requested, created
      // filesystem entries are registered for cleanup. Fail if the target
      // filesystem entry doesn't exist or an exception is thrown by the
      // underlying filesystem operation (specifically for an already existing
      // filesystem entry at the link path).
      //
      // Note that supporting optional removal of an existing filesystem entry
      // at the link path (the -f option) tends to get hairy. As soon as an
      // existing and the resulting filesystem entries could be of different
      // types, we would end up with canceling an old cleanup and registering
      // the new one. Also removing non-empty directories doesn't look very
      // natural, but would be required if we want the behavior on POSIX and
      // Windows to be consistent.
      //
      static void
      mksymlink (scope& sp,
                 const path& target, const path& link,
                 bool cleanup,
                 const function<error_record()>& fail)
      {
        // Determine the target type, fail if the target doesn't exist.
        //
        bool dir (false);

        try
        {
          pair<bool, entry_stat> pe (path_entry (target));

          if (!pe.first)
            fail () << "unable to create symlink to '" << target << "': "
                    << "no such file or directory";

          dir = pe.second.type == entry_type::directory;
        }
        catch (const system_error& e)
        {
          fail () << "unable to stat '" << target << "': " << e;
        }

        // First we try to create a symlink. If that fails (e.g., "Windows
        // happens"), then we resort to hard links. If that doesn't work out
        // either (e.g., not on the same filesystem), then we fall back to
        // copies. So things are going to get a bit nested.
        //
        try
        {
          mksymlink (target, link, dir);

          if (cleanup)
            sp.clean ({cleanup_type::always, link}, true);
        }
        catch (const system_error& e)
        {
          // Note that we are not guaranteed (here and below) that the
          // system_error exception is of the generic category.
          //
          int c (e.code ().value ());
          if (!(e.code ().category () == generic_category () &&
                (c == ENOSYS || // Not implemented.
                 c == EPERM)))  // Not supported by the filesystem(s).
            fail () << "unable to create symlink '" << link << "' to '"
                    << target << "': " << e;

          try
          {
            mkhardlink (target, link, dir);

            if (cleanup)
              sp.clean ({cleanup_type::always, link}, true);
          }
          catch (const system_error& e)
          {
            c = e.code ().value ();
            if (!(e.code ().category () == generic_category () &&
                  (c == ENOSYS || // Not implemented.
                   c == EPERM  || // Not supported by the filesystem(s).
                   c == EXDEV)))  // On different filesystems.
              fail () << "unable to create hardlink '" << link << "' to '"
                      << target << "': " << e;

            if (dir)
              cpdir (sp,
                     path_cast<dir_path> (target), path_cast<dir_path> (link),
                     false,
                     cleanup,
                     fail);
            else
              cpfile (sp,
                      target,
                      link,
                      false /* overwrite */,
                      true /* attrs */,
                      cleanup,
                      fail);
          }
        }
      }

      // ln [--no-cleanup] -s <target-path>    <link-path>
      // ln [--no-cleanup] -s <target-path>... <link-dir>/
      //
      // Note: can be executed synchronously.
      //
      static uint8_t
      ln (scope& sp,
          const strings& args,
          auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "ln");
        };

        try
        {
          in.close ();
          out.close ();

          auto i (args.begin ());
          auto e (args.end ());

          // Process options.
          //
          bool cleanup (true);
          bool symlink (false);

          for (; i != e; ++i)
          {
            const string& o (*i);

            if (o == "--no-cleanup")
              cleanup = false;
            else if (o == "-s")
              symlink = true;
            else
            {
              if (o == "--")
                ++i;

              break;
            }
          }

          if (!symlink)
            error () << "missing -s option";

          // Create file or directory symlinks.
          //
          if (i == e)
            error () << "missing arguments";

          const dir_path& wd (sp.wd_path);

          auto j (args.rbegin ());
          path link (parse_path (*j++, wd));
          e = j.base ();

          if (i == e)
            error () << "missing target path";

          auto fail = [&error] () {return error (true);};

          // If link is not a directory path (no trailing separator), then
          // create a symlink to the target path at the specified link path
          // (the only target path is allowed in such a case). Otherwise create
          // links to the target paths inside the specified directory.
          //
          if (!link.to_directory ())
          {
            path target (parse_path (*i++, wd));

            // If there are multiple targets but no trailing separator for the
            // link, then, most likelly, it is missing.
            //
            if (i != e)
              error () << "multiple target paths with non-directory link path";

            // Synopsis 1: create a target path symlink at the specified path.
            //
            mksymlink (sp, target, link, cleanup, fail);
          }
          else
          {
            for (; i != e; ++i)
            {
              path target (parse_path (*i, wd));

              // Synopsis 2: create a target path symlink in the specified
              // directory.
              //
              mksymlink (sp, target, link / target.leaf (), cleanup, fail);
            }
          }

          r = 0;
        }
        catch (const invalid_path& e)
        {
          error (false) << "invalid path '" << e.path << "'";
        }
        // Can be thrown while closing in, out or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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

      // Create a directory if not exist and its parent directories if
      // necessary. Throw system_error on failure. Register created
      // directories for cleanup. The directory path must be absolute.
      //
      static void
      mkdir_p (scope& sp, const dir_path& p, bool cleanup)
      {
        if (!dir_exists (p))
        {
          if (!p.root ())
            mkdir_p (sp, p.directory (), cleanup);

          try_mkdir (p); // Returns success or throws.

          if (cleanup)
            sp.clean ({cleanup_type::always, p}, true);
        }
      }

      // mkdir [--no-cleanup] [-p] <dir>...
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

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "mkdir");
        };

        try
        {
          in.close ();
          out.close ();

          auto i (args.begin ());
          auto e (args.end ());

          // Process options.
          //
          bool parent (false);
          bool cleanup (true);
          for (; i != e; ++i)
          {
            const string& o (*i);

            if (o == "-p")
              parent = true;
            else if (o == "--no-cleanup")
              cleanup = false;
            else
            {
              if (*i == "--")
                ++i;

              break;
            }
          }

          // Create directories.
          //
          if (i == e)
            error () << "missing directory";

          for (; i != e; ++i)
          {
            dir_path p (path_cast<dir_path> (parse_path (*i, sp.wd_path)));

            try
            {
              if (parent)
                mkdir_p (sp, p, cleanup);
              else if (try_mkdir (p) == mkdir_status::success)
              {
                if (cleanup)
                  sp.clean ({cleanup_type::always, p}, true);
              }
              else //                == mkdir_status::already_exists
                throw_generic_error (EEXIST);
            }
            catch (const system_error& e)
            {
              error () << "unable to create directory '" << p << "': " << e;
            }
          }

          r = 0;
        }
        catch (const invalid_path& e)
        {
          error (false) << "invalid path '" << e.path << "'";
        }
        // Can be thrown while closing in, out or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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

      // mv [--no-cleanup] [-f] <src-path>    <dst-path>
      // mv [--no-cleanup] [-f] <src-path>... <dst-dir>/
      //
      // Note: can be executed synchronously.
      //
      static uint8_t
      mv (scope& sp,
          const strings& args,
          auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "mv");
        };

        try
        {
          in.close ();
          out.close ();

          auto i (args.begin ());
          auto e (args.end ());

          // Process options.
          //
          bool no_cleanup (false);
          bool force (false);
          for (; i != e; ++i)
          {
            const string& o (*i);

            if (o == "--no-cleanup")
              no_cleanup = true;
            else if (*i == "-f")
              force = true;
            else
            {
              if (o == "--")
                ++i;

              break;
            }
          }

          // Move filesystem entries.
          //
          if (i == e)
            error () << "missing arguments";

          const dir_path& wd (sp.wd_path);

          auto j (args.rbegin ());
          path dst (parse_path (*j++, wd));
          e = j.base ();

          if (i == e)
            error () << "missing source path";

          auto mv = [no_cleanup, force, &wd, &sp, &error] (const path& from,
                                                           const path& to)
          {
            const dir_path& rwd (sp.root->wd_path);

            if (!from.sub (rwd) && !force)
              error () << "'" << from << "' is out of working directory '"
                       << rwd << "'";

            try
            {
              auto check_wd = [&wd, &error] (const path& p)
              {
                if (wd.sub (path_cast<dir_path> (p)))
                  error () << "'" << p << "' contains test working directory '"
                           << wd << "'";
              };

              check_wd (from);
              check_wd (to);

              bool exists (butl::entry_exists (to));

              // Fail if the source and destination paths are the same.
              //
              // Note that for mventry() function (that is based on the POSIX
              // rename() function) this is a noop.
              //
              if (exists && to == from)
                error () << "unable to move entity '" << from << "' to itself";

              // Rename/move the filesystem entry, replacing an existing one.
              //
              mventry (from,
                       to,
                       cpflags::overwrite_permissions |
                       cpflags::overwrite_content);

              // Unless suppressed, adjust the cleanups that are sub-paths of
              // the source path.
              //
              if (!no_cleanup)
              {
                // "Move" the matching cleanup if the destination path doesn't
                // exist and is a sub-path of the working directory. Otherwise
                // just remove it.
                //
                // Note that it's not enough to just change the cleanup paths.
                // We also need to make sure that these cleanups happen before
                // the destination directory (or any of its parents) cleanup,
                // that is potentially registered. To achieve that we can just
                // relocate these cleanup entries to the end of the list,
                // preserving their mutual order. Remember that cleanups in
                // the list are executed in the reversed order.
                //
                bool mv_cleanups (!exists && to.sub (rwd));
                cleanups cs;

                // Remove the source path sub-path cleanups from the list,
                // adjusting/caching them if required (see above).
                //
                for (auto i (sp.cleanups.begin ()); i != sp.cleanups.end (); )
                {
                  cleanup& c (*i);
                  path&    p (c.path);

                  if (p.sub (from))
                  {
                    if (mv_cleanups)
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

                    i = sp.cleanups.erase (i);
                  }
                  else
                    ++i;
                }

                // Re-insert the adjusted cleanups at the end of the list.
                //
                sp.cleanups.insert (sp.cleanups.end (),
                                    make_move_iterator (cs.begin ()),
                                    make_move_iterator (cs.end ()));
              }
            }
            catch (const system_error& e)
            {
              error () << "unable to move entity '" << from << "' to '" << to
                       << "': " << e;
            }
          };

          // If destination is not a directory path (no trailing separator)
          // then move the filesystem entry to the specified path (the only
          // source path is allowed in such a case). Otherwise move the source
          // filesystem entries into the destination directory.
          //
          if (!dst.to_directory ())
          {
            path src (parse_path (*i++, wd));

            // If there are multiple sources but no trailing separator for the
            // destination, then, most likelly, it is missing.
            //
            if (i != e)
              error () << "multiple source paths without trailing separator "
                       << "for destination directory";

            // Synopsis 1: move an entity to the specified path.
            //
            mv (src, dst);
          }
          else
          {
            // Synopsis 2: move entities into the specified directory.
            //
            for (; i != e; ++i)
            {
              path src (parse_path (*i, wd));
              mv (src, dst / src.leaf ());
            }
          }

          r = 0;
        }
        catch (const invalid_path& e)
        {
          error (false) << "invalid path '" << e.path << "'";
        }
        // Can be thrown while closing in, out or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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

      // rm [-r] [-f] <path>...
      //
      // The implementation deviates from POSIX in a number of ways. It doesn't
      // interact with a user and fails immediatelly if unable to process an
      // argument. It doesn't check for dots containment in the path, and
      // doesn't consider files and directory permissions in any way just
      // trying to remove a filesystem entry. Always fails if empty path is
      // specified.
      //
      // Note: can be executed synchronously.
      //
      static uint8_t
      rm (scope& sp,
          const strings& args,
          auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "rm");
        };

        try
        {
          in.close ();
          out.close ();

          auto i (args.begin ());
          auto e (args.end ());

          // Process options.
          //
          bool dir (false);
          bool force (false);
          for (; i != e; ++i)
          {
            if (*i == "-r")
              dir = true;
            else if (*i == "-f")
              force = true;
            else
            {
              if (*i == "--")
                ++i;

              break;
            }
          }

          // Remove entries.
          //
          if (i == e && !force)
            error () << "missing file";

          const dir_path& wd  (sp.wd_path);
          const dir_path& rwd (sp.root->wd_path);

          for (; i != e; ++i)
          {
            path p (parse_path (*i, wd));

            if (!p.sub (rwd) && !force)
              error () << "'" << p << "' is out of working directory '" << rwd
                       << "'";

            try
            {
              dir_path d (path_cast<dir_path> (p));

              if (dir_exists (d))
              {
                if (!dir)
                  error () << "'" << p << "' is a directory";

                if (wd.sub (d))
                  error () << "'" << p << "' contains test working directory '"
                           << wd << "'";

                // The call can result in rmdir_status::not_exist. That's not
                // very likelly but there is also nothing bad about it.
                //
                try_rmdir_r (d);
              }
              else if (try_rmfile (p) == rmfile_status::not_exist && !force)
                throw_generic_error (ENOENT);
            }
            catch (const system_error& e)
            {
              error () << "unable to remove '" << p << "': " << e;
            }
          }

          r = 0;
        }
        catch (const invalid_path& e)
        {
          error (false) << "invalid path '" << e.path << "'";
        }
        // Can be thrown while closing in, out or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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

      // rmdir [-f] <path>...
      //
      // Note: can be executed synchronously.
      //
      static uint8_t
      rmdir (scope& sp,
             const strings& args,
             auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "rmdir");
        };

        try
        {
          in.close ();
          out.close ();

          auto i (args.begin ());
          auto e (args.end ());

          // Process options.
          //
          bool force (false);
          for (; i != e; ++i)
          {
            if (*i == "-f")
              force = true;
            else
            {
              if (*i == "--")
                ++i;

              break;
            }
          }

          // Remove directories.
          //
          if (i == e && !force)
            error () << "missing directory";

          const dir_path& wd  (sp.wd_path);
          const dir_path& rwd (sp.root->wd_path);

          for (; i != e; ++i)
          {
            dir_path p (path_cast<dir_path> (parse_path (*i, wd)));

            if (wd.sub (p))
              error () << "'" << p << "' contains test working directory '"
                       << wd << "'";

            if (!p.sub (rwd) && !force)
              error () << "'" << p << "' is out of working directory '"
                       << rwd << "'";

            try
            {
              rmdir_status s (try_rmdir (p));

              if (s == rmdir_status::not_empty)
                throw_generic_error (ENOTEMPTY);
              else if (s == rmdir_status::not_exist && !force)
                throw_generic_error (ENOENT);
            }
            catch (const system_error& e)
            {
              error () << "unable to remove '" << p << "': " << e;
            }
          }

          r = 0;
        }
        catch (const invalid_path& e)
        {
          error (false) << "invalid path '" << e.path << "'";
        }
        // Can be thrown while closing in, out or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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

      // sed [-n] [-i] -e <script> [<file>]
      //
      // Note: must be executed asynchronously.
      //
      static uint8_t
      sed (scope& sp,
           const strings& args,
           auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "sed");
        };

        try
        {
          // Automatically remove a temporary file (used for in place editing)
          // on failure.
          //
          auto_rmfile rm;

          // Do not throw when failbit is set (getline() failed to extract any
          // character).
          //
          ifdstream cin  (move (in), ifdstream::badbit);
          ofdstream cout (move (out));

          auto i (args.begin ());
          auto e (args.end ());

          // Process options.
          //
          bool auto_prn (true);
          bool in_place (false);

          struct substitute
          {
            string regex;
            string replacement;
            bool icase  = false;
            bool global = false;
            bool print  = false;
          };
          optional<substitute> subst;

          for (; i != e; ++i)
          {
            const string& o (*i);

            if (o == "-n")
              auto_prn = false;
            else if (o == "-i")
              in_place = true;
            else if (o == "-e")
            {
              // Only a single script is supported.
              //
              if (subst)
                error () << "multiple scripts";

              // If option has no value then bail out and report.
              //
              if (++i == e)
                break;

              const string& v (*i);
              if (v.empty ())
                error () << "empty script";

              if (v[0] != 's')
                error () << "only 's' command supported";

              // Parse the substitute command.
              //
              if (v.size () < 2)
                error () << "no delimiter for 's' command";

              char delim (v[1]);
              if (delim == '\\' || delim == '\n')
                error () << "invalid delimiter for 's' command";

              size_t p (v.find (delim, 2));
              if (p == string::npos)
                error () << "unterminated 's' command regex";

              subst = substitute ();
              subst->regex.assign (v, 2, p - 2);

              // Empty regex matches nothing, so not of much use.
              //
              if (subst->regex.empty ())
                error () << "empty regex in 's' command";

              size_t b (p + 1);
              p = v.find (delim, b);
              if (p == string::npos)
                error () << "unterminated 's' command replacement";

              subst->replacement.assign (v, b, p - b);

              // Parse the substitute command flags.
              //
              char c;
              for (++p; (c = v[p]) != '\0'; ++p)
              {
                switch (c)
                {
                case 'i': subst->icase  = true; break;
                case 'g': subst->global = true; break;
                case 'p': subst->print  = true; break;
                default:
                  {
                    error () << "invalid 's' command flag '" << c << "'";
                  }
                }
              }
            }
            else
            {
              if (o == "--")
                ++i;

              break;
            }
          }

          if (!subst)
            error () << "missing script";

          // Path of a file to edit. An empty path represents stdin.
          //
          path p;
          if (i != e)
          {
            if (*i != "-")
              p = parse_path (*i, sp.wd_path);

            ++i;
          }

          if (i != e)
            error () << "unexpected argument '" << *i << "'";

          // If we edit file in place make sure that the file path is specified
          // and obtain a temporary file path. We will be writing to the
          // temporary file (rather than to stdout) and will move it to the
          // original file path afterwards.
          //
          path tp;
          if (in_place)
          {
            if (p.empty ())
              error () << "-i option specified while reading from stdin";

            try
            {
              tp = path::temp_path ("build2-sed");

              cout.close ();  // Flush and close.

              cout.open (
                fdopen (tp,
                        fdopen_mode::out | fdopen_mode::truncate |
                        fdopen_mode::create,
                        path_permissions (p)));
            }
            catch (const io_error& e)
            {
              error_record d (error ());
              d << "unable to open '" << tp << "': " << e;
            }
            catch (const system_error& e)
            {
              error_record d (error ());
              d << "unable to obtain temporary file: " << e;
            }

            rm = auto_rmfile (tp);
          }

          // Note that ECMAScript is implied if no grammar flag is specified.
          //
          regex re (subst->regex,
                    subst->icase ? regex::icase : regex::ECMAScript);

          // Edit a file or STDIN.
          //
          try
          {
            // Open a file if specified.
            //
            if (!p.empty ())
            {
              cin.close (); // Flush and close.
              cin.open (p);
            }

            // Read until failbit is set (throw on badbit).
            //
            string s;
            while (getline (cin, s))
            {
              auto r (regex_replace_search (
                        s,
                        re,
                        subst->replacement,
                        subst->global
                        ? regex_constants::format_default
                        : regex_constants::format_first_only));

              // Add newline regardless whether the source line is newline-
              // terminated or not (in accordance with POSIX).
              //
              if (auto_prn || (r.second && subst->print))
                cout << r.first << '\n';
            }

            cin.close ();
            cout.close ();

            if (in_place)
            {
              mvfile (
                tp, p,
                cpflags::overwrite_content | cpflags::overwrite_permissions);

              rm.cancel ();
            }

            r = 0;
          }
          catch (const io_error& e)
          {
            error_record d (error ());
            d << "unable to edit ";

            if (p.empty ())
              d << "stdin";
            else
              d << "'" << p << "'";

            d << ": " << e;
          }
        }
        catch (const regex_error& e)
        {
          // Print regex_error description if meaningful (no space).
          //
          error (false) << "invalid regex" << e;
        }
        catch (const invalid_path& e)
        {
          error (false) << "invalid path '" << e.path << "'";
        }
        // Can be thrown while creating cin, cout or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
        }
        catch (const system_error& e)
        {
          error (false) << e;
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

      // sleep <seconds>
      //
      // Note: can be executed synchronously.
      //
      static uint8_t
      sleep (scope&,
             const strings& args,
             auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (1);
        ofdstream cerr (move (err));

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "sleep");
        };

        try
        {
          in.close ();
          out.close ();

          if (args.empty ())
            error () << "missing time interval";

          if (args.size () > 1)
            error () << "unexpected argument '" << args[1] << "'";

          uint64_t n;

          for (;;) // Breakout loop.
          {
            const string& a (args[0]);

            // Note: strtoull() allows these.
            //
            if (!a.empty () && a[0] != '-' && a[0] != '+')
            {
              char* e (nullptr);
              n = strtoull (a.c_str (), &e, 10); // Can't throw.

              if (errno != ERANGE && e == a.c_str () + a.size ())
                break;
            }

            error () << "invalid time interval '" << a << "'";
          }

          // If/when required we could probably support the precise sleep mode
          // (e.g., via an option).
          //
          sched.sleep (chrono::seconds (n));

          r = 0;
        }
        // Can be thrown while closing in, out or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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

      // test -f|-d <path>
      //
      // Note: can be executed synchronously.
      //
      static uint8_t
      test (scope& sp,
            const strings& args,
            auto_fd in, auto_fd out, auto_fd err) noexcept
      try
      {
        uint8_t r (2);
        ofdstream cerr (move (err));

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "test");
        };

        try
        {
          in.close ();
          out.close ();

          if (args.size () < 2)
            error () << "missing path";

          bool file (args[0] == "-f");

          if (!file && args[0] != "-d")
            error () << "invalid option";

          if (args.size () > 2)
            error () << "unexpected argument '" << args[2] << "'";

          path p (parse_path (args[1], sp.wd_path));

          try
          {
            r = (file ? file_exists (p) : dir_exists (p)) ? 0 : 1;
          }
          catch (const system_error& e)
          {
            error () << "cannot test '" << p << "': " << e;
          }
        }
        catch (const invalid_path& e)
        {
          error (false)  << "invalid path '" << e.path << "'";
        }
        // Can be thrown while closing in, out or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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
        return 2;
      }

      // touch [--no-cleanup] [--after <ref-file>] <file>...
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

        auto error = [&cerr] (bool fail = true)
        {
          return error_record (cerr, fail, "touch");
        };

        try
        {
          in.close ();
          out.close ();

          auto mtime = [&error] (const path& p) -> timestamp
          {
            try
            {
              timestamp t (file_mtime (p));

              if (t == timestamp_nonexistent)
                throw_generic_error (ENOENT);

              return t;
            }
            catch (const system_error& e)
            {
              error () << "cannot obtain file '" << p
                       << "' modification time: " << e;
            }
            assert (false); // Can't be here.
            return timestamp ();
          };

          auto i (args.begin ());
          auto e (args.end ());

          // Process options.
          //
          bool cleanup (true);
          optional<timestamp> after;
          for (; i != e; ++i)
          {
            const string& o (*i);

            if (o == "--no-cleanup")
              cleanup = false;
            else if (o == "--after")
            {
              if (++i == e)
                error () << "missing --after option value";

              after = mtime (parse_path (*i, sp.wd_path));
            }
            else
            {
              if (o == "--")
                ++i;

              break;
            }
          }

          if (i == e)
            error () << "missing file";

          // Create files.
          //
          for (; i != e; ++i)
          {
            path p (parse_path (*i, sp.wd_path));

            try
            {
              // Note that we don't register (implicit) cleanup for an
              // existing path.
              //
              if (touch_file (p) && cleanup)
                sp.clean ({cleanup_type::always, p}, true);

              if (after)
              {
                while (mtime (p) <= *after)
                  touch_file (p, false /* create */);
              }
            }
            catch (const system_error& e)
            {
              error () << "cannot create/update '" << p << "': " << e;
            }
          }

          r = 0;
        }
        catch (const invalid_path& e)
        {
          error (false) << "invalid path '" << e.path << "'";
        }
        // Can be thrown while closing in, out or writing to cerr.
        //
        catch (const io_error& e)
        {
          error (false) << e;
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

      // Run builtin implementation asynchronously.
      //
      static builtin
      async_impl (builtin_impl* fn,
                  scope& sp,
                  uint8_t& r,
                  const strings& args,
                  auto_fd in, auto_fd out, auto_fd err)
      {
        return builtin (
          r,
          thread ([fn, &sp, &r, &args,
                   in  = move (in),
                   out = move (out),
                   err = move (err)] () mutable noexcept
                  {
                    r = fn (sp, args, move (in), move (out), move (err));
                  }));
      }

      template <builtin_impl fn>
      static builtin
      async_impl (scope& sp,
                  uint8_t& r,
                  const strings& args,
                  auto_fd in, auto_fd out, auto_fd err)
      {
        return async_impl (fn, sp, r, args, move (in), move (out), move (err));
      }

      // Run builtin implementation synchronously.
      //
      template <builtin_impl fn>
      static builtin
      sync_impl (scope& sp,
                 uint8_t& r,
                 const strings& args,
                 auto_fd in, auto_fd out, auto_fd err)
      {
        r = fn (sp, args, move (in), move (out), move (err));
        return builtin (r, thread ());
      }

      const builtin_map builtins
      {
        {"cat",   &async_impl<&cat>},
        {"cp",    &sync_impl<&cp>},
        {"echo",  &async_impl<&echo>},
        {"false", &false_},
        {"ln",    &sync_impl<&ln>},
        {"mkdir", &sync_impl<&mkdir>},
        {"mv",    &sync_impl<&mv>},
        {"rm",    &sync_impl<&rm>},
        {"rmdir", &sync_impl<&rmdir>},
        {"sed",   &async_impl<&sed>},
        {"sleep", &sync_impl<&sleep>},
        {"test",  &sync_impl<&test>},
        {"touch", &sync_impl<&touch>},
        {"true",  &true_}
      };
    }
  }
}
