// file      : libbuild2/dump.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/dump.hxx>

#ifndef BUILD2_BOOTSTRAP
#  include <iostream>  // cout
#  include <unordered_map>
#endif

#include <libbuild2/rule.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

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
      os << (a ? " " : "") << reverse (v, storage, true /* reduce */);
    }
  }

#ifndef BUILD2_BOOTSTRAP

  static string
  quoted_target_name (const names_view& ns, bool rel)
  {
    ostringstream os;
    stream_verb (os, stream_verbosity (rel ? 0 : 1, 0));
    to_stream (os, ns, quote_mode::effective, '@');
    return os.str ();
  }

  static void
  dump_quoted_target_name (json::stream_serializer& j,
                           const names_view& ns,
                           bool rel)
  {
    j.value (quoted_target_name (ns, rel));
  }

  static string
  quoted_target_name (const target& t, bool rel)
  {
    names ns (t.as_name ()); // Note: potentially adds an extension.

    // Don't print target names relative if the target is in src and out!=src.
    // Failed that, we will end up with pointless ../../../... paths.
    //
    // It may also seem that we can omit @-qualification in this case, since
    // it is implied by the containing scope. However, keep in mind that the
    // target may not be directly in this scope. We could make it relative,
    // though.
    //
    if (rel && !t.out.empty ())
    {
      // Make the out relative ourselves and then disable relative for src.
      //
      dir_path& o (ns.back ().dir);
      o = relative (o); // Note: may return empty path.
      if (o.empty ())
        o = dir_path (".");

      rel = false;
    }

    return quoted_target_name (ns, rel);
  }

  void
  dump_quoted_target_name (json::stream_serializer& j,
                           const target& t,
                           bool rel)
  {
    j.value (quoted_target_name (t, rel));
  }

  using target_name_cache = unordered_map<const target*, string>;

  static void
  dump_quoted_target_name (json::stream_serializer& j,
                           const target& t,
                           target_name_cache& tc)
  {
    auto i (tc.find (&t));
    if (i == tc.end ())
      i = tc.emplace (&t, quoted_target_name (t, false /* relative */)).first;

    j.value (i->second);
  }

  void
  dump_display_target_name (json::stream_serializer& j,
                            const target& t,
                            bool rel)
  {
    // Note: see the quoted version above for details.

    target_key tk (t.key ());

    dir_path o;
    if (rel && !tk.out->empty ())
    {
      o = relative (*tk.out);
      if (o.empty ())
        o = dir_path (".");
      tk.out = &o;

      rel = false;
    }

    // Change the stream verbosity to print relative if requested and omit
    // extension.
    //
    ostringstream os;
    stream_verb (os, stream_verbosity (rel ? 0 : 1, 0));
    os << tk;
    j.value (os.str ());
  }

  static void
  dump_value (json::stream_serializer& j, const value& v)
  {
    // Hints.
    //
    // Note that the pair hint should only be used for simple names.
    //
    optional<bool> h_array;
    optional<bool> h_pair; // true/false - second/first is optional.

    if (v.null)
    {
      j.value (nullptr);
      return;
    }
    else if (v.type != nullptr)
    {
      const value_type& t (*v.type);

      auto s_array = [&j] (const auto& vs)
      {
        j.begin_array ();
        for (const auto& v: vs) j.value (v);
        j.end_array ();
      };

      auto s_array_string = [&j] (const auto& vs)
      {
        j.begin_array ();
        for (const auto& v: vs) j.value (v.string ());
        j.end_array ();
      };

      // Note: check in the derived-first order.
      //
      if      (t.is_a<bool>           ()) j.value (v.as<bool> ());
      else if (t.is_a<int64_t>        ()) j.value (v.as<int64_t> ());
      else if (t.is_a<uint64_t>       ()) j.value (v.as<uint64_t> ());
      else if (t.is_a<string>         ()) j.value (v.as<string> ());
      else if (t.is_a<path>           ()) j.value (v.as<path> ().string ());
      else if (t.is_a<dir_path>       ()) j.value (v.as<dir_path> ().string ());
      else if (t.is_a<target_triplet> ()) j.value (v.as<target_triplet> ().string ());
      else if (t.is_a<project_name>   ()) j.value (v.as<project_name> ().string ());
      else if (t.is_a<int64s>         ()) s_array (v.as<int64s> ());
      else if (t.is_a<uint64s>        ()) s_array (v.as<uint64s> ());
      else if (t.is_a<strings>        ()) s_array (v.as<strings> ());
      else if (t.is_a<paths>          ()) s_array_string (v.as<paths> ());
      else if (t.is_a<dir_paths>      ()) s_array_string (v.as<dir_paths> ());
      else
      {
        // Note: check in the derived-first order.
        //
        if      (t.is_a<name>      ()) h_array = false;
        else if (t.is_a<name_pair> ())
        {
          h_array = false;
          h_pair = true;
        }
        else if (t.is_a<process_path_ex> ())
        {
          // Decide on array dynamically.
          h_pair = true;
        }
        else if (t.is_a<process_path> ())
        {
          h_array = false;
          h_pair = true;
        }
        else if (t.is_a<cmdline>      () ||
                 t.is_a<vector<name>> ())
        {
          h_array = true;
        }
        else if (t.is_a<vector<pair<string, string>>>           () ||
                 t.is_a<vector<pair<string, optional<string>>>> () ||
                 t.is_a<vector<pair<string, optional<bool>>>>   () ||
                 t.is_a<map<string, string>>                    () ||
                 t.is_a<map<string, optional<string>>>          () ||
                 t.is_a<map<string, optional<bool>>>            () ||
                 t.is_a<map<project_name, dir_path>>            ())
        {
          h_array = true;
          h_pair = true;
        }
        else if (t.is_a<map<optional<string>, string>>          () ||
                 t.is_a<vector<pair<optional<string>, string>>> ())
        {
          h_array = true;
          h_pair = false;
        }

        goto fall_through;
      }

      return;

    fall_through:
      ;
    }

    names storage;
    names_view ns (reverse (v, storage, true /* reduce */));

    if (ns.empty ())
    {
      // When it comes to representing an empty value, our options are: empty
      // array ([]), empty object ({}), or an absent member. The latter feels
      // closer to null than empty, so that's out. After some experimentation,
      // it feels the best choice is to use array unless we know for sure it
      // is not, in which case we use an object if it's a pair and empty
      // string otherwise (the empty string makes sense because we serialize
      // complex names as target names; see below).
      //
      if (!h_array || *h_array)
      {
        j.begin_array ();
        j.end_array ();
      }
      else
      {
        if (h_pair)
        {
          j.begin_object ();
          j.end_object ();
        }
        else
          j.value ("");
      }
    }
    else
    {
      if (!h_array)
        h_array = ns.size () > 2 || (ns.size () == 2 && !ns.front ().pair);

      if (*h_array)
        j.begin_array ();

      // While it may be tempting to try to provide a heterogeneous array
      // (i.e., all strings, all objects, all pairs), in case of pairs we
      // actually don't know whether a non-pair element is first or second
      // (it's up to interpretation; though we do hint which one is optional
      // for typed values above). So we serialize each name in its most
      // appropriate form.
      //
      auto simple = [] (const name& n)
      {
        return n.simple () || n.directory () || n.file ();
      };

      auto s_simple = [&j] (const name& n)
      {
        if (n.simple ())
          j.value (n.value);
        else if (n.directory ())
          j.value (n.dir.string ());
        else if (n.file ())
        {
          // Note: both must be present due to earlier checks.
          //
          j.value ((n.dir / n.value).string ());
        }
        else
          return false;

        return true;
      };

      for (auto i (ns.begin ()), e (ns.end ()); i != e; )
      {
        const name& l (*i++);
        const name* r (l.pair ? &*i++ : nullptr);

        optional<bool> hp (h_pair);

        if (!hp && r != nullptr && simple (l) && simple (*r))
          hp = true;

        if (hp)
        {
          // Pair of simple names.
          //
          j.begin_object ();

          if (r != nullptr)
          {
            j.member_name ("first");  s_simple (l);
            j.member_name ("second"); s_simple (*r);
          }
          else
          {
            j.member_name (*hp ? "first" : "second"); s_simple (l);
          }

          j.end_object ();
        }
        else if (r == nullptr && s_simple (l))
          ;
        else
        {
          // If complex name (or pair thereof), then assume a target name.
          //
          dump_quoted_target_name (j,
                                   names_view (&l, r != nullptr ? 2 : 1),
                                   false /* relative */);
        }
      }

      if (*h_array)
        j.end_array ();
    }
  }
#endif

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

      // On one hand it might be helpful to print the visibility. On the
      // other, it is always specified which means there will be a lot of
      // noise. So probably not.
      //
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

#ifndef BUILD2_BOOTSTRAP
  static void
  dump_variable (json::stream_serializer& j,
                 const variable_map& vm,
                 const variable_map::const_iterator& vi,
                 const scope& s,
                 variable_kind k)
  {
    // Note: see the buildfile version above for comments.

    assert (k != variable_kind::tt_pat); // TODO

    const auto& p (*vi);
    const variable& var (p.first);
    const value& v (p.second);

    lookup l (v, var, vm);
    if (k != variable_kind::prerequisite)
    {
      if (var.override ())
        return; // Ignore.

      if (var.overrides != nullptr)
      {
        l = s.lookup_override (
          var,
          make_pair (l, 1),
          k == variable_kind::target || k == variable_kind::rule,
          k == variable_kind::rule).first;

        assert (l.defined ()); // We at least have the original.
      }
    }

    // Note that we do not distinguish between variable/value type.
    //
    // An empty value of a non-array type is represented as an empty object
    // ({}).
    //
#if 0
    struct variable
    {
      string           name;
      optional<string> type;
      json_value       value; // string|number|boolean|null|object|array
    };
#endif

    j.begin_object ();

    j.member ("name", var.name);

    if (l->type != nullptr)
      j.member ("type", l->type->name);

    j.member_name ("value");
    dump_value (j, *l);

    j.end_object ();
  }
#endif

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

#ifndef BUILD2_BOOTSTRAP
  static void
  dump_variables (json::stream_serializer& j,
                  const variable_map& vars,
                  const scope& s,
                  variable_kind k)
  {
    for (auto i (vars.begin ()), e (vars.end ()); i != e; ++i)
    {
      dump_variable (j, vars, i, s, k);
    }
  }
#endif

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

  // Dump ad hoc recipe.
  //
  static void
  dump_recipe (ostream& os, string& ind, const adhoc_rule& r, const scope& s)
  {
    auto& re (*s.root_scope ()->root_extra);

    os << ind << '%';

    r.dump_attributes (os);

    for (action a: r.actions)
      os << ' ' << re.meta_operations[a.meta_operation ()]->name <<
        '(' << re.operations[a.operation ()].info->name << ')';

    os << endl;
    r.dump_text (os, ind);
  }

  // Dump pattern rule.
  //
  static void
  dump_rule (ostream& os,
             string& ind,
             const adhoc_rule_pattern& rp,
             const scope& s)
  {
    // Pattern.
    //
    os << ind;

    // Avoid printing the derived name.
    //
    if (rp.rule_name.front () != '<' || rp.rule_name.back () != '>')
    {
      os << "[rule_name=" << rp.rule_name << "] ";
    }

    rp.dump (os);

    // Recipes.
    //
    for (const shared_ptr<adhoc_rule>& r: rp.rules)
    {
      os << endl;
      dump_recipe (os, ind, *r, s);
    }
  }

  // Similar to target::matched() but for the load phase.
  //
  static inline bool
  matched (const target& t, action a)
  {
    // Note: running serial and task_count is 0 before any operation has
    // started.
    //
    if (size_t c = t[a].task_count.load (memory_order_relaxed))
    {
      if (c == t.ctx.count_applied () || c == t.ctx.count_executed ())
        return true;
    }

    return false;
  }

  static void
  dump_target (ostream& os,
               string& ind,
               optional<action> a,
               const target& t,
               const scope& s,
               bool rel)
  {
    // If requested, print the target and its prerequisites relative to the
    // scope. To achieve this we are going to temporarily lower the stream
    // path verbosity to level 0.
    //
    // @@ Not if in src and out != src? Otherwise end up with ../../../...
    //    See JSON version for the state of the art.
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

    os << ind;

    // Target attributes.
    //
    if (!t.rule_hints.map.empty ())
    {
      os << '[';

      bool f (true);
      for (const rule_hints::value_type& v: t.rule_hints.map)
      {
        if (f)
          f = false;
        else
          os << ", ";

        if (v.type != nullptr)
          os << v.type->name << '@';

        os << "rule_hint=";

        if (v.operation != default_id)
          os << s.root_scope ()->root_extra->operations[v.operation].info->name
             << '@';

        os << v.hint;
      }

      os << "] ";
    }

    os << t << ':';

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
    const prerequisite_targets* pts (nullptr);
    {
      action inner; // @@ Only for the inner part of the action currently.

      if (matched (t, inner))
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
      for (const shared_ptr<adhoc_rule>& r: t.adhoc_recipes)
      {
        os << endl;
        dump_recipe (os, ind, *r, s);
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

#ifndef BUILD2_BOOTSTRAP
  static void
  dump_target (json::stream_serializer& j,
               optional<action> a,
               const target& t,
               const scope& s,
               bool rel,
               target_name_cache& tcache)
  {
    // Note: see the buildfile version above for comments.

    // Note that the target name (and display_name) are relative to the
    // containing scope (if any).
    //
#if 0
    struct prerequisite
    {
      string name; // Quoted/qualified name.
      string type;
      vector<variable> variables; // Prerequisite variables.
    };

    struct loaded_target
    {
      string           name; // Quoted/qualified name.
      string   display_name;
      string           type; // Target type.
      //string         declaration;
      optional<string> group; // Quoted/qualified group target name.

      vector<variable> variables; // Target variables.

      vector<prerequisite> prerequisites;
    };

    // @@ TODO: target attributes (rule_hint)

    struct prerequisite_target
    {
      string name; // Target name (always absolute).
      string type;
      bool adhoc;
    };

    struct operation_state
    {
      string rule; // null if direct recipe match

      optional<string> state; // unchanged|changed|group

      vector<variable> variables; // Rule variables.

      vector<prerequisite_target> prerequisite_targets;
    };

    struct matched_target
    {
      string           name;
      string   display_name;
      string           type;
      //string         declaration;
      optional<string> group;

      optional<path> path; // Absent if not path-based target, not assigned.

      vector<variable> variables;

      optional<operation_state> outer_operation; // null if not matched.
      operation_state           inner_operation; // null if not matched.
    };
#endif

    j.begin_object ();

    j.member_name ("name");
    dump_quoted_target_name (j, t, rel /* relative */);

    j.member_name ("display_name");
    dump_display_target_name (j, t, rel /* relative */);

    j.member ("type", t.type ().name);

    // @@ This value currently doesn't make much sense:
    //
    //    - why are all the system headers prereq-new?
    //
    //    - why is synthesized obje{} prereq-new?
    //
#if 0
    {
      const char* v (nullptr);
      switch (t.decl)
      {
      case target_decl::prereq_new:  v = "prerequisite-new";  break;
      case target_decl::prereq_file: v = "prerequisite-file"; break;
      case target_decl::implied:     v = "implied";           break;
      case target_decl::real:        v = "real";              break;
      }
      j.member ("declaration", v);
    }
#endif

    if (t.group != nullptr)
    {
      j.member_name ("group");
      dump_quoted_target_name (j, *t.group, tcache);
    }

    if (a)
    {
      const string* v (nullptr);

      if (t.is_a<dir> () || t.is_a<fsdir> ())
      {
        v = &t.dir.string ();
      }
      else if (const auto* pt = t.is_a<path_target> ())
      {
        const path& p (pt->path ());

        if (!p.empty ())
          v = &p.string ();
      }

      if (v != nullptr)
        j.member ("path", *v);
    }

    // Target variables.
    //
    if (!t.vars.empty ())
    {
      j.member_begin_array ("variables");
      dump_variables (j, t.vars, s, variable_kind::target);
      j.end_array ();
    }

    // Prerequisites.
    //
    if (!a)
    {
      const prerequisites& ps (t.prerequisites ());

      if (!ps.empty ())
      {
        j.member_begin_array ("prerequisites");

        for (const prerequisite& p: ps)
        {
          j.begin_object ();

          {
            // Cobble together an equivalent of dump_quoted_target_name().
            //
            prerequisite_key pk (p.key ());
            target_key& tk (pk.tk);

            // It's possible that the containing scope differs from
            // prerequisite's. This, for example, happens when we copy the
            // prerequisite for a synthesized obj{} dependency that happens to
            // be in a subdirectory, as in exe{foo}:src/cxx{foo}. In this
            // case, we need to rebase relative paths to the containing scope.
            //
            dir_path d, o;
            if (p.scope != s)
            {
              if (tk.out->empty ())
              {
                if (tk.dir->relative ())
                {
                  d = (p.scope.out_path () / *tk.dir).relative (s.out_path ());
                  tk.dir = &d;
                }
              }
              else
              {
                if (tk.dir->relative ())
                {
                  d = (p.scope.src_path () / *tk.dir).relative (s.src_path ());
                  tk.dir = &d;
                }

                if (tk.out->relative ())
                {
                  o = (p.scope.out_path () / *tk.out).relative (s.out_path ());
                  if (o.empty ())
                    o = dir_path (".");
                  tk.out = &o;
                }
              }
            }

            // If prerequisite paths are absolute, keep them absolute.
            //
            ostringstream os;
            stream_verb (os, stream_verbosity (1, 0));

            if (pk.proj)
              os << *pk.proj << '%';

            to_stream (os, pk.tk.as_name (), quote_mode::effective, '@');

            j.member ("name", os.str ());
          }

          j.member ("type", p.type.name);

          if (!p.vars.empty ())
          {
            j.member_begin_array ("variables");
            dump_variables (j, p.vars, s, variable_kind::prerequisite);
            j.end_array ();
          }

          j.end_object ();
        }

        j.end_array ();
      }
    }
    else
    {
      // Matched rules and their state (prerequisite_targets, vars, etc).
      //
      auto dump_opstate = [&tcache, &j, &s, &t] (action a)
      {
        const target::opstate& o (t[a]);

        j.begin_object ();

        j.member ("rule", o.rule != nullptr ? o.rule->first.c_str () : nullptr);

        // It feels natural to omit the unknown state, as if it corresponded
        // to absent in optional<target_state>.
        //
        if (o.state != target_state::unknown)
        {
          assert (o.state == target_state::unchanged ||
                  o.state == target_state::changed ||
                  o.state == target_state::group);

          j.member ("state", to_string (o.state));
        }

        if (!o.vars.empty ())
        {
          j.member_begin_array ("variables");
          dump_variables (j, o.vars, s, variable_kind::rule);
          j.end_array ();
        }

        {
          bool first (true);
          for (const prerequisite_target& pt: t.prerequisite_targets[a])
          {
            if (pt.target == nullptr)
              continue;

            if (first)
            {
              j.member_begin_array ("prerequisite_targets");
              first = false;
            }

            j.begin_object ();

            j.member_name ("name");
            dump_quoted_target_name (j, *pt.target, tcache);

            j.member ("type", pt.target->type ().name);

            if (pt.adhoc ())
              j.member ("adhoc", true);

            j.end_object ();
          }

          if (!first)
            j.end_array ();
        }

        j.end_object ();
      };

      if (a->outer ())
      {
        j.member_name ("outer_operation");
        if (matched (t, *a))
          dump_opstate (*a);
        else
          j.value (nullptr);
      }

      {
        action ia (a->inner_action ());

        j.member_name ("inner_operation");
        if (matched (t, ia))
          dump_opstate (ia);
        else
          j.value (nullptr);
      }
    }

    j.end_object ();
  }
#endif

  static void
  dump_scope (ostream& os,
              string& ind,
              optional<action> a,
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

    // Variable/rule/scope/target block.
    //
    bool vb (false), rb (false), sb (false), tb (false);

    // Target type/pattern-specific variables.
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

    // Pattern rules.
    //
    for (const unique_ptr<adhoc_rule_pattern>& rp: p.adhoc_rules)
    {
      if (vb || rb)
      {
        os << endl;
        vb = false;
      }

      os << endl; // Extra newline between rules.
      dump_rule (os, ind, *rp, p);
      rb = true;
    }

    // Nested scopes of which we are an immediate parent. Only consider the
    // out hierarchy.
    //
    // Note that because we use the logical (rather than physical) parent, we
    // will be printing the logical scope hierarchy (i.e., a project with
    // disabled amalgamation will be printed directly inside the global
    // scope).
    //
    for (auto e (p.ctx.scopes.end ()); i != e; )
    {
      if (i->second.front () == nullptr)
        ++i; // Skip over src paths.
      else if (i->second.front ()->parent_scope () != &p)
        break; // Moved past our parent.
      else
      {
        if (vb || rb || sb)
        {
          os << endl;
          vb = rb = false;
        }

        os << endl; // Extra newline between scope blocks.

        dump_scope (os, ind, a, i, true /* relative */);
        sb = true;
      }
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

      if (vb || rb || sb || tb)
      {
        os << endl;
        vb = rb = sb = false;
      }

      os << endl; // Extra newline between targets.
      dump_target (os, ind, a, t, p, true /* relative */);
      tb = true;
    }

    ind.resize (ind.size () - 2);
    relative_base = orb;

    os << endl
       << ind << '}';
  }

#ifndef BUILD2_BOOTSTRAP
  static void
  dump_scope (json::stream_serializer& j,
              optional<action> a,
              scope_map::const_iterator& i,
              bool rel,
              target_name_cache& tcache)
  {
    // Note: see the buildfile version above for additional comments.

    const scope& p (*i->second.front ());
    const dir_path& d (i->first);
    ++i;

#if 0
    struct scope
    {
      // The out_path member is relative to the parent scope. It is empty for
      // the special global scope. The src_path member is absent if the same
      // as out_path (in-source build or scope outside of project).
      //
      string           out_path;
      optional<string> src_path;

      vector<variable> variables; // Non-type/pattern scope variables.

      vector<scope> scopes; // Immediate children.

      vector<loaded_target|matched_target> targets;
    };
#endif

    j.begin_object ();

    if (d.empty ())
      j.member ("out_path", ""); // Global scope.
    else
    {
      const dir_path& rd (rel ? relative (d) : d);
      j.member ("out_path", rd.empty () ? string (".") : rd.string ());

      if (!p.out_eq_src ())
        j.member ("src_path", p.src_path ().string ());
    }

    const dir_path* orb (relative_base);
    relative_base = &d;

    // Scope variables.
    //
    if (!p.vars.empty ())
    {
      j.member_begin_array ("variables");
      dump_variables (j, p.vars, p, variable_kind::scope);
      j.end_array ();
    }

    // Nested scopes of which we are an immediate parent.
    //
    {
      bool first (true);
      for (auto e (p.ctx.scopes.end ()); i != e; )
      {
        if (i->second.front () == nullptr)
          ++i;
        else if (i->second.front ()->parent_scope () != &p)
          break;
        else
        {
          if (first)
          {
            j.member_begin_array ("scopes");
            first = false;
          }

          dump_scope (j, a, i, true /* relative */, tcache);
        }
      }

      if (!first)
        j.end_array ();
    }

    // Targets.
    //
    {
      bool first (true);
      for (const auto& pt: p.ctx.targets)
      {
        const target& t (*pt);

        if (&p != &t.base_scope ()) // @@ PERF
          continue;

        // Skip targets that haven't been matched for this action.
        //
        if (a)
        {
          if (!(matched (t, a->inner_action ()) ||
                (a->outer () && matched (t, *a))))
            continue;
        }

        if (first)
        {
          j.member_begin_array ("targets");
          first = false;
        }

        dump_target (j, a, t, p, true /* relative */, tcache);
      }

      if (!first)
        j.end_array ();
    }

    relative_base = orb;
    j.end_object ();
  }
#endif

  void
  dump (const context& c, optional<action> a, dump_format fmt)
  {
    auto i (c.scopes.begin ());
    assert (i->second.front () == &c.global_scope);

    switch (fmt)
    {
    case dump_format::buildfile:
      {
        // We don't lock diag_stream here as dump() is supposed to be called
        // from the main thread prior/after to any other threads being
        // spawned.
        //
        string ind;
        ostream& os (*diag_stream);
        dump_scope (os, ind, a, i, false /* relative */);
        os << endl;
        break;
      }
    case dump_format::json:
      {
#ifndef BUILD2_BOOTSTRAP
        target_name_cache tc;
        json::stream_serializer j (cout, 0 /* indent */);
        dump_scope (j, a, i, false /* relative */, tc);
        cout << endl;
#else
        assert (false);
#endif
        break;
      }
    }
  }

  void
  dump (const scope* s, optional<action> a, dump_format fmt, const char* cind)
  {
    scope_map::const_iterator i;
    if (s != nullptr)
    {
      const scope_map& m (s->ctx.scopes);
      i = m.find_exact (s->out_path ());
      assert (i != m.end () && i->second.front () == s);
    }

    switch (fmt)
    {
    case dump_format::buildfile:
      {
        string ind (cind);
        ostream& os (*diag_stream);

        if (s != nullptr)
          dump_scope (os, ind, a, i, false /* relative */);
        else
          os << ind << "<no known scope to dump>";

        os << endl;
        break;
      }
    case dump_format::json:
      {
#ifndef BUILD2_BOOTSTRAP
        target_name_cache tc;
        json::stream_serializer j (cout, 0 /* indent */);

        if (s != nullptr)
          dump_scope (j, a, i, false /* relative */, tc);
        else
          j.value (nullptr);

        cout << endl;
#else
        assert (false);
#endif
        break;
      }
    }
  }

  void
  dump (const target* t, optional<action> a, dump_format fmt, const char* cind)
  {
    const scope* bs (t != nullptr ? &t->base_scope () : nullptr);

    switch (fmt)
    {
    case dump_format::buildfile:
      {
        string ind (cind);
        ostream& os (*diag_stream);

        if (t != nullptr)
          dump_target (os, ind, a, *t, *bs, false /* relative */);
        else
          os << ind << "<no known target to dump>";

        os << endl;
        break;
      }
    case dump_format::json:
      {
#ifndef BUILD2_BOOTSTRAP
        target_name_cache tc;
        json::stream_serializer j (cout, 0 /* indent */);

        if (t != nullptr)
          dump_target (j, a, *t, *bs, false /* relative */, tc);
        else
          j.value (nullptr);

        cout << endl;
#else
        assert (false);
#endif
        break;
      }
    }
  }
}
