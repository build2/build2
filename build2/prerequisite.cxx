// file      : build2/prerequisite.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/prerequisite>

#include <build2/scope>
#include <build2/target>
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

  // prerequisite
  //
  prerequisite::
  prerequisite (const prerequisite& p, target_type& w)
      : proj (p.proj),
        type (p.type),
        dir (p.dir),
        out (p.out),
        name (p.name),
        ext (p.ext),
        owner (w),
        target (nullptr)
  {
    assert (&w.base_scope () == &p.owner.base_scope ());
  }

  // Make a prerequisite from a target.
  //
  prerequisite::
  prerequisite (target_type& t, target_type& w)
      : proj (nullopt),
        type (t.type ()),
        dir (t.dir),
        out (t.out),   // @@ If it's empty, then we treat as undetermined?
        name (t.name),
        ext (t.ext),
        owner (w),
        target (&t)
  {
  }

  prerequisite_key prerequisite::
  key () const
  {
    return prerequisite_key {
      proj, {&type, &dir, &out, &name, ext}, &owner.base_scope ()};
  }
}
