// file      : libbuild2/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/utility.hxx>

#include <time.h>   // tzset() (POSIX), _tzset() (Windows)

#include <cstring>  // strlen(), str[n]cmp()
#include <iostream> // cerr

#include <libbuild2/target.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

//
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
}

namespace std
{
  ostream&
  operator<< (ostream& os, const ::butl::path& p)
  {
    using namespace build2;

    return os << (stream_verb (os).path < 1
                  ? diag_relative (p)
                  : p.representation ());
  }

  ostream&
  operator<< (ostream& os, const ::butl::process_path& p)
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

namespace build2
{
  //
  // <libbuild2/utility.hxx>
  //
  void (*terminate) (bool);

  process_path argv0;

  const standard_version build_version (LIBBUILD2_VERSION_STR);
  const string build_version_interface (
    build_version.pre_release ()
    ? build_version.string_project_id ()
    : (to_string (build_version.major ()) + '.' +
       to_string (build_version.minor ())));

  bool dry_run_option;
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
    if (p.string () == "-")
      return "<stdin>";

    const path& b (*relative_base);

    if (p.absolute ())
    {
      if (p == b)
        return cur ? "." + p.separator_string () : string ();

#ifndef _WIN32
      if (!home.empty ())
      {
        if (p == home)
          return "~" + p.separator_string ();
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
  try_run_search (const path& f,
                  bool init,
                  const dir_path& fallback,
                  bool path_only)
  {
    return process::try_path_search (f, init, fallback, path_only);
  }

  process
  run_start (uint16_t verbosity,
             const process_env& pe,
             const char* args[],
             int in,
             int out,
             bool err,
             const dir_path& cwd,
             const location& l)
  try
  {
    assert (args[0] == pe.path->recall_string ());

    if (verb >= verbosity)
      print_process (args, 0);

    return process (
      *pe.path,
      args,
      in,
      out,
      (err ? 2 : 1),
      (!cwd.empty ()
       ? cwd.string ().c_str ()
       : pe.cwd != nullptr ? pe.cwd->string ().c_str () : nullptr),
      pe.vars);
  }
  catch (const process_error& e)
  {
    if (e.child)
    {
      // Note: run_finish() expects this exact message.
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
  run_finish (const char* args[],
              process& pr,
              bool err,
              const string& l,
              const location& loc)
  try
  {
    tracer trace ("run_finish");

    if (pr.wait ())
      return true;

    const process_exit& e (*pr.exit);

    if (!e.normal ())
      fail (loc) << "process " << args[0] << " " << e;

    // Normall but non-zero exit status.
    //
    if (err)
    {
      // While we assuming diagnostics has already been issued (to STDERR), if
      // that's not the case, it's a real pain to debug. So trace it.
      //
      l4 ([&]{trace << "process " << args[0] << " " << e;});

      throw failed ();
    }

    // Even if the user asked to suppress diagnostiscs, one error that we
    // want to let through is the inability to execute the program itself.
    // We cannot reserve a special exit status to signal this so we will
    // just have to compare the output. This particular situation will
    // result in a single error line printed by run_start() above.
    //
    if (l.compare (0, 18, "unable to execute ") == 0)
      fail (loc) << l;

    return false;
  }
  catch (const process_error& e)
  {
    fail (loc) << "unable to execute " << args[0] << ": " << e << endf;
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
  hash_options (sha256& csum, const lookup& l)
  {
    if (l)
      hash_options (csum, cast<strings> (l));
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
  hash_options (sha256& csum, const strings& sv, size_t n)
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
      if (ic ? casecmp (s, o) == 0 : s == o)
        return true;

    return false;
  }

  bool
  find_option (const char* o, const cstrings& cstrs, bool ic)
  {
    for (const char* s: cstrs)
      if (s != nullptr && (ic ? casecmp (s, o) : strcmp (s, o)) == 0)
        return true;

    return false;
  }

  bool
  find_options (initializer_list<const char*> os, const lookup& l, bool ic)
  {
    return l && find_options (os, cast<strings> (l), ic);
  }

  bool
  find_options (initializer_list<const char*> os, const strings& strs, bool ic)
  {
    for (const string& s: strs)
      for (const char* o: os)
        if (ic ? casecmp (s, o) == 0 : s == o)
          return true;

    return false;
  }

  bool
  find_options (initializer_list<const char*> os,
                const cstrings& cstrs,
                bool ic)
  {
    for (const char* s: cstrs)
      if (s != nullptr)
        for (const char* o: os)
          if ((ic ? casecmp (s, o) : strcmp (s, o)) == 0)
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
      if ((ic ? casecmp (s, p, n) : s.compare (0, n, p)) == 0)
        return &s;

    return nullptr;
  }

  const char*
  find_option_prefix (const char* p, const cstrings& cstrs, bool ic)
  {
    size_t n (strlen (p));

    for (const char* s: reverse_iterate (cstrs))
      if (s != nullptr && (ic ? casecmp (s, p, n) : strncmp (s, p, n)) == 0)
        return s;

    return nullptr;
  }

  const string*
  find_option_prefixes (initializer_list<const char*> ps,
                        const lookup& l,
                        bool ic)
  {
    return l ? find_option_prefixes (ps, cast<strings> (l), ic) : nullptr;
  }

  const string*
  find_option_prefixes (initializer_list<const char*> ps,
                        const strings& strs,
                        bool ic)
  {
    for (const string& s: reverse_iterate (strs))
      for (const char* p: ps)
        if ((ic
             ? casecmp (s, p, strlen (p))
             : s.compare (0, strlen (p), p)) == 0)
          return &s;

    return nullptr;
  }

  const char*
  find_option_prefixes (initializer_list<const char*> ps,
                        const cstrings& cstrs,
                        bool ic)
  {
    for (const char* s: reverse_iterate (cstrs))
      if (s != nullptr)
        for (const char* p: ps)
          if ((ic
               ? casecmp (s, p, strlen (p))
               : strncmp (s, p, strlen (p))) == 0)
            return s;

    return nullptr;
  }

  string
  apply_pattern (const char* s, const string* p)
  {
    if (p == nullptr || p->empty ())
      return s;

    size_t i (p->find ('*'));
    assert (i != string::npos);

    string r (*p, 0, i++);
    r.append (s);
    r.append (*p, i, p->size () - i);
    return r;
  }

  void
  init (void (*t) (bool),
        const char* a0,
        bool kg, bool dr, optional<bool> mc,
        optional<path> cs, optional<path> cg)
  {
    terminate = t;

    argv0 = process::path_search (a0, true);

    keep_going = kg;
    dry_run_option = dr;
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
  }
}
