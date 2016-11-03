// file      : tests/test/script/runner/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <limits>    // numeric_limits
#include <string>
#include <cassert>
#include <ostream>   // endl, *bit
#include <istream>   // istream::traits_type::eof()
#include <iostream>
#include <exception>

#include <butl/path>
#include <butl/fdstream>
#include <butl/filesystem>

using namespace std;
using namespace butl;

int
main (int argc, char* argv[])
{
  // Usage: driver [-i <int>] [-s <int>] (-o <string>)* (-e <string>)*
  //        (-f <file>)* (-d <dir>)*
  //
  int status (256);
  int ifd (3);

  cout.exceptions (ostream::failbit | ostream::badbit);
  cerr.exceptions (ostream::failbit | ostream::badbit);

  for (int i (1); i < argc; ++i)
  {
    string o (argv[i++]);
    assert (i < argc);

    string v (argv[i]);

    auto toi = [] (const string& s) -> int
    {
      int r (-1);

      try
      {
        size_t n;
        r = stoi (s, &n);
        assert (n == s.size ());
      }
      catch (const exception&)
      {
        assert (false);
      }

      return r;
    };

    if (o == "-i")
    {
      assert (ifd == 3); // Make sure is not set yet.

      ifd = toi (v);
      assert (ifd >= 0 && ifd < 3);

      if (ifd == 0)
        cin.ignore (numeric_limits<streamsize>::max ());
      else if (cin.peek () != istream::traits_type::eof ())
        (ifd == 1 ? cout : cerr) << cin.rdbuf ();
    }
    else if (o == "-o")
    {
      cout << v << endl;
    }
    else if (o == "-e")
    {
      cerr << v << endl;
    }
    else if (o == "-s")
    {
      assert (status == 256); // Make sure is not set yet.

      status = toi (v);
      assert (status >= 0 && status < 256);
    }
    else if (o == "-f")
    {
      ofdstream os (v);
      os.close ();
    }
    else if (o == "-d")
    {
      try_mkdir_p (dir_path (v));
    }
    else
      assert (false);
  }

  return status == 256 ? 0 : status;
}
