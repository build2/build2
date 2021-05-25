// file      : libbuild2/dump.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/dump.hxx>

#include <libbuild2/rule.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

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

  enum class variable_kind {scope, tt_pat, target, rule, prerequisite};

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
      // @@ The override semantics for prerequisite-specific variables
      //    is still fuzzy/unimplemented, so ignore it for now.
      //
      if (k != variable_kind::prerequisite)
      {
        if (var.overrides != nullptr && !var.override ())
        {
          lookup org (v, var, vm);

          // The original is always from this scope/target, so depth is 1.
          //
          lookup l (
            s.lookup_override (
              var,
              make_pair (org, 1),
              k == variable_kind::target || k == variable_kind::rule,
              k == variable_kind::rule).first);

          assert (l.defined ()); // We at least have the original.

          if (org != l)
          {
            dump_value (os, *l, l->type != var.type);
            os << " # original: ";
          }
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
    using pattern = variable_pattern_map::pattern;
    using pattern_type = variable_pattern_map::pattern_type;

    for (const auto& vt: vtm)
    {
      const target_type& t (vt.first);
      const variable_pattern_map& vpm (vt.second);

      for (const auto& vp: vpm)
      {
        const pattern& pat (vp.first);
        const variable_map& vars (vp.second);

        os << endl
           << ind;

        if (t != target::static_type)
          os << t.name << '{';

        if (pat.type == pattern_type::regex_pattern)
          os << '~';

        os << pat.text;

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
  dump_target (optional<action> a,
               ostream& os,
               string& ind,
               const target& t,
               const scope& s,
               bool rel)
  {
    // If requested, print the target and its prerequisites relative to the
    // scope. To achieve this we are going to temporarily lower the stream
    // path verbosity to level 0.
    //
    stream_verbosity osv, nsv;
    if (rel)
    {
      osv = nsv = stream_verb (os);
      nsv.path = 0;
      stream_verb (os, nsv);
    }

    if (t.group != nullptr)
      os << ind << t << " -> " << *t.group << endl;

    os << ind << t << ':';

    // First check if this is the simple case where we can print everything
    // as a single declaration.
    //
    const prerequisites& ps (t.prerequisites ());
    bool simple (true);
    for (const prerequisite& p: ps)
    {
      if (!p.vars.empty ()) // Has prerequisite-specific vars.
      {
        simple = false;
        break;
      }
    }

    // If the target has been matched to a rule, we also print resolved
    // prerequisite targets.
    //
    // Note: running serial and task_count is 0 before any operation has
    // started.
    //
    const prerequisite_targets* pts (nullptr);
    {
      action inner; // @@ Only for the inner part of the action currently.

      if (size_t c = t[inner].task_count.load (memory_order_relaxed))
      {
        if (c == t.ctx.count_applied () || c == t.ctx.count_executed ())
        {
          pts = &t.prerequisite_targets[inner];

          bool f (false);
          for (const target* pt: *pts)
          {
            if (pt != nullptr)
            {
              f = true;
              break;
            }
          }

          if (!f)
            pts = nullptr;
        }
      }
    }

    auto print_pts = [&os, &ps, pts] ()
    {
      for (const target* pt: *pts)
      {
        if (pt != nullptr)
          os << ' ' << *pt;
      }

      // Only omit '|' if we have no prerequisites nor targets.
      //
      if (!ps.empty ())
      {
        os << " |";
        return true;
      }

      return false;
    };

    if (simple)
    {
      if (pts != nullptr)
        print_pts ();

      for (const prerequisite& p: ps)
      {
        // Print it as a target if one has been cached.
        //
        if (const target* t = p.target.load (memory_order_relaxed)) // Serial.
          os << ' ' << *t;
        else
          os << ' ' << p;
      }
    }

    bool used (false); // Target header has been used.

    // Print target/rule-specific variables, if any.
    //
    {
      bool tv (!t.vars.empty ());
      bool rv (a && !t.state[*a].vars.empty ());

      if (tv || rv)
      {
        if (rel)
          stream_verb (os, osv); // We want variable values in full.

        os << endl
           << ind << '{';
        ind += "  ";

        if (tv)
          dump_variables (os, ind, t.vars, s, variable_kind::target);

        if (rv)
        {
          // To distinguish target and rule-specific variables, we put the
          // latter into a nested block.
          //
          // @@ Maybe if we also print the rule name, then we could make
          //    the block associated with that?

          if (tv)
            os << endl;

          os << endl
             << ind << '{';
          ind += "  ";
          dump_variables (os, ind, t.state[*a].vars, s, variable_kind::rule);
          ind.resize (ind.size () - 2);
          os << endl
             << ind << '}';
        }

        ind.resize (ind.size () - 2);
        os << endl
           << ind << '}';

        if (rel)
          stream_verb (os, nsv);

        used = true;
      }
    }

    // Then ad hoc recipes, if any.
    //
    if (!t.adhoc_recipes.empty ())
    {
      auto& re (*s.root_scope ()->root_extra);

      for (const adhoc_recipe& r: t.adhoc_recipes)
      {
        os << endl
           << ind << '%';

        r.rule->dump_attributes (os);

        for (action a: r.actions)
          os << ' ' << re.meta_operations[a.meta_operation ()]->name <<
            '(' << re.operations[a.operation ()]->name << ')';

        os << endl;
        r.rule->dump_text (os, ind);
      }

      used = true;
    }

    if (!simple)
    {
      if (used)
      {
        os << endl
           << ind << t << ':';

        used = false;
      }

      if (pts != nullptr)
        used = print_pts () || used;

      // Print prerequisites. Those that have prerequisite-specific variables
      // have to be printed as a separate dependency.
      //
      for (auto i (ps.begin ()), e (ps.end ()); i != e; )
      {
        const prerequisite& p (*i++);
        bool ps (!p.vars.empty ()); // Has prerequisite-specific vars.

        if (ps && used) // If it has been used, get a new header.
          os << endl
             << ind << t << ':';

        // Print it as a target if one has been cached.
        //
        if (const target* t = p.target.load (memory_order_relaxed)) // Serial.
          os << ' ' << *t;
        else
          os << ' ' << p;

        if (ps)
        {
          if (rel)
            stream_verb (os, osv); // We want variable values in full.

          os << ':' << endl
             << ind << '{';
          ind += "  ";
          dump_variables (os, ind, p.vars, s, variable_kind::prerequisite);
          ind.resize (ind.size () - 2);
          os << endl
             << ind << '}';

          if (rel)
            stream_verb (os, nsv);

          if (i != e) // If we have another, get a new header.
            os << endl
               << ind << t << ':';
        }

        used = !ps;
      }
    }

    if (rel)
      stream_verb (os, osv);
  }

  static void
  dump_scope (optional<action> a,
              ostream& os,
              string& ind,
              scope_map::const_iterator& i,
              bool rel)
  {
    const scope& p (*i->second.front ());
    const dir_path& d (i->first);
    ++i;

    // We don't want the extra notations (e.g., ~/) provided by diag_relative()
    // since we want the path to be relative to the outer scope. Print the root
    // scope path (represented by an empty one) as a platform-dependent path
    // separator.
    //
    if (d.empty ())
      os << ind << dir_path::traits_type::directory_separator;
    else
    {
      const dir_path& rd (rel ? relative (d) : d);
      os << ind << (rd.empty () ? dir_path (".") : rd);
    }

    os << endl
       << ind << '{';

    const dir_path* orb (relative_base);
    relative_base = &d;

    ind += "  ";

    bool vb (false), sb (false), tb (false); // Variable/scope/target block.

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

    // Nested scopes of which we are an immediate parent. Only consider the
    // out hierarchy.
    //
    // Note that because we use the logical (rather than physical) parent, we
    // will be printing the logical scope hierarchy (i.e., a project with
    // disabled amalgamation will be printed directly inside the global
    // scope).
    //
    for (auto e (p.ctx.scopes.end ());
         (i != e &&
          i->second.front () != nullptr &&
          i->second.front ()->parent_scope () == &p); )
    {
      if (vb)
      {
        os << endl;
        vb = false;
      }

      if (sb)
        os << endl; // Extra newline between scope blocks.

      os << endl;
      dump_scope (a, os, ind, i, true /* relative */);
      sb = true;
    }

    // Targets.
    //
    // Since targets can occupy multiple lines, we separate them with a
    // blank line.
    //
    for (const auto& pt: p.ctx.targets)
    {
      const target& t (*pt);

      if (&p != &t.base_scope ())
        continue;

      if (vb || sb || tb)
      {
        os << endl;
        vb = sb = false;
      }

      os << endl;
      dump_target (a, os, ind, t, p, true /* relative */);
      tb = true;
    }

    ind.resize (ind.size () - 2);
    relative_base = orb;

    os << endl
       << ind << '}';
  }

  void
  dump (const context& c, optional<action> a)
  {
    auto i (c.scopes.begin ());
    assert (i->second.front () == &c.global_scope);

    // We don't lock diag_stream here as dump() is supposed to be called from
    // the main thread prior/after to any other threads being spawned.
    //
    string ind;
    ostream& os (*diag_stream);
    dump_scope (a, os, ind, i, false /* relative */);
    os << endl;
  }

  void
  dump (const scope& s, const char* cind)
  {
    const scope_map& m (s.ctx.scopes);
    auto i (m.find_exact (s.out_path ()));
    assert (i != m.end () && i->second.front () == &s);

    string ind (cind);
    ostream& os (*diag_stream);
    dump_scope (nullopt /* action */, os, ind, i, false /* relative */);
    os << endl;
  }

  void
  dump (const target& t, const char* cind)
  {
    string ind (cind);
    ostream& os (*diag_stream);
    dump_target (nullopt /* action */,
                 os,
                 ind,
                 t,
                 t.base_scope (),
                 false /* relative */);
    os << endl;
  }
}
