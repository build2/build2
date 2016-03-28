// file      : build2/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/utility>

#include <cstdlib>  // strtol()
#include <iostream> // cerr

#include <build2/context>
#include <build2/variable>
#include <build2/diagnostics>

using namespace std;

namespace build2
{
  //
  // <build2/types>
  //

  ostream&
  operator<< (ostream& os, const path& p)
  {
    return os << (stream_verb (os) < 2 ? diag_relative (p) : p.string ());
  }

  ostream&
  operator<< (ostream& os, const dir_path& d)
  {
    if (stream_verb (os) < 2)
      os << diag_relative (d); // Adds trailing '/'.
    else
    {
      const string& s (d.string ());

      // Print the directory with trailing '/'.
      //
      if (!s.empty ())
        os << s << (dir_path::traits::is_separator (s.back ()) ? "" : "/");
    }

    return os;
  }

  //
  // <build2/utility>
  //

  string&
  trim (string& l)
  {
    /*
    assert (trim (r = "") == "");
    assert (trim (r = " ") == "");
    assert (trim (r = " \t\r") == "");
    assert (trim (r = "a") == "a");
    assert (trim (r = " a") == "a");
    assert (trim (r = "a ") == "a");
    assert (trim (r = " \ta") == "a");
    assert (trim (r = "a \r") == "a");
    assert (trim (r = " a ") == "a");
    assert (trim (r = " \ta \r") == "a");
    */

    size_t i (0), n (l.size ());

    for (char c;
         i != n && ((c = l[i]) == ' ' || c == '\t' || c == '\r');
         ++i) ;

    for (char c;
         n != i && ((c = l[n - 1]) == ' ' || c == '\t' || c == '\r');
         --n) ;

    if (i != 0)
    {
      string s (l, i, n - i);
      l.swap (s);
    }
    else if (n != l.size ())
      l.resize (n);

    return l;
  }

  process
  start_run (const char* const* args, bool err)
  {
    if (verb >= 3)
      print_process (args);

    try
    {
      return process (args, 0, -1, (err ? 2 : 1));
    }
    catch (const process_error& e)
    {
      if (e.child ())
      {
        // Note: finish_run() expects this exact message.
        //
        cerr << "unable to execute " << args[0] << ": " << e.what () << endl;
        exit (1);
      }
      else
        error << "unable to execute " << args[0] << ": " << e.what ();

      throw failed ();
    }
  };

  bool
  finish_run (const char* const* args, bool err, process& pr, const string& l)
  try
  {
    if (pr.wait ())
      return true;

    if (err)
      // Assuming diagnostics has already been issued (to STDERR).
      //
      throw failed ();

    // Even if the user asked to suppress diagnostiscs, one error that we
    // want to let through is the inability to execute the program itself.
    // We cannot reserve a special exit status to signal this so we will
    // just have to compare the output. This particular situation will
    // result in a single error line printed by run_start() above.
    //
    if (l.compare (0, 18, "unable to execute ") == 0)
      fail << l;

    return false;
  }
  catch (const process_error& e)
  {
    error << "unable to execute " << args[0] << ": " << e.what ();
    throw failed ();
  }

  const string empty_string;
  const path empty_path;
  const dir_path empty_dir_path;

  void
  append_options (cstrings& args, const lookup<const value>& l)
  {
    if (l)
      append_options (args, cast<strings> (*l));
  }

  void
  hash_options (sha256& csum, const lookup<const value>& l)
  {
    if (l)
      hash_options (csum, cast<strings> (*l));
  }

  void
  append_options (cstrings& args, const strings& sv)
  {
    if (!sv.empty ())
    {
      args.reserve (args.size () + sv.size ());

      for (const string& s: sv)
        args.push_back (s.c_str ());
    }
  }

  void
  hash_options (sha256& csum, const strings& sv)
  {
    for (const string& s: sv)
      csum.append (s);
  }

  bool
  find_option (const char* option, const lookup<const value>& l)
  {
    if (l)
    {
      for (const string& s: cast<strings> (*l))
      {
        if (s == option)
          return true;
      }
    }

    return false;
  }

  unsigned int
  to_version (const string& s)
  {
    // See tests/version.
    //

    auto parse = [&s] (size_t& p, const char* m, long min = 0, long max = 99)
      -> unsigned int
    {
      if (s[p] == '-' || s[p] == '+') // stoi() allows these.
        throw invalid_argument (m);

      const char* b (s.c_str () + p);
      char* e;
      long r (strtol (b, &e, 10));

      if (b == e || r < min || r > max)
        throw invalid_argument (m);

      p = e - s.c_str ();
      return static_cast<unsigned int> (r);
    };

    auto bail = [] (const char* m) {throw invalid_argument (m);};

    unsigned int ma, mi, bf, ab (0);

    size_t p (0), n (s.size ());
    ma = parse (p, "invalid major version");

    if (p >= n || s[p] != '.')
      bail ("'.' expected after major version");

    mi = parse (++p, "invalid minor version");

    if (p >= n || s[p] != '.')
      bail ("'.' expected after minor version");

    bf = parse (++p, "invalid bugfix version");

    if (p < n)
    {
      if (s[p] != '-')
        bail ("'-' expected after bugfix version");

      char k (s[++p]);

      if (k != '\0')
      {
        if (k != 'a' && k != 'b')
          bail ("'a' or 'b' expected in release component");

        ab = parse (++p, "invalid release component", 1, 49);

        if (p != n)
          bail ("junk after release component");

        if (k == 'b')
          ab += 50;
      }
      else
        ab = 1;
    }

    //                  AABBCCDD
    unsigned int r (ma * 1000000U +
                    mi *   10000U +
                    bf *     100U);

    if (ab != 0)
    {
      if (r == 0)
        bail ("0.0.0 version with release component");

      r -= 100;
      r += ab;
    }

    return r;
  }

  bool exception_unwinding_dtor = false;
}
