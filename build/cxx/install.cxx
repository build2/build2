// file      : build/cxx/install.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/install>

#include <build/bin/target>

#include <build/cxx/target>
#include <build/cxx/link>

using namespace std;

namespace build
{
  namespace cxx
  {
    using namespace bin;

    bool install::
    filter (action, target& t, prerequisite_member p) const
    {
      // Don't install executable's prerequisite headers.
      //
      if (t.is_a<exe> () &&
          (p.is_a<hxx> () || p.is_a<ixx> () || p.is_a<txx> () || p.is_a<h> ()))
        return false;

      return true;
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

      return r ? install::rule::match (a, t, "") : r;
    }

    install install::instance;
  }
}
