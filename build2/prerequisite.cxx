// file      : build2/prerequisite.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/prerequisite>

#include <build2/scope>
#include <build2/target> // target_type
#include <build2/context>
#include <build2/diagnostics>

using namespace std;

namespace build2
{
  // prerequisite_key
  //
  const optional<string> prerequisite_key::nullproj = nullopt;

  ostream&
  operator<< (ostream& os, const prerequisite_key& pk)
  {
    if (pk.proj)
      os << *pk.proj << '%';
    //
    // Don't print scope if we are project-qualified or the prerequisite's
    // directory is absolute. In both these cases the scope is not used to
    // resolve it to target.
    //
    else if (!pk.tk.dir->absolute ())
    {
      // Avoid printing './' in './:...', similar to what we do for the
      // directory in target_key.
      //
      const dir_path& s (pk.scope->out_path ());

      if (stream_verb (os) < 2)
      {
        const string& r (diag_relative (s, false));

        if (!r.empty ())
          os << r << ':';
      }
      else
        os << s << ':';
    }

    return os << pk.tk;
  }

  // prerequisite_set
  //
  auto prerequisite_set::
  insert (optional<string> proj,
          const target_type& tt,
          dir_path dir,
          dir_path out,
          string name,
          optional<string> ext,
          scope& s,
          tracer& trace) -> pair<prerequisite&, bool>
  {
    //@@ OPT: would be nice to somehow first check if this prerequisite is
    //   already in the set before allocating a new instance. Something with
    //   bounds and insert hints?

    // Find or insert.
    //
    auto r (emplace (move (proj),
                     tt,
                     move (dir),
                     move (out),
                     move (name),
                     ext, // Note: cannot move.
                     s));
    prerequisite& p (const_cast<prerequisite&> (*r.first));

    // Update extension if the existing prerequisite has it unspecified.
    //
    if (p.ext != ext)
    {
      l5 ([&]{
          diag_record r (trace);
          r << "assuming prerequisite " << p << " is the same as the "
            << "one with ";
          if (!ext)
            r << "unspecified extension";
          else if (ext->empty ())
            r << "no extension";
          else
            r << "extension " << *ext;
        });

      if (ext)
        const_cast<optional<string>&> (p.ext) = move (ext);
    }

    return pair<prerequisite&, bool> (p, r.second);
  }
}
