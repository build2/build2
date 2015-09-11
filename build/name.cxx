// file      : build/name.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
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
    if (n.proj != nullptr)
      os << *n.proj << '%';

    // If the value is empty, then we want to print the directory
    // inside {}, e.g., dir{bar/}, not bar/dir{}. We also want to
    // print {} for an empty name.
    //
    bool d (!n.dir.empty ());
    bool v (!n.value.empty ());
    bool t (!n.type.empty () || (!d && !v));

    if (v)
      os << n.dir;

    if (t)
      os << n.type << '{';

    if (v)
      os << n.value;
    else
      os << n.dir;

    if (t)
      os << '}';

    return os;
  }

  ostream&
  operator<< (ostream& os, const names& ns)
  {
    for (auto i (ns.begin ()), e (ns.end ()); i != e; )
    {
      const name& n (*i);
      ++i;
      os << n;

      if (n.pair != '\0')
        os << n.pair;
      else if (i != e)
        os << ' ';
    }

    return os;
  }
}
