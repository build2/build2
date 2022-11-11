// file      : tests/test/simple/generated/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef _WIN32
#  include <chrono>
#  include <thread>
#else
#  include <libbutl/win32-utility.hxx>
#endif

#include <string>
#include <fstream>
#include <iostream>

#undef NDEBUG
#include <cassert>

using namespace std;

// If the -s option is specified, then also sleep for 5 seconds.
//
int
main (int argc, char* argv[])
{
  int i (1);
  for (; i != argc; ++i)
  {
    string a (argv[i]);

    if (a == "-s")
    {
      // MINGW GCC 4.9 doesn't implement this_thread so use Win32 Sleep().
      //
#ifndef _WIN32
      this_thread::sleep_for (chrono::seconds (5));
#else
      Sleep (5000);
#endif
    }
    else
      break;
  }

  int r (0);

  if (i == argc)
  {
    cout << "1.2.3" << endl;
  }
  else
  {
    istream* is;
    ifstream ifs;

    if (argv[i] != string ("-"))
    {
      ifs.open (argv[i]);

      if (!ifs.is_open ())
        cerr << "unable to open " << argv[1] << endl;

      is = &ifs;
    }
    else
      is = &cin;

    string s;
    r = getline (*is, s) && s == "1.2.3" ? 0 : 1;
  }

  return r;
}
