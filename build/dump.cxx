// file      : build/dump.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/dump>

#include <set>
#include <string>
#include <cassert>

#include <build/scope>
#include <build/target>
#include <build/variable>
#include <build/context>
#include <build/diagnostics>

using namespace std;

namespace build
{
  static void
  dump_target (ostream& os, const target& t)
  {
    os << t << ':';

    for (const prerequisite_target& pe: t.prerequisites)
    {
      os << ' ';

      if (pe.target != nullptr)
        os << *pe.target;
      else
        os << *pe.prereq;
    }
  }

  static void
  dump_scope (ostream& os,
              scope& p,
              scope_map::iterator& i,
              string& ind,
              set<const target*>& rts)
  {
    // We don't want the extra notations (e.g., ~/) provided by
    // diag_relative() since we want the path to be relative to
    // the global scope.
    //
    os << ind << relative (p.path ()) << ":" << endl
       << ind << '{';

    const dir_path* orb (relative_base);
    relative_base = &p.path ();

    ind += "  ";

    bool vb (false), sb (false); // Variable/scope block.

    // Variables.
    //
    for (const auto& e: p.vars)
    {
      const variable& var (e.first);
      const value_ptr& val (e.second);

      os << endl
         << ind << var.name << " = ";

      if (val == nullptr)
        os << "[null]";
      else
      {
        //@@ TODO: assuming it is a list.
        //
        os << dynamic_cast<list_value&> (*val);
      }

      vb = true;
    }

    // Nested scopes of which we are a parent.
    //
    for (auto e (scopes.end ()); i != e && i->second.parent_scope () == &p; )
    {
      if (vb)
      {
        os << endl;
        vb = false;
      }

      if (sb)
        os << endl; // Extra newline between scope blocks.

      os << endl;
      scope& s (i->second);
      dump_scope (os, s, ++i, ind, rts);

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
        os << endl;
        vb = sb = false;
      }

      os << endl
         << ind;
      dump_target (os, t);
    }

    ind.resize (ind.size () - 2);
    relative_base = orb;

    os << endl
       << ind << '}';
  }

  void
  dump ()
  {
    auto i (scopes.begin ());
    scope& g (i->second); // Global scope.
    assert (&g == global_scope);

    string ind;
    set<const target*> rts;

    ostream& os (*diag_stream);
    dump_scope (os, g, ++i, ind, rts);
    os << endl;
  }
}
