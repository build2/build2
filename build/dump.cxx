// file      : build/dump.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/dump>

#include <set>
#include <string>
#include <cassert>
#include <iostream>

#include <build/scope>
#include <build/target>
#include <build/variable>
#include <build/context>
#include <build/diagnostics>

using namespace std;

namespace build
{
  static void
  dump_target (const target& t)
  {
    cerr << t << ':';

    for (const prerequisite& p: t.prerequisites)
    {
      cerr << ' ' << p;
    }
  }

  static void
  dump_scope (scope& p,
              scope_map::iterator& i,
              string& ind,
              set<const target*>& rts)
  {
    string d (diag_relative (p.path ()));

    if (d.back () != path::traits::directory_separator)
      d += '/';

    cerr << ind << d << ":" << endl
         << ind << '{';

    const path* orb (relative_base);
    relative_base = &p.path ();

    ind += "  ";

    bool vb (false), sb (false); // Variable/scope block.

    // Variables.
    //
    for (const auto& e: p.variables)
    {
      const variable& var (e.first);
      const value_ptr& val (e.second);

      cerr << endl
           << ind << var.name << " = ";

      if (val == nullptr)
        cerr << "[undefined]";
      else
      {
        //@@ TODO: assuming it is a list.
        //
        cerr << dynamic_cast<list_value&> (*val).data;
      }

      vb = true;
    }

    // Nested scopes of which we are a parent.
    //
    for (auto e (scopes.end ()); i != e && i->second.parent_scope () == &p; )
    {
      if (vb)
      {
        cerr << endl;
        vb = false;
      }

      if (sb)
        cerr << endl; // Extra newline between scope blocks.

      cerr << endl;
      scope& s (i->second);
      dump_scope (s, ++i, ind, rts);

      sb = true;
    }

    // Targets.
    //
    for (const auto& pt: targets)
    {
      const target& t (*pt);
      const scope* ts (&t.base_scope ());

      bool f (false);

      if (ts == &p)
      {
        // If this is the global scope, check that this target hasn't
        // been handled by the src logic below.
        //
        f = (ts != global_scope || rts.find (&t) == rts.end ());
      }
      // If this target is in the global scope and we have a corresponding
      // src directory (i.e., we are a scope inside a project), check
      // whether this target is in our src.
      //
      else if (ts == global_scope && p.src_path_ != nullptr)
      {
        if (t.dir.sub (p.src_path ()))
        {
          // Check that it hasn't already been handled by a more qualified
          // scope.
          //
          f = rts.insert (&t).second;
        }
      }

      if (!f)
        continue;

      if (vb || sb)
      {
        cerr << endl;
        vb = sb = false;
      }

      cerr << endl
           << ind;
      dump_target (t);
    }

    ind.resize (ind.size () - 2);
    relative_base = orb;

    cerr << endl
         << ind << '}';
  }

  void
  dump ()
  {
    string ind;
    set<const target*> rts;
    auto i (scopes.begin ());
    scope& g (i->second); // Global scope.
    assert (&g == global_scope);
    dump_scope (g, ++i, ind, rts);
    cerr << endl;
  }
}
