// file      : tests/test/simple/generated/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <iostream>
#include <fstream>

using namespace std;

int
main (int argc, char* argv[])
{
  if (argc != 2)
  {
    cerr << "usage: " << argv[0] << " <file>" << endl;
    return 1;
  }

  ifstream ifs (argv[1], ifstream::in | ifstream::binary | ifstream::ate);

  if (!ifs.is_open ())
    cerr << "unable to open " << argv[1] << endl;

  if (ifs.tellg () == 0)
    cerr << argv[1] << " is empty" << endl;

  return 0;
}
