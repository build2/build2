// file      : build/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/utility>

#include <cstdlib> // strtol()

using namespace std;

namespace build
{
  const string empty_string;
  const path empty_path;
  const dir_path empty_dir_path;

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

    unsigned int ma, mi, bf, ab;

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

      if (k != 'a' && k != 'b')
        bail ("'a' or 'b' expected in release component");

      ab = parse (++p, "invalid release component", 1, 49);

      if (p != n)
        bail ("junk after release component");

      if (k == 'b')
        ab += 50;
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
