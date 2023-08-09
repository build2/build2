// file      : libbuild2/functions-process.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbutl/regex.hxx>
#include <libbutl/builtin.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // Ideas for potential further improvements:
  //
  // - Use scope to query environment.
  // - Mode to ignore error/suppress diagnostics and return NULL?
  // - Similar regex flags to regex.* functions (icase, etc)?

  // Convert the program (builtin or process) arguments from names to strings.
  // The function name is only used for diagnostics.
  //
  static inline strings
  program_args (names&& args, const char* fn)
  {
    try
    {
      return convert<strings> (move (args));
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid process." << fn << "() argument: " << e << endf;
    }
  }

  // Read text from a stream, trim it and return as a value. Throw io_error on
  // the stream reading error.
  //
  static value
  read (auto_fd&& fd)
  {
    string v;
    ifdstream is (move (fd));

    // Note that getline() will fail if there is no data.
    //
    if (is.peek () != ifdstream::traits_type::eof ())
      getline (is, v, '\0');

    is.close (); // Detect errors.

    names r;
    r.push_back (to_name (move (trim (v))));
    return value (move (r));
  }

  regex
  parse_regex (const string&, regex::flag_type); // functions-regex.cxx

  // Read lines from a stream, match them against a regular expression, and
  // return the list of matched lines or their replacements, if the format is
  // specified. Throw invalid_argument on the regex parsing error and io_error
  // on the stream reading error.
  //
  static value
  read_regex (auto_fd&& fd, const string& pat, const optional<string>& fmt)
  {
    names r;
    ifdstream is (move (fd), fdstream_mode::skip, ifdstream::badbit);

    // Note that the stream is read out (and is silently closed) if
    // invalid_argument is thrown, which is probably ok since this is not a
    // common case.
    //
    regex re (parse_regex (pat, regex::ECMAScript));

    for (string l; !eof (getline (is, l)); )
    {
      if (fmt)
      {
        pair<string, bool> p (regex_replace_match (l, re, *fmt));

        if (p.second)
          r.push_back (to_name (move (p.first)));
      }
      else if (regex_match (l, re))
        r.push_back (to_name (move (l)));
    }

    is.close (); // Detect errors.

    return value (move (r));
  }

  // Return the builtin function pointer if this is a call to an internal
  // builtin and NULL otherwise.
  //
  static builtin_function*
  builtin (const names& args)
  {
    if (args.empty ())
      return nullptr;

    const name& nm (args[0]);
    if (!nm.simple () || nm.pair)
      return nullptr;

    const builtin_info* r (builtins.find (nm.value));
    return r != nullptr ? r->function : nullptr;
  }

  // Return the builtin name and its arguments. The builtin function is only
  // used to make sure that args have been checked with the builtin()
  // predicate.
  //
  static pair<string, strings>
  builtin_args (builtin_function*, names&& args, const char* fn)
  {
    string bn (move (args[0].value));
    args.erase (args.begin (), args.begin () + 1);
    return pair<string, strings> (move (bn), program_args (move (args), fn));
  }

  // Read data from a stream, optionally processing it and returning the
  // result as a value.
  //
  using read_function = function<value (auto_fd&&)>;

  // Run a builtin. The builtin name is only used for diagnostics.
  //
  static value
  run_builtin_impl (builtin_function* bf,
                    const strings& args,
                    const string& bn,
                    const read_function& read)
  {
    try
    {
      dir_path cwd;
      builtin_callbacks cb;
      fdpipe ofd (open_pipe ());

      if (verb >= 3)
        print_process (process_args (bn, args));

      uint8_t rs; // Storage.
      butl::builtin b (bf (rs,
                           args,
                           nullfd         /* stdin */,
                           move (ofd.out) /* stdout */,
                           nullfd         /* stderr */,
                           cwd,
                           cb));

      try
      {
        value r (read (move (ofd.in)));

        if (b.wait () == 0)
          return r;

        // Fall through.
        //
      }
      catch (const io_error& e)
      {
        // If the builtin has failed then assume the io error was caused by
        // that and so fall through.
        //
        if (b.wait () == 0)
          fail << "io error reading " << bn << " builtin output: " << e;
      }

      // While assuming that the builtin has issued the diagnostics on failure
      // we still print the error message (see process_finish() for details).
      //
      diag_record dr;
      dr << fail << "builtin " << bn << " " << process_exit (rs);

      if (verb >= 1 && verb <= 2)
      {
        dr << info << "command line: ";
        print_process (dr, process_args (bn, args));
      }

      dr << endf;
    }
    catch (const system_error& e)
    {
      fail << "unable to execute " << bn << " builtin: " << e << endf;
    }
  }

  static inline value
  run_builtin (const scope* s,
               builtin_function* bf,
               const strings& args,
               const string& bn)
  {
    // See below.
    //
    if (s != nullptr && s->ctx.phase != run_phase::load)
      fail << "process.run() called during " << s->ctx.phase << " phase";

    return run_builtin_impl (bf, args, bn, read);
  }

  static inline value
  run_builtin_regex (const scope* s,
                     builtin_function* bf,
                     const strings& args,
                     const string& bn,
                     const string& pat,
                     const optional<string>& fmt)
  {
    // See below.
    //
    if (s != nullptr && s->ctx.phase != run_phase::load)
      fail << "process.run_regex() called during " << s->ctx.phase << " phase";

    // Note that we rely on the "small function object" optimization here.
    //
    return run_builtin_impl (bf, args, bn,
                             [&pat, &fmt] (auto_fd&& fd)
                             {
                               return read_regex (move (fd), pat, fmt);
                             });
  }

  // Return the process path and its arguments.
  //
  static pair<process_path, strings>
  process_args (names&& args, const char* fn)
  {
    if (args.empty () || args[0].empty ())
      fail << "executable name expected in process." << fn << "()";

    optional<process_path> pp;

    try
    {
      size_t erase (0);

      // This can be a process_path (pair), process_path_ex (process_path
      // optionally followed by the name@, checksum@, and env-checksum@
      // pairs), or just a path.
      //
      // First, check if the arguments begin with a process_path[_ex] and, if
      // that's the case, only use the leading name/pair to create the process
      // path, discarding the metadata.
      //
      if (args[0].file ())
      {
        // Find the end of the process_path[_ex] value.
        //
        auto b (args.begin ());
        auto i (value_traits<process_path_ex>::find_end (args));

        if (b->pair || i != b + 1) // First is a pair or pairs after.
        {
          pp = convert<process_path> (
            names (make_move_iterator (b),
                   make_move_iterator (b + (b->pair ? 2 : 1))));

          erase = i - b;
        }
      }

      // Fallback to a path, if this is not a process path.
      //
      if (!pp)
      {
        // Strip the builtin-escaping '^' character, if present.
        //
        path p (convert<path> (move (args[0])));

        if (p.simple ())
        try
        {
          const string& s (p.string ());

          // Don't end up with an empty path.
          //
          if (s.size () > 1 && s[0] == '^')
            p = path (s, 1, s.size () - 1);
        }
        catch (const invalid_path& e)
        {
          throw invalid_argument (e.path);
        }

        pp = run_search (p);
        erase = 1;
      }

      args.erase (args.begin (), args.begin () + erase);
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid process." << fn << "() executable path: " << e;
    }

    return pair<process_path, strings> (move (*pp),
                                        program_args (move (args), fn));
  }

  static process
  process_start (const scope*,
                 const process_path& pp,
                 const strings& args,
                 cstrings& cargs)
  {
    cargs.reserve (args.size () + 2);
    cargs.push_back (pp.recall_string ());
    transform (args.begin (),
               args.end (),
               back_inserter (cargs),
               [] (const string& s) {return s.c_str ();});
    cargs.push_back (nullptr);

    // Note that for now these functions can only be called during the load
    // phase (see below) and so no diagnostics buffering is needed.
    //
    return run_start (3               /* verbosity */,
                      pp,
                      cargs,
                      0               /* stdin  */,
                      -1              /* stdout */);
  }

  // Always issue diagnostics on process failure, regardless if the process
  // exited abnormally or normally with non-zero exit code.
  //
  // Note that the diagnostics stack is only printed if a diagnostics record
  // is created, which is not always the case for run_finish().
  //
  void
  process_finish (const scope*, const cstrings& args, process& pr)
  {
    run_finish (args, pr, 2 /* verbosity */);
  }

  // Run a process.
  //
  static value
  run_process_impl (const scope* s,
                    const process_path& pp,
                    const strings& args,
                    const read_function& read)
  {
    cstrings cargs;
    process pr (process_start (s, pp, args, cargs));

    value r;
    try
    {
      r = read (move (pr.in_ofd));
    }
    catch (const io_error& e)
    {
      if (run_wait (cargs, pr))
        fail << "io error reading " << cargs[0] << " output: " << e;

      // If the child process has failed then assume the io error was
      // caused by that and let process_finish() deal with it.
    }

    process_finish (s, cargs, pr);
    return r;
  }

  static inline value
  run_process (const scope* s, const process_path& pp, const strings& args)
  {
    // The only plausible place where these functions can be called outside
    // the load phase are scripts and there it doesn't make much sense to use
    // them (the same can be achieved with commands in a uniform manner). Note
    // that if there is no scope, then this is most likely (certainly?) the
    // load phase (for example, command line).
    //
    if (s != nullptr && s->ctx.phase != run_phase::load)
      fail << "process.run() called during " << s->ctx.phase << " phase";

    return run_process_impl (s, pp, args, read);
  }

  static inline value
  run_process_regex (const scope* s,
                     const process_path& pp,
                     const strings& args,
                     const string& pat,
                     const optional<string>& fmt)
  {
    // See above.
    //
    if (s != nullptr && s->ctx.phase != run_phase::load)
      fail << "process.run_regex() called during " << s->ctx.phase << " phase";

    // Note that we rely on the "small function object" optimization here.
    //
    return run_process_impl (s, pp, args,
                             [&pat, &fmt] (auto_fd&& fd)
                             {
                               return read_regex (move (fd), pat, fmt);
                             });
  }

  static inline value
  run (const scope* s, names&& args)
  {
    if (builtin_function* bf = builtin (args))
    {
      pair<string, strings> ba (builtin_args (bf, move (args), "run"));
      return run_builtin (s, bf, ba.second, ba.first);
    }
    else
    {
      pair<process_path, strings> pa (process_args (move (args), "run"));
      return run_process (s, pa.first, pa.second);
    }
  }

  static inline value
  run_regex (const scope* s,
             names&& args,
             const string& pat,
             const optional<string>& fmt)
  {
    if (builtin_function* bf = builtin (args))
    {
      pair<string, strings> ba (builtin_args (bf, move (args), "run_regex"));
      return run_builtin_regex (s, bf, ba.second, ba.first, pat, fmt);
    }
    else
    {
      pair<process_path, strings> pa (process_args (move (args),
                                                    "run_regex"));

      return run_process_regex (s, pa.first, pa.second, pat, fmt);
    }
  }

  void
  process_functions (function_map& m)
  {
    function_family f (m, "process");

    // $process.run(<prog>[ <args>...])
    //
    // Run builtin or external program and return trimmed `stdout` output.
    //
    // Note that if the result of executing the program can be affected by
    // environment variables and this result can in turn affect the build
    // result, then such variables should be reported with the
    // `config.environment` directive.
    //
    // Note that this function is not pure and can only be called during the
    // load phase.
    //
    f.insert (".run", false) += [](const scope* s, names args)
    {
      return run (s, move (args));
    };

    f.insert ("run", false) += [](const scope* s, process_path pp)
    {
      return run_process (s, pp, strings ());
    };

    // $process.run_regex(<prog>[ <args>...], <pat>[, <fmt>])
    //
    // Run builtin or external program and return `stdout` output lines
    // matched and optionally processed with a regular expression.
    //
    // Each line of stdout (including the customary trailing blank) is matched
    // (as a whole) against <pat> and, if successful, returned, optionally
    // processed with <fmt>, as an element of a list. See the `$regex.*()`
    // function family for details on regular expressions and format strings.
    //
    // Note that if the result of executing the program can be affected by
    // environment variables and this result can in turn affect the build
    // result, then such variables should be reported with the
    // `config.environment` directive.
    //
    // Note that this function is not pure and can only be called during the
    // load phase.
    //
    {
      auto e (f.insert (".run_regex", false));

      e += [](const scope* s, names a, string p, optional<string> f)
      {
        return run_regex (s, move (a), p, f);
      };

      e += [] (const scope* s, names a, names p, optional<names> f)
      {
        return run_regex (s,
                          move (a),
                          convert<string> (move (p)),
                          f ? convert<string> (move (*f)) : nullopt_string);
      };
    }
    {
      auto e (f.insert ("run_regex", false));

      e += [](const scope* s, process_path pp, string p, optional<string> f)
      {
        return run_process_regex (s, pp, strings (), p, f);
      };

      e += [](const scope* s, process_path pp, names p, optional<names> f)
      {
        return run_process_regex (s,
                                  pp, strings (),
                                  convert<string> (move (p)),
                                  (f
                                   ? convert<string> (move (*f))
                                   : nullopt_string));
      };
    }
  }
}
