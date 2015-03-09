// file      : build/spec.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/spec>

#include <ostream>

#include <build/diagnostics>

using namespace std;

namespace build
{
  ostream&
  operator<< (ostream& os, const targetspec& s)
  {
    if (!s.src_base.empty ())
    {
      string d (diag_relative_work (s.src_base));

      if (d != ".")
      {
        os << d;

        // Add the directory separator unless it is already there.
        //
        if (d.back () != path::traits::directory_separator)
          os << path::traits::directory_separator;

        os << '=';
      }
    }

    os << s.target;
    return os;
  }

  ostream&
  operator<< (ostream& os, const opspec& s)
  {
    bool ho (!s.operation.empty ());
    bool ht (!s.empty ());

    //os << s.operation;
    os << (ho ? "\"" : "") << s.operation << (ho ? "\"" : "");

    if (ho && ht)
      os << '(';

    for (auto b (s.begin ()), i (b); i != s.end (); ++i)
      os << (i != b ? " " : "") << *i;

    if (ho && ht)
      os << ')';

    return os;
  }

  ostream&
  operator<< (ostream& os, const metaopspec& s)
  {
    bool hm (!s.meta_operation.empty ());
    bool ho (!s.empty ());

    //os << s.meta_operation;
    os << (hm ? "\'" : "") << s.meta_operation << (hm ? "\'" : "");

    if (hm && ho)
      os << '(';

    for (auto b (s.begin ()), i (b); i != s.end (); ++i)
      os << (i != b ? " " : "") << *i;

    if (hm && ho)
      os << ')';

    return os;
  }

  ostream&
  operator<< (ostream& os, const buildspec& s)
  {
    for (auto b (s.begin ()), i (b); i != s.end (); ++i)
      os << (i != b ? " " : "") << *i;

    return os;
  }
}
