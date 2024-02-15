// file      : libbuild2/adhoc-rule-regex-pattern.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/adhoc-rule-regex-pattern.hxx>

#include <libbutl/regex.hxx>

#include <libbuild2/algorithm.hxx>

namespace build2
{
  using pattern_type = name::pattern_type;

  adhoc_rule_regex_pattern::
  adhoc_rule_regex_pattern (
    const scope& s, string rn, const target_type& tt,
    name&& n, const location& nloc,
    names&& ans, const location& aloc,
    names&& pns, const location& ploc)
      : adhoc_rule_pattern (s, move (rn), tt)
  {
    // Semantically, our rule pattern is one logical regular expression that
    // spans multiple targets and prerequisites with a single back reference
    // (\N) space.
    //
    // To implement this we are going to concatenate all the target and
    // prerequisite sub-patterns separated with a character which cannot
    // appear in the name (nor is a special regex character) but which is
    // printable (for diagnostics). The directory separator (`/`) feels like a
    // natural choice. We will call such a concatenated string of names a
    // "name signature" (we also have a "type signature"; see below) and its
    // pattern a "name signature pattern".
    //
    regex::flag_type flags (regex::ECMAScript);

    // Append the sub-pattern to text_ returning the status of the `e` flag.
    //
    auto append_pattern = [this, &flags, first = true] (
      const string& t,
      const location& loc) mutable -> bool
    {
      size_t n (t.size ()), p (t.rfind (t[0]));

      // Process flags.
      //
      bool fi (false), fe (false);
      for (size_t i (p + 1); i != n; ++i)
      {
        switch (t[i])
        {
        case 'i': fi = true; break;
        case 'e': fe = true; break;
        }
      }

      // For icase we require all or none of the patterns to have it.
      //
      if (first)
      {
        if (fi)
          flags |= regex::icase;
      }
      else if (((flags & regex::icase) != 0) != fi)
        fail (loc) << "inconsistent regex 'i' flag in '" << t << "'";

      if (!first)
        text_ += '/';
      else
        first = false;

      text_.append (t.c_str () + 1, p - 1);

      return fe;
    };

    // Append an element either to targets_ or prereqs_.
    //
    auto append_element = [&s, &append_pattern] (
      vector<element>& v,
      name&& n,
      const location& loc,
      const target_type* tt = nullptr)
    {
      if (tt == nullptr)
      {
        tt = n.untyped () ? &file::static_type : s.find_target_type (n.type);

        if (tt == nullptr)
          fail (loc) << "unknown target type " << n.type <<
            info << "perhaps the module that defines this target type is "
                 << "not loaded by project " << *s.root_scope ();
      }

      bool e (n.pattern                                 &&
              *n.pattern == pattern_type::regex_pattern &&
              append_pattern (n.value, loc));

      v.push_back (element {move (n), *tt, e});
    };

    // This one is always a pattern.
    //
    append_element (targets_, move (n), nloc, &tt);

    // These are all patterns or substitutions.
    //
    for (name& an: ans)
      append_element (targets_, move (an), aloc);

    // These can be patterns, substitutions, or non-patterns.
    //
    for (name& pn: pns)
      append_element (prereqs_, move (pn), ploc);

    try
    {
      regex_ = regex (text_, flags);
    }
    catch (const regex_error& e)
    {
      // Print regex_error description if meaningful (no space).
      //
      // This may not necessarily be pointing at the actual location of the
      // error but it should be close enough.
      //
      fail (nloc) << "invalid regex pattern '" << text_ << "'" << e;
    }
  }

  bool adhoc_rule_regex_pattern::
  match (action a, const target& t, const string&, match_extra& me) const
  {
    tracer trace ("adhoc_rule_regex_pattern::match");

    // Note: target may not be locked in which case we should not modify
    //       target or match_extra (see adhoc_rule::match() for background).

    // The plan is as follows: First check the "type signature" of the target
    // and its prerequisites (the primary target type has already been matched
    // by the rule matching machinery). If there is a match, then concatenate
    // their names into a "name signature" in the same way as for sub-patterns
    // above and match that against the name signature regex pattern. If there
    // is a match then this rule matches and the apply_*() functions should be
    // called to process any member/prerequisite substitutions and inject them
    // along with non-pattern prerequisites.
    //
    // It would be natural to perform the type match and concatenation of the
    // names simultaneously. However, while the former should be quite cheap,
    // the latter will most likely require dynamic allocation. To mitigate
    // this we are going to pre-type-match the first prerequisite before
    // concatenating any names. This should weed out most of the non-matches
    // for sane patterns.
    //
    // Note also that we don't backtrack and try different combinations of the
    // type-matching targets/prerequisites. We also ignore prerequisites
    // marked ad hoc for type-matching.
    //
    auto pattern = [] (const element& e) -> bool
    {
      return e.name.pattern && *e.name.pattern == pattern_type::regex_pattern;
    };

    auto find_prereq = [a, &t] (const target_type& tt) -> optional<target_key>
    {
      // We use the standard logic that one would use in the rule::match()
      // implementation. Except we support the unmatch and match values in
      // the update variable.
      //
      // Note: assuming group prerequisites are immutable (not locked).
      //
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        // Note that here we don't validate the update operation override
        // value (since we may not match). Instead the rule does this in
        // apply().
        //
        // Note: assuming include()'s use of target only relied on immutable
        // data (not locked).
        //
        lookup l;
        if (include (a, t, p, a.operation () == update_id ? &l : nullptr) ==
              include_type::normal && p.is_a (tt))
          return p.key ().tk;
      }
      return nullopt;
    };

    // Pre-type-match the first prerequisite, if any.
    //
    auto pe (prereqs_.end ()), pi (find_if (prereqs_.begin (), pe, pattern));

    optional<target_key> pk1;
    if (pi != pe)
    {
      if (!(pk1 = find_prereq (pi->type)))
      {
        l4 ([&]{trace << rule_name << ": no " << pi->type.name
                      << "{} prerequisite for target " << t;});
        return false;
      }
    }

    // Ok, this is a potential match, start concatenating the names.
    //
    // Note that the regex_match_results object (which we will be passing
    // through to apply() in the target's auxiliary data storage) contains
    // iterators pointing to the string being matched. Which means this string
    // must be kept around until we are done with replacing the subsitutions.
    // In fact, we cannot even move it because this may invalidate the
    // iterators (e.g., in case of a small string optimization). We also
    // cannot set the data ahead of time because we may not match. Plus,
    // resorting to a dynamic memory allocation even if we don't match feels
    // heavy-handed.
    //
    // So the plan is to store the string in match_extra::data() and
    // regex_match_results (which we can move) in the auxiliary data storage.
    //
    // Note: only cache if locked.
    //
    static_assert (sizeof (string) <= match_extra::data_size,
                   "match data too large");

    string tmp;
    string& ns (me.locked ? me.data (string ()) : tmp);

    auto append_name = [&ns,
                        first = true,
                        storage = string ()] (const target_key& tk,
                                              const element& e) mutable
    {
      if (!first)
        ns += '/';
      else
        first = false;

      ns += tk.effective_name (storage, e.match_ext);
    };

    // Primary target (always a pattern).
    //
    auto te (targets_.end ()), ti (targets_.begin ());
    append_name (t.key (), *ti); // Immutable (not locked).

    // Match ad hoc group members.
    //
    // Note: shouldn't be in effect for an explicit group (not locked).
    //
    while ((ti = find_if (ti + 1, te, pattern)) != te)
    {
      const target* at (find_adhoc_member (t, ti->type));

      if (at == nullptr)
      {
        l4 ([&]{trace << rule_name << ": no " << ti->type.name
                      << "{} ad hoc target group member for target " << t;});
        return false;
      }

      append_name (at->key (), *ti);
    }

    // Finish prerequisites.
    //
    if (pi != pe)
    {
      append_name (*pk1, *pi);

      while ((pi = find_if (pi + 1, pe, pattern)) != pe)
      {
        optional<target_key> pk (find_prereq (pi->type));

        if (!pk)
        {
          l4 ([&]{trace << rule_name << ": no " << pi->type.name
                        << "{} prerequisite for target " << t;});
          return false;
        }

        append_name (*pk, *pi);
      }
    }

    // While it can be tempting to optimize this for patterns that don't have
    // any substitutions (which would be most of them), keep in mind that we
    // will also need match_results for $N variables in the recipe (or a C++
    // rule implementation may want to access the match_results object).
    //
    regex_match_results mr;
    if (!regex_match (ns, mr, regex_))
    {
      l4 ([&]{trace << rule_name << ": name signature '" << ns
                    << "' does not match regex '" << text_
                    << "' for target " << t;});
      return false;
    }

    if (me.locked)
      t.data (a, move (mr));

    return true;
  }

  static inline string
  substitute (const target& t,
              const regex_match_results& mr,
              const string& s,
              const char* what)
  {
    string r (butl::regex_replace_match_results (
                mr, s.c_str () + 1, s.rfind (s[0]) - 1));

    // @@ Note that while it would have been nice to print the location here,
    //    (and also pass to search()->find_target_type()), we would need to
    //    save location_value in each element to cover multiple declarations.
    //
    if (r.empty ())
      fail << what << " substitution '" << s << "' for target " << t
           << " results in empty name";

    return r;
  }

  void adhoc_rule_regex_pattern::
  apply_group_members (action a, target& t, const scope& bs,
                       match_extra&) const
  {
    if (targets_.size () == 1) // The group/primary target is always present.
      return;

    group* g (t.is_a<group> ());

    const auto& mr (t.data<regex_match_results> (a));

    for (auto i (targets_.begin () + 1); i != targets_.end (); ++i)
    {
      // These are all patterns or substitutions.
      //
      const element& e (*i);

      if (*e.name.pattern == pattern_type::regex_pattern)
        continue;

      // Similar to prerequisites below, we treat member substitutions
      // relative to the target.
      //
      dir_path d;
      if (e.name.dir.empty ())
        d = t.dir; // Absolute and normalized.
      else
      {
        if (e.name.dir.absolute ())
          d = e.name.dir;
        else
          d = t.dir / e.name.dir;

        d.normalize ();
      }

      string n (substitute (
                  t,
                  mr,
                  e.name.value,
                  (g != nullptr
                   ? "explicit target group member"
                   : "ad hoc target group member")));

      // @@ TODO: save location in constructor?
      //
      location loc;

      optional<string> ext (target::split_name (n, loc));

      if (g != nullptr)
      {
        auto& ms (g->members);

        // These are conceptually static but they behave more like dynamic in
        // that we likely need to insert the target, set its group, etc. In a
        // sense, they are rule-static, but group-dynamic.
        //
        // Note: a custom version of the dyndep_rule::inject_group_member()
        // logic.
        //
        auto l (search_new_locked (
                  bs.ctx,
                  e.type,
                  move (d),
                  dir_path (), // Always in out.
                  move (n),
                  ext ? &*ext : nullptr,
                  &bs));

        const target& t (l.first); // Note: non-const only if have lock.

        if (l.second)
        {
          l.first.group = g;
          l.second.unlock ();
        }
        else
        {
          if (find (ms.begin (), ms.end (), &t) != ms.end ())
            continue;

          if (t.group != g) // Note: atomic.
          {
            // We can only update the group under lock.
            //
            target_lock tl (lock (a, t));

            if (!tl)
              fail << "group " << *g << " member " << t << " is already matched" <<
                info << "static group members specified by pattern rules cannot "
                     << "be used as prerequisites directly, only via group";

            if (t.group == nullptr)
              tl.target->group = g;
            else if (t.group != g)
            {
              fail << "group " << *g << " member " << t
                   << " is already member of group " << *t.group;
            }
          }
        }

        ms.push_back (&t);
      }
      else
      {
        add_adhoc_member_identity (
          t,
          e.type,
          move (d),
          dir_path (), // Always in out.
          move (n),
          move (ext),
          loc);
      }
    }
  }

  void adhoc_rule_regex_pattern::
  apply_prerequisites (action a, target& t,
                       const scope& bs,
                       match_extra&) const
  {
    const auto& mr (t.data<regex_match_results> (a));

    // Re-create the same clean semantics as in match_prerequisite_members().
    //
    bool clean (a.operation () == clean_id && !t.is_a<alias> ());

    auto& pts (t.prerequisite_targets[a]);

    // Avoid duplicating fsdir{} that may have already been injected by
    // inject_fsdir() (in which case it is expected to be first).
    //
    const target* dir (nullptr);
    if (!pts.empty ())
    {
      const prerequisite_target& pt (pts.front ());

      if (pt.target != nullptr && pt.adhoc () && pt.target->is_a<fsdir> ())
        dir = pt.target;
    }

    for (const element& e: prereqs_)
    {
      // While it would be nice to avoid copying here, the semantics of
      // search() (and find_target_type() that it calls) is just too hairy to
      // duplicate and try to optimize. It feels like most of the cases will
      // either fall under the small string optimization or be absolute target
      // names (e.g., imported tools).
      //
      // @@ Perhaps we should try to optimize the absolute target name case?
      //
      // Which scope should we use to resolve this prerequisite? After some
      // meditation it feels natural to use the target's scope for patterns
      // and the rule's scope for non-patterns.
      //
      name n;
      const scope* s;
      if (e.name.pattern)
      {
        if (*e.name.pattern == pattern_type::regex_pattern)
          continue;

        // Note: cannot be project-qualified.
        //
        n = name (e.name.dir,
                  e.name.type,
                  substitute (t, mr, e.name.value, "prerequisite"));
        s = &bs;
      }
      else
      {
        n = e.name;
        s = &rule_scope;
      }

      const target& pt (search (t, move (n), *s, &e.type));

      if (&pt == dir || (clean && !pt.in (*bs.root_scope ())))
        continue;

      // @@ TODO: it could be handy to mark a prerequisite (e.g., a tool)
      //    ad hoc so that it doesn't interfere with the $< list. Also
      //    clean=false. Also update=match|unmatch.
      //
      pts.push_back (prerequisite_target (&pt, false /* adhoc */));
    }
  }

  void adhoc_rule_regex_pattern::
  dump (ostream& os) const
  {
    // Targets.
    //
    size_t tn (targets_.size ());

    if (tn != 1)
      os << '<';

    for (size_t i (0); i != tn; ++i)
      os << (i != 0 ? " " : "") << targets_[i].name;

    if (tn != 1)
      os << '>';

    // Prerequisites.
    //
    os << ':';

    for (size_t i (0); i != prereqs_.size (); ++i)
      os << ' ' << prereqs_[i].name;
  }
}
