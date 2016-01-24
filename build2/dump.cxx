// file      : build2/dump.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/dump>

#include <string>
#include <cassert>

#include <build2/scope>
#include <build2/target>
#include <build2/variable>
#include <build2/context>
#include <build2/diagnostics>

using namespace std;

namespace build2
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

        if (t != target::static_type)
          os << t.name << '{';

        os << p;

        if (t != target::static_type)
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
    // Print the target and its prerequisites relative to the scope. To achieve
    // this we are going to temporarily lower the stream verbosity to level 1.
    // The drawback of doing this is that we also lower the verbosity of
    // extension printing (it wouldn't have been bad at all to get 'foo.?' for
    // unassigned and 'foo.' for empty).
    //
    uint16_t sv (stream_verb (os));
    stream_verb (os, 1);

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

    stream_verb (os, sv); // We want variable values in full.

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
              scope_map::const_iterator& i)
  {
    scope& p (*i->second);
    const dir_path& d (i->first);
    ++i;

    // We don't want the extra notations (e.g., ~/) provided by
    // diag_relative() since we want the path to be relative to
    // the outer scope.
    //
    os << ind << relative (d) << ":" << endl
       << ind << '{';

    const dir_path* orb (relative_base);
    relative_base = &d;

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

    // Nested scopes of which we are an immediate parent.
    //
    for (auto e (scopes.end ()); i != e && i->second->parent_scope () == &p;)
    {
      // See what kind of scope entry this is. It can be:
      //
      // 1. Out-of-project scope.
      // 2. In-project out entry.
      // 3. In-project src entry.
      //
      // We want to print #2 and #3 as a single, unified scope.
      //
      scope& s (*i->second);
      if (s.src_path_ != s.out_path_ && s.src_path_ == &i->first)
      {
        ++i;
        continue;
      }

      if (vb)
      {
        os << endl;
        vb = false;
      }

      if (sb)
        os << endl; // Extra newline between scope blocks.

      os << endl;
      dump_scope (os, ind, a, i);
      sb = true;
    }

    // Targets.
    //
    for (const auto& pt: targets)
    {
      const target& t (*pt);

      if (&p != &t.base_scope ())
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
    assert (i->second == global_scope);

    string ind;
    ostream& os (*diag_stream);
    dump_scope (os, ind, a, i);
    os << endl;
  }
}
