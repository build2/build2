// file      : build2/cxx/install.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/install>

#include <build2/bin/target>

#include <build2/cxx/target>
#include <build2/cxx/link>

using namespace std;

namespace build2
{
  namespace cxx
  {
    using namespace bin;

    target* install::
    filter (action a, target& t, prerequisite_member p) const
    {
      if (t.is_a<exe> ())
      {
        // Don't install executable's prerequisite headers.
        //
        if (p.is_a<hxx> () || p.is_a<ixx> () || p.is_a<txx> () || p.is_a<h> ())
          return nullptr;
      }

      // If this is a shared library prerequisite, install it as long as it
      // is in the same amalgamation as we are.
      //
      if ((t.is_a<exe> () || t.is_a<libso> ()) &&
          (p.is_a<lib> () || p.is_a<libso> ()))
      {
        target* pt (&p.search ());

        // If this is the lib{} group, pick a member which we would link.
        //
        if (lib* l = pt->is_a<lib> ())
          pt = &link::link_member (*l, link::link_order (t));

        if (pt->is_a<libso> ()) // Can be liba{}.
          return pt->in (t.weak_scope ()) ? pt : nullptr;
      }

      return file_rule::filter (a, t, p);
    }

    match_result install::
    match (action a, target& t, const std::string& hint) const
    {
      // @@ How do we split the hint between the two?
      //

      // We only want to handle installation if we are also the
      // ones building this target. So first run link's match().
      //
      match_result r (link::instance.match (a, t, hint));
      return r ? install::file_rule::match (a, t, "") : r;
    }

    install install::instance;
  }
}
