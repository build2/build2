// file      : build/prerequisite.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/prerequisite>

#include <ostream>

#include <build/scope>
#include <build/target> // target_type
#include <build/context>

using namespace std;

namespace build
{
  ostream&
  operator<< (ostream& os, const prerequisite& p)
  {
    if (p.target != nullptr)
      os << *p.target;
    else
    {
      os << p.type.name << '{';

      // Print scope unless the directory is absolute.
      //
      if (!p.directory.absolute ())
      {
        string s (diagnostic_string (p.scope.path ()));

        if (!s.empty ())
          os << s << path::traits::directory_separator << ": ";
      }

      // Print directory.
      //
      if (!p.directory.empty ())
      {
        string s (diagnostic_string (p.directory));

        if (!s.empty ())
          os << s << path::traits::directory_separator;
      }

      os << p.name;

      if (p.ext != nullptr)
        os << '.' << *p.ext;

      os << '}';
    }

    return os;
  }

  bool
  operator< (const prerequisite& x, const prerequisite& y)
  {
    //@@ TODO: use compare() to compare once.

    // Unspecified and specified extension are assumed equal. The
    // extension strings are from the pool, so we can just compare
    // pointers.
    //
    return
      (x.type.id < y.type.id) ||
      (x.type.id == y.type.id && x.name < y.name) ||
      (x.type.id == y.type.id && x.name == y.name &&
       x.directory < y.directory) ||
      (x.type.id == y.type.id && x.name == y.name &&
       x.directory == y.directory &&
       x.ext != nullptr && y.ext != nullptr && x.ext < y.ext);
  }
}
