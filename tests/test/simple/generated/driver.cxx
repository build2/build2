// file      : tests/test/simple/generated/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <string>
#include <fstream>
#include <iostream>

using namespace std;

int
main (int argc, char* argv[])
{
  int r (0);

  if (argc == 1)
  {
    cout << "1.2.3" << endl;
  }
  else
  {
    ifstream ifs (argv[1]);

    if (!ifs.is_open ())
      cerr << "unable to open " << argv[1] << endl;

    string s;
    r = getline (ifs, s) && s == "1.2.3" ? 0 : 1;
  }

  return r;
}
