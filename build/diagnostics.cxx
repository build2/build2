// file      : build/diagnostics.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/diagnostics>

#include <iostream>

using namespace std;

namespace build
{
  void
  print_process (const char* const* args)
  {
    for (const char* const* p (args); *p != nullptr; p++)
      cerr << (p != args ? " " : "") << *p;
    cerr << endl;
  }
}
