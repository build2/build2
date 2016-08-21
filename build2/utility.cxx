// file      : build2/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/utility>

#include <cstring>  // strlen(), str[n]cmp()
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
    return os << (stream_verb (os) < 2
                  ? diag_relative (p)
                  : p.representation ());
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
  start_run (const char* args[], bool err)
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
  finish_run (const char* args[], bool err, process& pr, const string& l)
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
    // result in a single error line printed by start_run() above.
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
  append_options (cstrings& args, const lookup& l)
  {
    if (l)
      append_options (args, cast<strings> (l));
  }

  void
  hash_options (sha256& csum, const lookup& l)
  {
    if (l)
      hash_options (csum, cast<strings> (l));
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

  bool
  find_option_prefix (const char* p, const lookup& l, bool ic)
  {
    return l && find_option_prefix (p, cast<strings> (l), ic);
  }

  bool
  find_option_prefix (const char* p, const strings& strs, bool ic)
  {
    size_t n (strlen (p));

    for (const string& s: strs)
      if ((ic ? casecmp (s, p, n) : s.compare (0, n, p)) == 0)
        return true;

    return false;
  }

  bool
  find_option_prefix (const char* p, const cstrings& cstrs, bool ic)
  {
    size_t n (strlen (p));

    for (const char* s: cstrs)
      if (s != nullptr && (ic ? casecmp (s, p, n) : strncmp (s, p, n)) == 0)
        return true;

    return false;
  }

  bool
  find_option_prefixes (initializer_list<const char*> ps,
                        const lookup& l,
                        bool ic)
  {
    return l && find_option_prefixes (ps, cast<strings> (l), ic);
  }

  bool
  find_option_prefixes (initializer_list<const char*> ps,
                        const strings& strs,
                        bool ic)
  {
    for (const string& s: strs)
      for (const char* p: ps)
        if ((ic
             ? casecmp (s, p, strlen (p))
             : s.compare (0, strlen (p), p)) == 0)
          return true;

    return false;
  }

  bool
  find_option_prefixes (initializer_list<const char*> ps,
                        const cstrings& cstrs,
                        bool ic)
  {
    for (const char* s: cstrs)
      if (s != nullptr)
        for (const char* p: ps)
          if ((ic
               ? casecmp (s, p, strlen (p))
               : strncmp (s, p, strlen (p))) == 0)
            return true;

    return false;
  }

  string
  apply_pattern (const char* s, const string* p)
  {
    if (p == nullptr)
      return s;

    size_t i (p->find ('*'));
    assert (i != string::npos);

    string r (*p, 0, i++);
    r.append (s);
    r.append (*p, i, p->size () - i);
    return r;
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
