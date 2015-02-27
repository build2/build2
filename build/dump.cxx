// file      : build/dump.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/dump>

#include <string>
#include <cassert>
#include <iostream>

#include <build/scope>
#include <build/variable>
#include <build/diagnostics>

using namespace std;

namespace build
{
  static void
  dump_scope (scope& p, scope_map::iterator& i, string& ind)
  {
    string d (diag_relative_work (p.path ()));

    if (d.back () != path::traits::directory_separator)
      d += '/';

    cerr << ind << d << ":" << endl
         << ind << '{' << endl;

    ind += "  ";

    for (const auto& e: p.variables)
    {
      const variable& var (e.first);
      const value_ptr& val (e.second);

      cerr << ind << var.name << " = ";

      if (val == nullptr)
        cerr << "[undefined]";
      else
      {
        //@@ TODO: assuming it is a list.
        //
        cerr << dynamic_cast<list_value&> (*val).data;
      }

      cerr << endl;
    }

    // Print nested scopes of which we are a parent.
    //
    for (auto e (scopes.end ()); i != e && i->second.parent () == &p; )
    {
      scope& s (i->second);
      dump_scope (s, ++i, ind);
    }

    ind.resize (ind.size () - 2);
    cerr << ind << '}' << endl;
  }

  void
  dump_scopes ()
  {
    string ind;
    auto i (scopes.begin ());
    scope& r (i->second); // Root scope.
    assert (&r == root_scope);
    dump_scope (r, ++i, ind);
  }
}
