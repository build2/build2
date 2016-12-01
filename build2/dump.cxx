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
    bool a (!v || (type && v.type != nullptr));

    if (a)
      os << '[';

    const char* s ("");

    if (type && v.type != nullptr)
    {
      os << s << v.type->name;
      s = " ";
    }

    if (!v)
    {
      os << s << "null";
      s = " ";
    }

    if (a)
      os << ']';

    // Now the value if there is one.
    //
    if (v)
    {
      names storage;
      os << (a ? " " : "") << reverse (v, storage);
    }
  }

  enum class variable_kind {scope, tt_pat, target};

  static void
  dump_variable (ostream& os,
                 const variable_map& vm,
                 const variable_map::const_iterator& vi,
                 const scope& s,
                 variable_kind k)
  {
    // Target type/pattern-specific prepends/appends are kept untyped and not
    // overriden.
    //
    if (k == variable_kind::tt_pat && vi.extra () != 0)
    {
      // @@ Might be useful to dump the cache.
      //
      const auto& p (vi.untyped ());
      const variable& var (p.first);
      const value& v (p.second);
      assert (v.type == nullptr);

      os << var << (v.extra == 1 ? " =+ " : " += ");
      dump_value (os, v, false);
    }
    else
    {
      const auto& p (*vi);
      const variable& var (p.first);
      const value& v (p.second);

      if (var.type != nullptr)
        os << '[' << var.type->name << "] ";

      os << var << " = ";

      // If this variable is overriden, print both the override and the
      // original values.
      //
      if (var.override != nullptr &&
          var.name.rfind (".__override") == string::npos &&
          var.name.rfind (".__suffix") == string::npos &&
          var.name.rfind (".__prefix") == string::npos)
      {
        lookup org (v, vm);

        // The original is always from this scope/target, so depth is 1.
        //
        lookup l (
          s.find_override (
            var, make_pair (org, 1), k == variable_kind::target).first);

        assert (l.defined ()); // We at least have the original.

        if (org != l)
        {
          dump_value (os, *l, l->type != var.type);
          os << " # original: ";
        }
      }

      dump_value (os, v, v.type != var.type);
    }
  }

  static void
  dump_variables (ostream& os,
                  string& ind,
                  const variable_map& vars,
                  const scope& s,
                  variable_kind k)
  {
    for (auto i (vars.begin ()), e (vars.end ()); i != e; ++i)
    {
      os << endl
         << ind;

      dump_variable (os, vars, i, s, k);
    }
  }

  // Dump target type/pattern-specific variables.
  //
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
          dump_variable (os, vars, vars.begin (), s, variable_kind::tt_pat);
        }
        else
        {
          os << endl
             << ind << '{';
          ind += "  ";
          dump_variables (os, ind, vars, s, variable_kind::tt_pat);
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
      dump_variables (os, ind, t.vars, s, variable_kind::target);
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

    // We don't want the extra notations (e.g., ~/) provided by diag_relative()
    // since we want the path to be relative to the outer scope. Print the root
    // scope path (represented by an empty one) as a platform-dependent path
    // separator.
    //
    if (d.empty ())
      os << ind << dir_path::traits::directory_separator;
    else
      os << ind << relative (d);

    os << ":" << endl << ind << '{';

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

      dump_variables (os, ind, p.vars, p, variable_kind::scope);
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
