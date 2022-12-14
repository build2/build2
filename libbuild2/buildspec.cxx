// file      : libbuild2/buildspec.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/buildspec.hxx>

#include <libbuild2/diagnostics.hxx>

using namespace std;

namespace build2
{
  ostream&
  operator<< (ostream& os, const targetspec& s)
  {
    if (!s.src_base.empty ())
    {
      // Avoid printing './' in './@...', similar to what we do for the
      // {target,prerequisite}_key.
      //
      if (stream_verb (os).path < 1)
      {
        const string& r (diag_relative (s.src_base, false));

        if (!r.empty ())
          os << r << '@';
      }
      else
        os << s.src_base << '@';
    }

    os << s.name;
    return os;
  }

  ostream&
  operator<< (ostream& os, const opspec& s)
  {
    bool hn (!s.name.empty ());
    bool ht (!s.empty ());

    os << (hn ? "\"" : "") << s.name << (hn ? "\"" : "");

    if (hn && ht)
      os << '(';

    for (auto b (s.begin ()), i (b); i != s.end (); ++i)
      os << (i != b ? " " : "") << *i;

    for (const value& v: s.params)
    {
      os << ", ";

      if (v)
      {
        names storage;
        os << reverse (v, storage, true /* reduce */);
      }
      else
        os << "[null]";
    }

    if (hn && ht)
      os << ')';

    return os;
  }

  ostream&
  operator<< (ostream& os, const metaopspec& s)
  {
    bool hn (!s.name.empty ());
    bool ho (!s.empty ());

    os << (hn ? "\'" : "") << s.name << (hn ? "\'" : "");

    if (hn && ho)
      os << '(';

    for (auto b (s.begin ()), i (b); i != s.end (); ++i)
      os << (i != b ? " " : "") << *i;

    for (const value& v: s.params)
    {
      os << ", ";

      if (v)
      {
        names storage;
        os << reverse (v, storage, true /* reduce */);
      }
      else
        os << "[null]";
    }

    if (hn && ho)
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
