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
    bool hd (false);

    // Print the directory before type.
    //
    if (!n.dir.empty ())
    {
      string s (diag_relative_work (n.dir));

      // If both type and value are empty, there will be nothing printed.
      //
      if (s != "." || (!ht && !hv))
      {
        os << s;

        // Add the directory separator unless it is already there.
        //
        if (s.back () != path::traits::directory_separator)
          os << path::traits::directory_separator;

        hd = true;
      }
    }

    if (ht)
      os << n.type;

    if (ht || (hd && hv))
      os << '{';

    os << n.value;

    if (ht || (hd && hv))
      os << '}';

    if (!ht && !hv && !hd)
      os << "{}"; // Nothing got printed.

    return os;
  }

  ostream&
  operator<< (ostream& os, const names& ns)
  {
    for (auto i (ns.begin ()), e (ns.end ()); i != e; )
    {
      const name& n (*i);
      ++i;
      os << n << (n.pair ? "=" : (i != e ? " " : ""));
    }

    return os;
  }
}
