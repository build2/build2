// file      : build/algorithm.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/algorithm>

#include <memory>   // unique_ptr
#include <utility>  // move
#include <cassert>
#include <iostream>

#include <build/path>
#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/rule>
#include <build/diagnostics>

using namespace std;

namespace build
{
  target*
  search (prerequisite& p)
  {
    assert (p.target == nullptr);

    //@@ TODO for now we just default to the directory scope.
    //
    path d;
    if (p.directory.absolute ())
      d = p.directory; // Already normalized.
    else
    {
      d = p.scope.path () / p.directory;
      d.normalize ();
    }

    //@@ TODO would be nice to first check if this target is
    //   already in the set before allocating a new instance.

    // Find or insert.
    //
    auto r (
      targets.emplace (
        unique_ptr<target> (p.type.factory (p.name, move (d)))));

    //if (r.second)
    //  cout << "new target for prerequsite " << p << " " << d << endl;

    return (p.target = r.first->get ());
  }

  bool
  match (target& t)
  {
    assert (!t.recipe ());

    for (auto tt (&t.type ());
         tt != nullptr && !t.recipe ();
         tt = tt->base)
    {
      for (auto rs (rules.equal_range (tt->id));
           rs.first != rs.second;
           ++rs.first)
      {
        const rule& ru (rs.first->second);

        recipe re;

        {
          auto g (
            make_exception_guard (
              [] (target& t)
              {
                cerr << "info: while matching a rule for target " << t << endl;
              },
              t));

          re = ru.match (t);
        }

        if (re)
        {
          t.recipe (re);
          break;
        }
      }
    }

    return bool (t.recipe ());
  }
}
