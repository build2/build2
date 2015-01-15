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
    tracer tr ("search");

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

    //@@ TODO: would be nice to first check if this target is
    //   already in the set before allocating a new instance.

    // Find or insert.
    //
    auto r (
      targets.emplace (
        unique_ptr<target> (p.type.factory (move (d), p.name, p.ext))));

    target& t (**r.first);

    trace (4, [&]{
        tr << (r.second ? "new" : "existing") << " target " << t
           << " for prerequsite " << p;});

    // Update extension if the existing target has it unspecified.
    //
    if (t.ext != p.ext)
    {
      trace (4, [&]{
          tracer::record r (tr);
          r << "assuming target " << t << " is the same as the one with ";
          if (p.ext == nullptr)
            r << "unspecified extension";
          else if (p.ext->empty ())
            r << "no extension";
          else
            r << "extension " << *p.ext;
        });

      if (p.ext != nullptr)
        t.ext = p.ext;
    }

    return (p.target = &t);
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
      bool single;

      auto rs (hint.empty ()
               ? make_pair (rules.begin (), rules.end ())
               : rules.find (hint));

      for (auto i (rs.first); i != rs.second;)
      {
        const string& n (i->first);
        const rule& ru (i->second);

        if (i++ == rs.first)
          single = (i == rs.second);

        recipe re;
        string h (hint);
        {
          auto g (
            make_exception_guard (
              [] (target& t, const string& n)
              {
                cerr << "info: while matching rule " << n
                     << " for target " << t << endl;
              },
              t, n));

          // If the rule matches, then it updates the hint with the one we
          // need to use when checking for ambiguity.
          //
          re = ru.match (t, single, h);
        }

        if (re)
        {
          t.recipe (re);

          // If the returned hint is more "general" than what we had,
          // then narrow it back down.
          //
          if (h < hint)
            h = hint;

          // Do the ambiguity test unless it is an unambiguous match (the
          // hint is the rule's full name).
          //
          if (h == n)
            break;

          auto rs1 (h == hint
                    ? make_pair (i, rs.second) // Continue iterating.
                    : rules.find (h));

          bool ambig (false);

          // See if any other rules match.
          //
          for (auto i (rs1.first); i != rs1.second; ++i)
          {
            const string& n1 (i->first);
            const rule& ru1 (i->second);

            string h1 (h);
            {
              auto g (
                make_exception_guard (
                  [] (target& t, const string& n1)
                  {
                    cerr << "info: while matching rule " << n1
                         << " for target " << t << endl;
                  },
                  t, n1));

              re = ru1.match (t, false, h1);
            }

            if (re)
            {
              // A subsequent rule cannot return a more specific hint.
              // Remember, the hint returning mechanism is here to
              // indicate that only a class of rules that perform a
              // similar rule chaining transformation may apply (e.g.,
              // cxx.gnu and cxx.clang).
              //
              assert (h1 <= h);

              if (!ambig)
              {
                cerr << "error: multiple rules matching target " << t << endl;
                cerr << "info: rule " << n << " matches" << endl;
                ambig = true;
              }

              cerr << "info: rule " << n1 << " also matches" << endl;
            }
          }

          if (ambig)
          {
            cerr << "info: use rule hint to disambiguate this match" << endl;
            throw error ();
          }

          break;
        }
      }
    }

    return bool (t.recipe ());
  }
}
