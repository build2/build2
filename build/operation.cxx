// file      : build/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/operation>

#include <ostream>

using namespace std;

namespace build
{
  ostream&
  operator<< (ostream& os, action a)
  {
    return os << '('
              << static_cast<uint16_t> (a.meta_operation ()) << ','
              << static_cast<uint16_t> (a.operation ())
              << ')';
  }

  meta_operation_info perform {"perform"};

  operation_info default_ {"<default>", execution_mode::first};
  operation_info update {"update", execution_mode::first};
  operation_info clean {"clean", execution_mode::last};
}
