// file      : build/name.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/name>

#include <ostream>

#include <build/diagnostics>

using namespace std;

namespace build
{
  ostream&
  operator<< (ostream& os, const name& n)
  {
    if (!n.type.empty ())
      os << n.type << '{';

    if (!n.dir.empty ())
    {
      string s (diag_relative_work (n.dir));

      if (s != ".")
      {
        os << s;

        if (!n.value.empty () &&
            s.back () != path::traits::directory_separator)
          os << path::traits::directory_separator;
      }
      else if (n.value.empty () && n.type.empty ())
        os << s; // Otherwise nothing gets printed.
    }

    os << n.value;

    if (!n.type.empty ())
      os << '}';

    return os;
  }

  ostream&
  operator<< (ostream& os, const names& ns)
  {
    for (auto b (ns.begin ()), i (b), e (ns.end ()); i != e; ++i)
      os << (i != b ? " " : "") << *i;

    return os;
  }
}
