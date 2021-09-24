// file      : tests/test/config-test/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <iostream>

#undef NDEBUG
#include <cassert>

using namespace std;

int
main (int argc, char* argv[])
{
  if (argc != 2)
  {
    cerr << "usage: " << argv[0] << " <arg>" << endl;
    return 1;
  }

  cout << argv[1] << endl;
}
