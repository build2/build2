// file      : build2/dump.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/dump>

#include <build2/scope>
#include <build2/target>
#include <build2/variable>
#include <build2/context>
#include <build2/diagnostics>

using namespace std;

namespace build2
{
  // If type is false, don't print the value's type (e.g., because it is the
  // same as variable's).
  //
  static void
  dump_value (ostream& os, const value& v, bool type)
  {
    // First print attributes if any.
    //
    bool a (v.null () || (type && v.type != nullptr));

    if (a)
      os << '[';

    const char* s ("");

    if (type && v.type != nullptr)
    {
      os << s << v.type->name;
      s = " ";
    }

    if (v.null ())
    {
      os << s << "null";
      s = " ";
    }

    if (a)
      os << ']';

    // Now the value if there is one.
    //
    if (!v.null ())
    {
      names storage;
      os << (a ? " " : "") << reverse (v, storage);
    }
  }

  static void
  dump_variable (ostream& os,
                 const variable& var,
                 const lookup& org,
                 const scope& s,
                 bool target)
  {
    if (var.type != nullptr)
      os << '[' << var.type->name << "] ";

    os << var.name << " = ";

    // If this variable is overriden, print both the override and the
    // original values.
    //
    if (var.override != nullptr &&
        var.name.rfind (".__override") == string::npos &&
        var.name.rfind (".__suffix") == string::npos &&
        var.name.rfind (".__prefix") == string::npos)
    {
      // The original is always from this scope/target, so depth is 1.
      //
      lookup l (s.find_override (var, make_pair (org, 1), target).first);
      assert (l.defined ()); // We at least have  the original.

      if (org != l)
      {
        dump_value (os, *l, l->type != var.type);
        os << " # original: ";
      }
    }

    dump_value (os, *org, org->type != var.type);
  }

  static void
  dump_variables (ostream& os,
                  string& ind,
                  const variable_map& vars,
                  const scope& s,
                  bool target)
  {
    for (const auto& e: vars)
    {
      os << endl
         << ind;

      dump_variable (os, e.first, lookup (&e.second, &vars), s, target);
    }
  }

  static void
  dump_variables (ostream& os,
                  string& ind,
                  const variable_type_map& vtm,
                  const scope& s)
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
          dump_variable (os,
                         vars.begin ()->first,
                         lookup (&vars.begin ()->second, &vars),
                         s,
                         false);
        }
        else
        {
          os << endl
             << ind << '{';
          ind += "  ";
          dump_variables (os, ind, vars, s, false);
          ind.resize (ind.size () - 2);
          os << endl
             << ind << '}';
        }
      }
    }
  }

  static void
  dump_target (ostream& os,
               string& ind,
               action a,
               const target& t,
               const scope& s)
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
      dump_variables (os, ind, t.vars, s, true);
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
    const scope& p (i->second);
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
      dump_variables (os, ind, p.target_vars, p);
      vb = true;
    }

    // Scope variables.
    //
    if (!p.vars.empty ())
    {
      if (vb)
        os << endl;

      dump_variables (os, ind, p.vars, p, false);
      vb = true;
    }

    // Nested scopes of which we are an immediate parent.
    //
    for (auto e (scopes.end ()); i != e && i->second.parent_scope () == &p;)
    {
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
      dump_target (os, ind, a, t, p);
    }

    ind.resize (ind.size () - 2);
    relative_base = orb;

    os << endl
       << ind << '}';
  }

  void
  dump (action a)
  {
    auto i (scopes.cbegin ());
    assert (&i->second == global_scope);

    string ind;
    ostream& os (*diag_stream);
    dump_scope (os, ind, a, i);
    os << endl;
  }
}
