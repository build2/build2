// file      : libbuild2/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/utility.hxx>

#ifndef _WIN32
#  include <signal.h> // signal()
#else
#  include <libbutl/win32-utility.hxx>
#endif

#include <time.h>   // tzset() (POSIX), _tzset() (Windows)

#ifdef __GLIBCXX__
#  include <locale>
#endif

#include <cerrno>   // ENOENT
#include <cstring>  // strlen(), str[n]cmp()
#include <iostream> // cerr

#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/script/regex.hxx> // script::regex::init()

using namespace std;
using namespace butl;

// <libbuild2/types.hxx>
//
namespace build2
{
  static const char* const run_phase_[] = {"load", "match", "execute"};

  ostream&
  operator<< (ostream& os, run_phase p)
  {
    return os << run_phase_[static_cast<uint8_t> (p)];
  }

  ostream&
  operator<< (ostream& os, const path& p)
  {
    using namespace build2;

    if (stream_verb (os).path < 1)
      return os << diag_relative (p);
    else
      return to_stream (os, p, true /* representation */);
  }

  ostream&
  operator<< (ostream& os, const path_name_view& v)
  {
    assert (!v.empty ());

    return v.name != nullptr && *v.name ? (os << **v.name) : (os << *v.path);
  }

  ostream&
  operator<< (ostream& os, const process_path& p)
  {
    using namespace build2;

    if (p.empty ())
      os << "<empty>";
    else
    {
      // @@ Is there a reason not to print as a relative path as it is done
      //    for path (see above)?
      //
      os << p.recall_string ();

      if (!p.effect.empty ())
        os << '@' << p.effect.string (); // Suppress relative().
    }

    return os;
  }
}

// <libbuild2/utility.hxx>
//
namespace build2
{
  static const char hex_digits[] = "0123456789abcdef";

  string
  to_string (uint64_t i, int b, size_t w)
  {
    // One day we can switch to C++17 std::to_chars().
    //
    string r;
    switch (b)
    {
    case 10:
      {
        r = to_string (i);
        if (w > r.size ())
          r.insert (0, w - r.size (), '0');
        break;
      }
    case 16:
      {
        r.reserve (18);
        r += "0x";

        for (size_t j (64); j != 0; )
        {
          j -= 4;
          size_t d ((i >> j) & 0x0f);

          // Omit leading zeros but watch out for the i==0 corner case.
          //
          if (d != 0 || r.size () != 2 || j == 0)
            r += hex_digits[d];
        }

        if (w > r.size () - 2)
          r.insert (2, w - (r.size () - 2), '0');

        break;
      }
    default:
      throw invalid_argument ("unsupported base");
    }

    return r;
  }

  void (*terminate) (bool);

  process_path argv0;

  const standard_version build_version (LIBBUILD2_VERSION_STR);
  const string build_version_interface (
    build_version.pre_release ()
    ? build_version.string_project_id ()
    : (to_string (build_version.major ()) + '.' +
       to_string (build_version.minor ())));

  optional<bool> mtime_check_option;

  optional<path> config_sub;
  optional<path> config_guess;

  void
  check_build_version (const standard_version_constraint& c, const location& l)
  {
    if (!c.satisfies (build_version))
      fail (l) << "incompatible build2 version" <<
        info << "running " << build_version.string () <<
        info << "required " << c.string ();
  }

  dir_path work;
  dir_path home;
  const dir_path* relative_base = &work;

  path
  relative (const path_target& t)
  {
    const path& p (t.path ());
    assert (!p.empty ());
    return relative (p);
  }

  string
  diag_relative (const path& p, bool cur)
  {
    const path& b (*relative_base);

    if (p.absolute ())
    {
      if (p == b)
        return cur ? '.' + p.separator_string () : string ();

#ifndef _WIN32
      if (!home.empty ())
      {
        if (p == home)
          return '~' + p.separator_string ();
      }
#endif

      path rb (relative (p));

#ifndef _WIN32
      if (!home.empty ())
      {
        if (rb.relative ())
        {
          // See if the original path with the ~/ shortcut is better that the
          // relative to base.
          //
          if (p.sub (home))
          {
            path rh (p.leaf (home));
            if (rb.size () > rh.size () + 2) // 2 for '~/'
              return "~/" + move (rh).representation ();
          }
        }
        else if (rb.sub (home))
          return "~/" + rb.leaf (home).representation ();
      }
#endif

      return move (rb).representation ();
    }

    return p.representation ();
  }

  process_path
  run_search (const char*& args0, bool path_only, const location& l)
  try
  {
    return process::path_search (args0, dir_path () /* fallback */, path_only);
  }
  catch (const process_error& e)
  {
    fail (l) << "unable to execute " << args0 << ": " << e << endf;
  }

  process_path
  run_search (const path& f,
              bool init,
              const dir_path& fallback,
              bool path_only,
              const location& l)
  try
  {
    return process::path_search (f, init, fallback, path_only);
  }
  catch (const process_error& e)
  {
    fail (l) << "unable to execute " << f << ": " << e << endf;
  }

  process_path
  run_try_search (const path& f,
                  bool init,
                  const dir_path& fallback,
                  bool path_only,
                  const char* paths)
  {
    return process::try_path_search (f, init, fallback, path_only, paths);
  }

  [[noreturn]] void
  run_search_fail (const path& f, const location& l)
  {
    fail (l) << "unable to execute " << f << ": " << process_error (ENOENT)
             << endf;
  }

  process
  run_start (uint16_t verbosity,
             const process_env& pe,
             const char* const* args,
             int in,
             int out,
             int err,
             const location& l)
  try
  {
    assert (args[0] == pe.path->recall_string ());

    if (verb >= verbosity)
      print_process (pe, args, 0);

    return process (
      *pe.path,
      args,
      in,
      out,
      err,
      pe.cwd != nullptr ? pe.cwd->string ().c_str () : nullptr,
      pe.vars);
  }
  catch (const process_error& e)
  {
    if (e.child)
    {
      // Note: run_finish_impl() below expects this exact message.
      //
      cerr << "unable to execute " << args[0] << ": " << e << endl;

      // In a multi-threaded program that fork()'ed but did not exec(), it is
      // unwise to try to do any kind of cleanup (like unwinding the stack and
      // running destructors).
      //
      exit (1);
    }
    else
      fail (l) << "unable to execute " << args[0] << ": " << e << endf;
  }

  bool
  run_wait (const char* const* args, process& pr, const location& loc)
  try
  {
    return pr.wait ();
  }
  catch (const process_error& e)
  {
    fail (loc) << "unable to execute " << args[0] << ": " << e << endf;
  }

  bool
  run_finish_impl (const char* const* args,
                   process& pr,
                   bool f,
                   const string& l,
                   uint16_t v,
                   bool omit_normal,
                   const location& loc)
  {
    tracer trace ("run_finish");

    try
    {
      if (pr.wait ())
        return true;
    }
    catch (const process_error& e)
    {
      fail (loc) << "unable to execute " << args[0] << ": " << e << endf;
    }

    // Note: see similar code in diag_buffer::close().
    //
    const process_exit& pe (*pr.exit);
    bool ne (pe.normal ());

    // Even if the user redirected the diagnostics, one error that we want to
    // let through is the inability to execute the program itself. We cannot
    // reserve a special exit status to signal this so we will just have to
    // compare the output. In a sense, we treat this as a special case of
    // abnormal termination. This particular situation will result in a single
    // error line printed by run_start() above.
    //
    if (ne && l.compare (0, 18, "unable to execute ") == 0)
      fail (loc) << l;

    if (omit_normal && ne)
    {
      // While we assume diagnostics has already been issued (to stderr), if
      // that's not the case, it's a real pain to debug. So trace it. (And
      // if you think that doesn't happen in sensible programs, check GCC
      // bug #107448).
      //
      l4 ([&]{trace << "process " << args[0] << " " << pe;});
    }
    else
    {
      // It's unclear whether we should print this only if printing the
      // command line (we could also do things differently for normal/abnormal
      // exit). Let's print this always and see how it wears. Note that we now
      // rely on this in, for example, process_finish(), extract_metadata().
      //
      // Note: make sure keep the above trace if decide not to print.
      //
      diag_record dr;
      dr << error (loc) << "process " << args[0] << " " << pe;

      if (verb >= 1 && verb <= v)
      {
        dr << info << "command line: ";
        print_process (dr, args);
      }
    }

    if (f || !ne)
      throw failed ();

    return false;
  }

  bool
  run_finish_impl (diag_buffer& dbuf,
                   const char* const* args,
                   process& pr,
                   bool f,
                   uint16_t v,
                   bool on,
                   const location& loc)
  {
    try
    {
      pr.wait ();
    }
    catch (const process_error& e)
    {
      fail (loc) << "unable to execute " << args[0] << ": " << e << endf;
    }

    const process_exit& pe (*pr.exit);

    dbuf.close (args, pe, v, on, loc);

    if (pe)
      return true;

    if (f || !pe.normal ())
      throw failed ();

    return false;
  }

  void
  run (context& ctx,
       const process_env& pe,
       const char* const* args,
       uint16_t v)
  {
    if (ctx.phase == run_phase::load)
    {
      process pr (run_start (pe, args));
      run_finish (args, pr, v);
    }
    else
    {
      process pr (run_start (pe,
                             args,
                             0                       /* stdin  */,
                             1                       /* stdout */,
                             diag_buffer::pipe (ctx) /* stderr */));
      diag_buffer dbuf (ctx, args[0], pr);
      dbuf.read ();
      run_finish (dbuf, args, pr, v);
    }
  }

  bool
  run (context& ctx,
       uint16_t verbosity,
       const process_env& pe,
       const char* const* args,
       uint16_t finish_verbosity,
       const function<bool (string&, bool)>& f,
       bool tr,
       bool err,
       bool ignore_exit,
       sha256* checksum)
  {
    assert (!err || !ignore_exit);

    if (!err || ctx.phase == run_phase::load)
    {
      process pr (run_start (verbosity,
                             pe,
                             args,
                             0           /* stdin */,
                             -1          /* stdout */,
                             err ? 2 : 1 /* stderr */));

      string l; // Last line of output.
      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

        bool empty (true);

        // Make sure we keep the last line.
        //
        for (bool last (is.peek () == ifdstream::traits_type::eof ());
             !last && getline (is, l); )
        {
          last = (is.peek () == ifdstream::traits_type::eof ());

          if (tr)
            trim (l);

          if (checksum != nullptr)
            checksum->append (l);

          if (empty)
          {
            empty = f (l, last);

            if (!empty && checksum == nullptr)
              break;
          }
        }

        is.close ();
      }
      catch (const io_error& e)
      {
        if (run_wait (args, pr))
          fail << "io error reading " << args[0] << " output: " << e << endf;

        // If the child process has failed then assume the io error was
        // caused by that and let run_finish() deal with it.
      }

      // Omit normal exit code diagnostics if err is false.
      //
      if (!(run_finish_impl (args, pr, err, l, finish_verbosity, !err) ||
            ignore_exit))
        return false;
    }
    else
    {
      // We have to use the non-blocking setup since we have to read from stdout
      // and stderr simultaneously.
      //
      process pr (run_start (verbosity,
                             pe,
                             args,
                             0                       /* stdin */,
                             -1                      /* stdout */,
                             diag_buffer::pipe (ctx) /* stderr */));

      // Note that while we read both streams until eof in the normal
      // circumstances, we cannot use fdstream_mode::skip for the exception
      // case on both of them: we may end up being blocked trying to read one
      // stream while the process may be blocked writing to the other. So in
      // case of an exception we only skip the diagnostics and close stdout
      // hard. The latter should happen first so the order of the dbuf/is
      // variables is important.
      //
      diag_buffer dbuf (ctx, args[0], pr, (fdstream_mode::non_blocking |
                                           fdstream_mode::skip));
      try
      {
        ifdstream is (move (pr.in_ofd),
                      fdstream_mode::non_blocking,
                      ifdstream::badbit);

        bool empty (true);

        // Read until we reach EOF on all streams.
        //
        // Note that if dbuf is not opened, then we automatically get an
        // inactive nullfd entry.
        //
        fdselect_set fds {is.fd (), dbuf.is.fd ()};
        fdselect_state& ist (fds[0]);
        fdselect_state& dst (fds[1]);

        // To detect the last line we are going keep the previous line and
        // only call the function once we've read the next.
        //
        optional<string> pl;

        for (string l; ist.fd != nullfd || dst.fd != nullfd; )
        {
          if (ist.fd != nullfd && getline_non_blocking (is, l))
          {
            if (eof (is))
            {
              if (pl && empty)
                f (*pl, true /* last */);

              ist.fd = nullfd;
            }
            else
            {
              if (checksum != nullptr || empty)
              {
                if (tr)
                  trim (l);

                if (checksum != nullptr)
                  checksum->append (l);

                if (empty)
                {
                  if (pl)
                  {
                    if ((empty = f (*pl, false /* last */)))
                      swap (l, *pl);

                    // Note that we cannot bail out like in the other version
                    // since we don't have the skip mode on is. Plus, we might
                    // still have the diagnostics.
                  }
                  else
                    pl = move (l);
                }
              }

              l.clear ();
            }

            continue;
          }

          ifdselect (fds);

          if (dst.ready)
          {
            if (!dbuf.read ())
              dst.fd = nullfd;
          }
        }

        is.close ();
      }
      catch (const io_error& e)
      {
        if (run_wait (args, pr))
        {
          // Note that we will drop the diagnostics in this case since reading
          // it could have been the cause of this error.
          //
          fail << "io error reading " << args[0] << " output: " << e << endf;
        }

        // If the child process has failed then assume the io error was caused
        // by that and let run_finish() deal with it.
      }

      run_finish_impl (dbuf, args, pr, true /* fail */, finish_verbosity);
    }

    return true;
  }

  cstrings
  process_args (const char* program, const strings& args)
  {
    cstrings r;
    r.reserve (args.size () + 2);

    r.push_back (program);

    for (const string& a: args)
      r.push_back (a.c_str ());

    r.push_back (nullptr);
    return r;
  }

  fdpipe
  open_pipe ()
  {
    try
    {
      return fdopen_pipe ();
    }
    catch (const io_error& e)
    {
      fail << "unable to open pipe: " << e << endf;
    }
  }

  auto_fd
  open_null ()
  {
    try
    {
      return fdopen_null ();
    }
    catch (const io_error& e)
    {
      fail << "unable to open null device: " << e << endf;
    }
  }

  const string       empty_string;
  const path         empty_path;
  const dir_path     empty_dir_path;
  const project_name empty_project_name;

  const optional<string>       nullopt_string;
  const optional<path>         nullopt_path;
  const optional<dir_path>     nullopt_dir_path;
  const optional<project_name> nullopt_project_name;

  void
  append_options (cstrings& args, const lookup& l, const char* e)
  {
    if (l)
      append_options (args, cast<strings> (l), e);
  }

  void
  append_options (strings& args, const lookup& l, const char* e)
  {
    if (l)
      append_options (args, cast<strings> (l), e);
  }

  void
  append_options (sha256& csum, const lookup& l)
  {
    if (l)
      append_options (csum, cast<strings> (l));
  }

  void
  append_options (cstrings& args, const strings& sv, size_t n, const char* e)
  {
    if (n != 0)
    {
      args.reserve (args.size () + n);

      for (size_t i (0); i != n; ++i)
      {
        if (e == nullptr || e != sv[i])
          args.push_back (sv[i].c_str ());
      }
    }
  }

  void
  append_options (strings& args, const strings& sv, size_t n, const char* e)
  {
    if (n != 0)
    {
      args.reserve (args.size () + n);

      for (size_t i (0); i != n; ++i)
      {
        if (e == nullptr || e != sv[i])
          args.push_back (sv[i]);
      }
    }
  }

  void
  append_options (sha256& csum, const strings& sv, size_t n)
  {
    for (size_t i (0); i != n; ++i)
      csum.append (sv[i]);
  }

  bool
  find_option (const char* o, const lookup& l, bool ic)
  {
    return l && find_option (o, cast<strings> (l), ic);
  }

  bool
  find_option (const char* o, const strings& strs, bool ic)
  {
    for (const string& s: strs)
      if (ic ? icasecmp (s, o) == 0 : s == o)
        return true;

    return false;
  }

  bool
  find_option (const char* o, const cstrings& cstrs, bool ic)
  {
    for (const char* s: cstrs)
      if (s != nullptr && (ic ? icasecmp (s, o) : strcmp (s, o)) == 0)
        return true;

    return false;
  }

  bool
  find_options (const initializer_list<const char*>& os,
                const lookup& l,
                bool ic)
  {
    return l && find_options (os, cast<strings> (l), ic);
  }

  bool
  find_options (const initializer_list<const char*>& os,
                const strings& strs,
                bool ic)
  {
    for (const string& s: strs)
      for (const char* o: os)
        if (ic ? icasecmp (s, o) == 0 : s == o)
          return true;

    return false;
  }

  bool
  find_options (const initializer_list<const char*>& os,
                const cstrings& cstrs,
                bool ic)
  {
    for (const char* s: cstrs)
      if (s != nullptr)
        for (const char* o: os)
          if ((ic ? icasecmp (s, o) : strcmp (s, o)) == 0)
            return true;

    return false;
  }

  const string*
  find_option_prefix (const char* p, const lookup& l, bool ic)
  {
    return l ? find_option_prefix (p, cast<strings> (l), ic) : nullptr;
  }

  const string*
  find_option_prefix (const char* p, const strings& strs, bool ic)
  {
    size_t n (strlen (p));

    for (const string& s: reverse_iterate (strs))
      if ((ic ? icasecmp (s, p, n) : s.compare (0, n, p)) == 0)
        return &s;

    return nullptr;
  }

  const char*
  find_option_prefix (const char* p, const cstrings& cstrs, bool ic)
  {
    size_t n (strlen (p));

    for (const char* s: reverse_iterate (cstrs))
      if (s != nullptr && (ic ? icasecmp (s, p, n) : strncmp (s, p, n)) == 0)
        return s;

    return nullptr;
  }

  const string*
  find_option_prefixes (const initializer_list<const char*>& ps,
                        const lookup& l,
                        bool ic)
  {
    return l ? find_option_prefixes (ps, cast<strings> (l), ic) : nullptr;
  }

  const string*
  find_option_prefixes (const initializer_list<const char*>& ps,
                        const strings& strs,
                        bool ic)
  {
    for (const string& s: reverse_iterate (strs))
      for (const char* p: ps)
        if ((ic
             ? icasecmp (s, p, strlen (p))
             : s.compare (0, strlen (p), p)) == 0)
          return &s;

    return nullptr;
  }

  const char*
  find_option_prefixes (const initializer_list<const char*>& ps,
                        const cstrings& cstrs,
                        bool ic)
  {
    for (const char* s: reverse_iterate (cstrs))
      if (s != nullptr)
        for (const char* p: ps)
          if ((ic
               ? icasecmp (s, p, strlen (p))
               : strncmp (s, p, strlen (p))) == 0)
            return s;

    return nullptr;
  }

  string
  apply_pattern (const char* stem, const char* pat)
  {
    if (pat == nullptr || *pat == '\0')
      return stem;

    size_t n (string::traits_type::length (pat));
    const char* p (string::traits_type::find (pat, n, '*'));
    assert (p != nullptr);

    string r (pat, p++ - pat);
    r.append (stem);
    r.append (p, n - (p - pat));
    return r;
  }

  void
  init_process ()
  {
    // This is a little hack to make out baseutils for Windows work when
    // called with absolute path. In a nutshell, MSYS2's exec*p() doesn't
    // search in the parent's executable directory, only in PATH. And since we
    // are running without a shell (that would read /etc/profile which sets
    // PATH to some sensible values), we are only getting Win32 PATH values.
    // And MSYS2 /bin is not one of them. So what we are going to do is add
    // /bin at the end of PATH (which will be passed as is by the MSYS2
    // machinery). This will make MSYS2 search in /bin (where our baseutils
    // live). And for everyone else this should be harmless since it is not a
    // valid Win32 path.
    //
#ifdef _WIN32
    {
      string mp;
      if (optional<string> p = getenv ("PATH"))
      {
        mp = move (*p);
        mp += ';';
      }
      mp += "/bin";

      setenv ("PATH", mp);
    }
#endif

    // On POSIX ignore SIGPIPE which is signaled to a pipe-writing process if
    // the pipe reading end is closed. Note that by default this signal
    // terminates a process. Also note that there is no way to disable this
    // behavior on a file descriptor basis or for the write() function call.
    //
#ifndef _WIN32
    if (signal (SIGPIPE, SIG_IGN) == SIG_ERR)
      fail << "unable to ignore broken pipe (SIGPIPE) signal: "
           << system_error (errno, generic_category ()); // Sanitize.
#endif

    // Initialize time conversion data that is used by localtime_r().
    //
#ifndef _WIN32
    tzset ();
#else
    _tzset ();
#endif

    // A data race happens in the libstdc++ (as of GCC 7.2) implementation of
    // the ctype<char>::narrow() function (bug #77704). The issue is easily
    // triggered by the testscript runner that indirectly (via regex) uses
    // ctype<char> facet of the global locale (and can potentially be
    // triggered by other locale-aware code). We work around this by
    // pre-initializing the global locale facet internal cache.
    //
#ifdef __GLIBCXX__
    {
      const ctype<char>& ct (use_facet<ctype<char>> (locale ()));

      for (size_t i (0); i != 256; ++i)
        ct.narrow (static_cast<char> (i), '\0');
    }
#endif
  }

  void
  init (void (*t) (bool),
        const char* a0,
        bool ss,
        optional<bool> mc,
        optional<path> cs,
        optional<path> cg)
  {
    terminate = t;

    argv0 = process::path_search (a0, true);

    mtime_check_option = mc;

    config_sub = move (cs);
    config_guess = move (cg);

    // Figure out work and home directories.
    //
    try
    {
      work = dir_path::current_directory ();
    }
    catch (const system_error& e)
    {
      fail << "invalid current working directory: " << e;
    }

    try
    {
      home = dir_path::home_directory ();
    }
    catch (const system_error& e)
    {
      fail << "unable to obtain home directory: " << e;
    }

    script::regex::init ();

    if (!ss)
    {
#ifdef _WIN32
      // On Windows disable displaying error reporting dialog box for the
      // current and child processes unless we are in the stop mode. Failed
      // that we may have multiple dialog boxes popping up.
      //
      SetErrorMode (SetErrorMode (0) | // Returns the current mode.
                    SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    }
  }

  optional<uint64_t>
  parse_number (const string& s, uint64_t max_num)
  {
    optional<uint64_t> r;

    if (!s.empty ())
    {
      const char* b (s.c_str ());
      char* e (nullptr);
      errno = 0; // We must clear it according to POSIX.
      uint64_t v (strtoull (b, &e, 10)); // Can't throw.

      if (errno != ERANGE && e == b + s.size () && v <= max_num)
        r = v;
    }

    return r;
  }
}
