// file      : build/prerequisite.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/prerequisite>

#include <ostream>

#include <build/target> // target_type

using namespace std;

namespace build
{
  ostream&
  operator<< (ostream& os, const prerequisite& p)
  {
    // @@ TODO: need to come up with a relative (to current) path.

    return os << p.type.name << '{' << p.name << '}';
  }

  bool
  operator< (const prerequisite& x, const prerequisite& y)
  {
    return
      (x.type.id < y.type.id) ||
      (x.type.id == y.type.id && x.name < y.name) ||
      (x.type.id == y.type.id && x.name == y.name &&
       x.directory < y.directory);
  }
}
