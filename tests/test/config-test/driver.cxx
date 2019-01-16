// file      : tests/test/config-test/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <iostream>

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
