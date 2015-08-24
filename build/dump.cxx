// file      : build/dump.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
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
  dump_variable (ostream& os, const variable& var, const value& val)
  {
    os << var.name << " = ";

    if (val.null ())
      os << "[null]";
    else
      os << val.data_;
  }

  static void
  dump_variables (ostream& os, string& ind, const variable_map& vars)
  {
    for (const auto& e: vars)
    {
      os << endl
         << ind;

      dump_variable (os, e.first, e.second);
    }
  }

  static void
  dump_variables (ostream& os, string& ind, const variable_type_map& vtm)
  {
    for (const auto& vt: vtm)
    {
      const target_type& t (vt.first);
      const variable_pattern_map& vpm (vt.second);

      for (const auto& vp: vpm)
      {
        const string p (vp.first);
        const variable_map& vars (vp.second);

        os << endl
           << ind;

        if (t.id != target::static_type.id)
          os << t.name << '{';

        os << p;

        if (t.id != target::static_type.id)
          os << '}';

        os << ':';

        if (vars.size () == 1)
        {
          os << ' ';
          dump_variable (os, vars.begin ()->first, vars.begin ()->second);
        }
        else
        {
          os << endl
             << ind << '{';
          ind += "  ";
          dump_variables (os, ind, vars);
          ind.resize (ind.size () - 2);
          os << endl
             << ind << '}';
        }
      }
    }
  }

  static void
  dump_target (ostream& os, string& ind, action a, const target& t)
  {
    os << ind << t;

    if (t.group != nullptr)
      os << "->" << *t.group;

    os << ':';

    for (const prerequisite& p: t.prerequisites)
    {
      os << ' ';

      // Print it as target if one has been cached.
      //
      if (p.target != nullptr)
        os << *p.target;
      else
        os << p;
    }

    // If the target has been matched to a rule, also print resolved
    // prerequisite targets.
    //
    if (t.recipe (a))
    {
      bool first (true);
      for (const target* pt: t.prerequisite_targets)
      {
        if (pt == nullptr) // Skipped.
          continue;

        os << (first ? " | " : " ") << *pt;
        first = false;
      }
    }

    // Print target-specific variables.
    //
    if (!t.vars.empty ())
    {
      os << endl
         << ind << '{';
      ind += "  ";
      dump_variables (os, ind, t.vars);
      ind.resize (ind.size () - 2);
      os << endl
         << ind << '}';
    }
  }

  static void
  dump_scope (ostream& os,
              string& ind,
              action a,
              scope& p,
              scope_map::iterator& i,
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

    // Target type/pattern-sepcific variables.
    //
    if (!p.target_vars.empty ())
    {
      dump_variables (os, ind, p.target_vars);
      vb = true;
    }

    // Scope variables.
    //
    if (!p.vars.empty ())
    {
      if (vb)
        os << endl;

      dump_variables (os, ind, p.vars);
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
      dump_scope (os, ind, a, s, ++i, rts);

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

      os << endl;
      dump_target (os, ind, a, t);
    }

    ind.resize (ind.size () - 2);
    relative_base = orb;

    os << endl
       << ind << '}';
  }

  void
  dump (action a)
  {
    auto i (scopes.begin ());
    scope& g (i->second); // Global scope.
    assert (&g == global_scope);

    string ind;
    set<const target*> rts;

    ostream& os (*diag_stream);
    dump_scope (os, ind, a, g, ++i, rts);
    os << endl;
  }
}
