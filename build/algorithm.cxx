// file      : build/algorithm.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/algorithm>

#include <memory>   // unique_ptr
#include <utility>  // move
#include <cassert>

#include <build/path>
#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/rule>
#include <build/utility>
#include <build/diagnostics>

using namespace std;

namespace build
{
  target&
  search (prerequisite& p)
  {
    tracer trace ("search");

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

    // Find or insert.
    //
    auto r (targets.insert (p.type, move (d), p.name, p.ext, trace));

    level4 ([&]{trace << (r.second ? "new" : "existing") << " target "
                      << r.first << " for prerequsite " << p;});

    p.target = &r.first;
    return r.first;
  }

  bool
  match (target& t)
  {
    assert (!t.recipe ());

    for (auto tt (&t.type ());
         tt != nullptr && !t.recipe ();
         tt = tt->base)
    {
      auto i (rules.find (tt->id));

      if (i == rules.end ()) // No rules registered for this target type.
        continue;

      const auto& rules (i->second); // Name map.

      string hint; // @@ TODO

      auto rs (hint.empty ()
               ? make_pair (rules.begin (), rules.end ())
               : rules.find (hint));

      for (auto i (rs.first); i != rs.second; ++i)
      {
        const string& n (i->first);
        const rule& ru (i->second);

        void* m;
        {
          auto g (
            make_exception_guard (
              [](target& t, const string& n)
              {
                info << "while matching rule " << n << " for target " << t;
              },
              t, n));

          m = ru.match (t, hint);
        }

        if (m != nullptr)
        {
          // Do the ambiguity test.
          //
          bool ambig (false);

          diag_record dr;

          for (++i; i != rs.second; ++i)
          {
            const string& n1 (i->first);
            const rule& ru1 (i->second);

            void* m1;
            {
              auto g (
                make_exception_guard (
                  [](target& t, const string& n1)
                  {
                    info << "while matching rule " << n1 << " for target "
                         << t;
                  },
                  t, n1));

              m1 = ru1.match (t, hint);
            }

            if (m1 != nullptr)
            {
              if (!ambig)
              {
                dr << fail << "multiple rules matching target " << t
                   << info << "rule " << n << " matches";
                ambig = true;
              }

              dr << info << "rule " << n1 << " also matches";
            }
          }

          if (!ambig)
          {
            t.recipe (ru.select (t, m));
            break;
          }
          else
            dr << info << "use rule hint to disambiguate this match";
        }
      }
    }

    return bool (t.recipe ());
  }
}
