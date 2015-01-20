// file      : build/prerequisite.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/prerequisite>

#include <ostream>

#include <build/scope>
#include <build/target> // target_type
#include <build/context>
#include <build/diagnostics>

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

  // prerequisite_set
  //
  auto prerequisite_set::
  insert (const target_type& tt,
          path dir,
          std::string name,
          const std::string* ext,
          scope& s,
          tracer& trace) -> pair<prerequisite&, bool>
  {
    //@@ OPT: would be nice to somehow first check if this prerequisite is
    //   already in the set before allocating a new instance.

    // Find or insert.
    //
    auto r (emplace (tt, move (dir), move (name), ext, s));
    prerequisite& p (const_cast<prerequisite&> (*r.first));

    // Update extension if the existing prerequisite has it unspecified.
    //
    if (p.ext != ext)
    {
      level4 ([&]{
          diag_record r (trace);
          r << "assuming prerequisite " << p << " is the same as the "
            << "one with ";
          if (ext == nullptr)
            r << "unspecified extension";
          else if (ext->empty ())
            r << "no extension";
          else
            r << "extension " << *ext;
        });

      if (ext != nullptr)
        p.ext = ext;
    }

    return pair<prerequisite&, bool> (p, r.second);
  }
}
