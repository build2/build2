// file      : build/prerequisite.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
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
  // prerequisite_key
  //
  ostream&
  operator<< (ostream& os, const prerequisite_key& pk)
  {
    if (pk.proj != nullptr)
      os << *pk.proj << '%';

    // Don't print scope if we are project-qualified or the
    // prerequisite's directory is absolute. In both these
    // cases the scope is not used to resolve it to target.
    //
    else if (!pk.tk.dir->absolute ())
    {
      string s (diag_relative (pk.scope->out_path (), false));

      if (!s.empty ())
        os << s << ':';
    }

    return os << pk.tk;
  }

  // prerequisite_set
  //
  auto prerequisite_set::
  insert (const std::string* proj,
          const target_type& tt,
          dir_path dir,
          std::string name,
          const std::string* ext,
          scope& s,
          tracer& trace) -> pair<prerequisite&, bool>
  {
    //@@ OPT: would be nice to somehow first check if this prerequisite is
    //   already in the set before allocating a new instance.

    // Find or insert.
    //
    auto r (emplace (proj, tt, move (dir), move (name), ext, s));
    prerequisite& p (const_cast<prerequisite&> (*r.first));

    // Update extension if the existing prerequisite has it unspecified.
    //
    if (p.ext != ext)
    {
      level5 ([&]{
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
