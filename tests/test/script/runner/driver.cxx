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

using namespace std;

int
main (int argc, char* argv[])
{
  // Usage: driver [-i <int>] [-s <int>] (-o <string>)* (-e <string>)*
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
      try
      {
        return stoi (s);
      }
      catch (const exception&)
      {
        assert (false);
      }
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
    else
      assert (false);
  }

  return status == 256 ? 0 : status;
}
