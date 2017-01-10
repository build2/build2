// file      : tests/test/script/runner/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <limits>    // numeric_limits
#include <string>
#include <cstdlib>   // abort()
#include <cassert>
#include <ostream>   // endl, *bit
#include <istream>   // istream::traits_type::eof()
#include <iostream>
#include <exception>

#include <butl/path>
#include <butl/optional>
#include <butl/fdstream>
#include <butl/filesystem>

using namespace std;
using namespace butl;

// Call itself recursively causing stack overflow. Parameterized to avoid
// "stack overflow" warning.
//
static void
stack_overflow (bool overflow)
{
  if (overflow)
    stack_overflow (true);
}

int
main (int argc, char* argv[])
{
  // Usage: driver [-i <int>] (-o <string>)* (-e <string>)* (-f <file>)*
  //        (-d <dir>)* [(-t (a|m|s|z)) | (-s <int>)]
  //
  // Execute actions specified by -i, -o, -e, -f, -d options in the order as
  // they appear on the command line. After that terminate abnormally if -t
  // option is provided, otherwise exit normally with the status specified by
  // -s option (0 by default).
  //
  // -i <fd>
  //    Forward STDIN data to the standard stream denoted by the file
  //    descriptor. Read and discard if 0.
  //
  // -o <string>
  //    Print the line to STDOUT.
  //
  // -e <string>
  //    Print the line to STDERR.
  //
  // -f <path>
  //    Create an empty file with the path specified.
  //
  // -d <path>
  //    Create a directory with the path specified. Create parent directories
  //    if required.
  //
  // -t <method>
  //    Abnormally terminate itself using one of the following methods:
  //
  //    a - call abort()
  //    m - dereference null-pointer
  //    s - cause stack overflow using infinite function call recursion
  //    z - divide integer by zero
  //
  // -s <int>
  //    Exit normally with the status specified. The default status is 0.
  //
  int ifd (3);
  optional<int> status;
  char aterm ('\0');    // Abnormal termination method.

  cin.exceptions  (ostream::failbit | ostream::badbit);
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
      {
        ostream& o (ifd == 1 ? cout : cerr);
        o << cin.rdbuf ();
        o.flush ();
      }
    }
    else if (o == "-o")
    {
      cout << v << endl;
    }
    else if (o == "-e")
    {
      cerr << v << endl;
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
    else if (o == "-t")
    {
      assert (aterm == '\0' && !status); // Make sure exit method is not set.
      assert (v.size () == 1 && v.find_first_of ("amsz") != string::npos);
      aterm = v[0];
    }
    else if (o == "-s")
    {
      assert (!status && aterm == '\0'); // Make sure exit method is not set.
      status = toi (v);
    }
    else
      assert (false);
  }

  switch (aterm)
  {
  case 'a': abort (); break;
  case 'm':
    {
      int* p (nullptr);
      *p = 0;
      break;
    }
  case 's': stack_overflow (true); break;
  case 'z':
    {
      int z (0);
      z /= z;
      break;
    }
  }

  return status ? *status : 0;
}
