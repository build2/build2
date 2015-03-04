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
    bool ht (!n.type.empty ());
    bool hv (!n.value.empty ());

    if (ht)
      os << n.type << '{';

    if (!n.dir.empty ())
    {
      string s (diag_relative_work (n.dir));

      // If both type and value are empty, there will be nothing printed.
      //
      if (s != "." || (!ht && !hv))
      {
        os << s;

        // Add the directory separator unless it is already there
        // or we have type but no value. The idea is to print foo/
        // or dir{foo}.
        //
        if (s.back () != path::traits::directory_separator && (hv || !ht))
          os << path::traits::directory_separator;
      }
    }

    os << n.value;

    if (ht)
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
