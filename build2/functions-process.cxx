// file      : build2/functions-process.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbutl/regex.mxx>

#include <build2/function.hxx>
#include <build2/variable.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // Ideas for potential further improvements:
  //
  // - Use scope to query environment.
  // - Mode to ignore error/suppress diagnostics and return NULL?
  // - Similar regex flags to regex.* functions (icase, etc)?

  // Process arguments.
  //
  static pair<process_path, strings>
  process_args (names&& args, const char* fn)
  {
    if (args.empty () || args[0].empty ())
      fail << "executable name expected in process." << fn << "()";

    process_path pp;
    try
    {
      size_t erase;

      // This can be a process_path (pair) or just a path.
      //
      if (args[0].pair)
      {
        pp = convert<process_path> (move (args[0]), move (args[1]));
        erase = 2;
      }
      else
      {
        pp = run_search (convert<path> (move (args[0])));
        erase = 1;
      }

      args.erase (args.begin (), args.begin () + erase);
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid process." << fn << "() executable path: " << e.what ();
    }

    strings sargs;
    try
    {
      sargs = convert<strings> (move (args));
    }
    catch (const invalid_argument& e)
    {
      fail << "invalid process." << fn << "() argument: " << e.what ();
    }

    return pair<process_path, strings> (move (pp), move (sargs));
  }

  static process
  start (const scope*,
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

    return run_start (3               /* verbosity */,
                      pp,
                      cargs.data (),
                      0               /* stdin  */,
                      -1              /* stdout */);
  }

  static void
  finish (cstrings& args, process& pr, bool io)
  {
    run_finish (args, pr);

    if (io)
      fail << "error reading " << args[0] << " output";
  }

  static value
  run (const scope* s, const process_path& pp, const strings& args)
  {
    cstrings cargs;
    process pr (start (s, pp, args, cargs));

    string v;
    bool io (false);
    try
    {
      ifdstream is (move (pr.in_ofd));

      // Note that getline() will fail if there is no output.
      //
      if (is.peek () != ifdstream::traits_type::eof ())
        getline (is, v, '\0');

      is.close (); // Detect errors.
    }
    catch (const io_error&)
    {
      // Presumably the child process failed and issued diagnostics so let
      // finish() try to deal with that first.
      //
      io = true;
    }

    finish (cargs, pr, io);

    names r;
    r.push_back (to_name (move (trim (v))));
    return value (move (r));
  }

  regex
  parse_regex (const string&, regex::flag_type); // functions-regex.cxx

  static value
  run_regex (const scope* s,
             const process_path& pp,
             const strings& args,
             const string& pat,
             const optional<string>& fmt)
  {
    regex re (parse_regex (pat, regex::ECMAScript));

    cstrings cargs;
    process pr (start (s, pp, args, cargs));

    names r;
    bool io (false);
    try
    {
      ifdstream is (move (pr.in_ofd), ifdstream::badbit);

      for (string l; !eof (getline (is, l)); )
      {
        if (fmt)
        {
          pair<string, bool> p (regex_replace_match (l, re, *fmt));

          if (p.second)
            r.push_back (to_name (move (p.first)));
        }
        else
        {
          if (regex_match (l, re))
            r.push_back (to_name (move (l)));
        }
      }

      is.close (); // Detect errors.
    }
    catch (const io_error&)
    {
      // Presumably the child process failed and issued diagnostics so let
      // finish() try to deal with that first.
      //
      io = true;
    }

    finish (cargs, pr, io);

    return value (move (r));
  }

  static inline value
  run_regex (const scope* s,
             names&& args,
             const string& pat,
             const optional<string>& fmt)
  {
    pair<process_path, strings> pa (process_args (move (args), "run_regex"));
    return run_regex (s, pa.first, pa.second, pat, fmt);
  }

  void
  process_functions ()
  {
    function_family f ("process");

    // $process.run(<prog>[ <args>...])
    //
    // Return trimmed stdout.
    //
    f[".run"] = [](const scope* s, names args)
    {
      pair<process_path, strings> pa (process_args (move (args), "run"));
      return run (s, pa.first, pa.second);
    };

    f["run"] = [](const scope* s, process_path pp)
    {
      return run (s, pp, strings ());
    };

    // $process.run_regex(<prog>[ <args>...], <pat> [, <fmt>])
    //
    // Return stdout lines matched and optionally processed with regex.
    //
    // Each line of stdout (including the customary trailing blank) is matched
    // (as a whole) against <pat> and, if successful, returned, optionally
    // processed with <fmt>, as an element of a list.
    //
    f[".run_regex"] = [](const scope* s, names a, string p, optional<string> f)
    {
      return run_regex (s, move (a), p, f);
    };

    f[".run_regex"] = [] (const scope* s, names a, names p, optional<names> f)
    {
      return run_regex (s,
                        move (a),
                        convert<string> (move (p)),
                        f ? convert<string> (move (*f)) : nullopt_string);
    };

    f["run_regex"] = [](const scope* s,
                        process_path pp,
                        string p,
                        optional<string> f)
    {
      return run_regex (s, pp, strings (), p, f);
    };

    f["run_regex"] = [](const scope* s,
                        process_path pp,
                        names p,
                        optional<names> f)
    {
      return run_regex (s,
                        pp, strings (),
                        convert<string> (move (p)),
                        f ? convert<string> (move (*f)) : nullopt_string);
    };
  }
}
