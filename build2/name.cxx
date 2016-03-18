// file      : build2/name.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/name>

#include <build2/diagnostics>

using namespace std;

namespace build2
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

      if (n.pair)
        os << '@';
      else if (i != e)
        os << ' ';
    }

    return os;
  }
}
