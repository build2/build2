// file      : libbuild2/scope.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/scope.hxx>

#include <libbuild2/rule.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>

using namespace std;

namespace build2
{
  ostream&
  operator<< (ostream& os, const subprojects& sps)
  {
    for (auto b (sps.begin ()), i (b); os && i != sps.end (); ++i)
    {
      // See find_subprojects() for details.
      //
      const project_name& n (
        path::traits_type::is_separator (i->first.string ().back ())
        ? empty_project_name
        : i->first);

      os << (i != b ? " " : "") << n << '@' << i->second.string ();
    }

    return os;
  }

  // scope
  //
  scope::
  scope (context& c, bool shared)
      : ctx (c), vars (*this, shared), target_vars (c, shared)
  {
  }

  scope::
  ~scope ()
  {
    // Definition of adhoc_rule_pattern.
  }

  pair<lookup, size_t> scope::
  lookup_original (const variable& var,
                   const target_key* tk,
                   const target_key* g1k,
                   const target_key* g2k,
                   size_t start_d) const
  {
    assert (tk != nullptr || var.visibility != variable_visibility::target);
    assert (g2k == nullptr || g1k != nullptr);

    size_t d (0);

    if (var.visibility == variable_visibility::prereq)
      return make_pair (lookup_type (), d);

    // Process target type/pattern-specific prepend/append values.
    //
    auto pre_app = [&var, this] (lookup_type& l,
                                 const scope* s,
                                 const target_key* tk,
                                 const target_key* g1k,
                                 const target_key* g2k,
                                 string n)
    {
      const value& v (*l);
      assert ((v.extra == 1 || v.extra == 2) && v.type == nullptr);

      // First we need to look for the stem value starting from the "next
      // lookup point". That is, if we have the group, then from the
      // s->target_vars (for the group), otherwise from s->vars, and then
      // continuing looking in the outer scopes (for both target and group).
      // Note that this may have to be repeated recursively, i.e., we may have
      // prepents/appends in outer scopes. Also, if the value is for the
      // group, then we shouldn't be looking for stem in the target's
      // variables. In other words, once we "jump" to group, we stay there.
      //
      lookup_type stem (s->lookup_original (var, tk, g1k, g2k, 2).first);

      // Check the cache.
      //
      pair<value&, ulock> entry (
        s->target_vars.cache.insert (
          ctx,
          make_tuple (&v, tk->type, !n.empty () ? move (n) : *tk->name),
          stem,
          static_cast<const variable_map::value_data&> (v).version,
          var));

      value& cv (entry.first);

      // If cache miss/invalidation, update the value.
      //
      if (entry.second.owns_lock ())
      {
        // Un-typify the cache. This can be necessary, for example, if we are
        // changing from one value-typed stem to another.
        //
        // Note: very similar logic as in the override cache population code
        // below.
        //
        if (!stem.defined () || cv.type != stem->type)
        {
          cv = nullptr;
          cv.type = nullptr; // Un-typify.
        }

        // Copy the stem.
        //
        if (stem.defined ())
          cv = *stem;

        // Typify the cache value in case there is no stem (we still want to
        // prepend/append things in type-aware way).
        //
        if (cv.type == nullptr && var.type != nullptr)
          typify (cv, *var.type, &var);

        // Now prepend/append the value, unless it is NULL.
        //
        if (v)
        {
          if (v.extra == 1)
            cv.prepend (names (cast<names> (v)), &var);
          else
            cv.append (names (cast<names> (v)), &var);
        }
      }

      // Return cache as the resulting value but retain l.var/vars, so it
      // looks as if the value came from s->target_vars.
      //
      l.value = &cv;
    };

    // Most of the time we match against the target name directly but
    // sometimes we may need to match against the directory leaf (dir{} or
    // fsdir{}) or incorporate the extension. We therefore try hard to avoid
    // the copy.
    //
    optional<string> tn;
    optional<string> g1n;
    optional<string> g2n;

    for (const scope* s (this); s != nullptr; )
    {
      if (tk != nullptr) // This started from the target.
      {
        bool f (!s->target_vars.empty ());

        // Target.
        //
        if (++d >= start_d)
        {
          if (f)
          {
            lookup_type l (s->target_vars.find (*tk, var, tn));

            if (l.defined ())
            {
              if (l->extra != 0) // Prepend/append?
                pre_app (l, s, tk, g1k, g2k, move (*tn));

              return make_pair (move (l), d);
            }
          }
        }

        // Group.
        //
        if (++d >= start_d)
        {
          if (f && g1k != nullptr)
          {
            lookup_type l (s->target_vars.find (*g1k, var, g1n));

            if (l.defined ())
            {
              if (l->extra != 0) // Prepend/append?
                pre_app (l, s, g1k, g2k, nullptr, move (*g1n));

              return make_pair (move (l), d);
            }

            if (g2k != nullptr)
            {
              l = s->target_vars.find (*g2k, var, g2n);

              if (l.defined ())
              {
                if (l->extra != 0) // Prepend/append?
                  pre_app (l, s, g2k, nullptr, nullptr, move (*g2n));

                return make_pair (move (l), d);
              }
            }
          }
        }
      }

      // Note that we still increment the lookup depth so that we can compare
      // depths of variables with different visibilities.
      //
      if (++d >= start_d && var.visibility != variable_visibility::target)
      {
        auto p (s->vars.lookup (var));
        if (p.first != nullptr)
          return make_pair (lookup_type (*p.first, p.second, s->vars), d);
      }

      switch (var.visibility)
      {
      case variable_visibility::scope:
        s = nullptr;
        break;
      case variable_visibility::target:
      case variable_visibility::project:
        s = s->root () ? nullptr : s->parent_scope ();
        break;
      case variable_visibility::global:
        s = s->parent_scope ();
        break;
      case variable_visibility::prereq:
        assert (false);
      }
    }

    return make_pair (lookup_type (), size_t (~0));
  }

  auto scope::
  lookup_override_info (const variable& var,
                        const pair<lookup_type, size_t> original,
                        bool target,
                        bool rule) const -> override_info
  {
    assert (!rule || target); // Rule-specific is target-specific.

    // Normally there would be no overrides and if there are, there will only
    // be a few of them. As a result, here we concentrate on keeping the logic
    // as straightforward as possible without trying to optimize anything.
    //
    // Note also that we rely (e.g., in the config module) on the fact that if
    // no overrides apply, then we return the original value and not its copy
    // in the cache (this is used to detect if the value was overriden).
    //
    assert (var.overrides != nullptr);

    const lookup_type& orig (original.first);
    size_t orig_depth (original.second);

    // The first step is to find out where our cache will reside. After some
    // meditation you will see it should be next to the innermost (scope-wise)
    // value of this variable (override or original).
    //
    // We also keep track of the root scope of the project from which this
    // innermost value comes. This is used to decide whether a non-recursive
    // project-wise override applies. And also where our variable cache is.
    //
    const variable_map* inner_vars (nullptr);
    const scope* inner_proj (nullptr);

    // One special case is if the original is target/rule-specific, which is
    // the most innermost. Or is it innermostest?
    //
    bool targetspec (false);
    if (target)
    {
      targetspec = orig.defined () && (orig_depth == 1 ||
                                       orig_depth == 2 ||
                                       (rule && orig_depth == 3));
      if (targetspec)
      {
        inner_vars = orig.vars;
        inner_proj = root_scope ();
      }
    }

    const scope* s;

    // Return true if the override applies to a value from vars/proj. Note
    // that it expects vars and proj to be not NULL; if there is nothing "more
    // inner", then any override will still be "visible".
    //
    auto applies = [&s] (const variable* o,
                         const variable_map* vars,
                         const scope* proj) -> bool
    {
      switch (o->visibility)
      {
      case variable_visibility::scope:
      {
        // Does not apply if in a different scope.
        //
        if (vars != &s->vars)
          return false;

        break;
      }
      case variable_visibility::project:
      {
        // Does not apply if in a subproject.
        //
        // Note that before we used to require the same project but that
        // missed values that are "visible" from the outer projects.
        //
        // If root scope is NULL, then we are looking at the global scope.
        //
        const scope* rs (s->root_scope ());
        if (rs != nullptr && rs->sub_root (*proj))
          return false;

        break;
      }
      case variable_visibility::global:
        break;
      case variable_visibility::target:
      case variable_visibility::prereq:
        assert (false);
      }

      return true;
    };

    // Return the override value if present in scope s and (optionally) of
    // the specified kind (__override, __prefix, etc).
    //
    auto lookup = [&s, &var] (const variable* o,
                              const char* k = nullptr) -> lookup_type
    {
      if (k != nullptr && !o->override (k))
        return lookup_type ();

      // Note: using the original as storage variable.
      // Note: have to suppress aliases since used for something else.
      //
      return lookup_type (
        s->vars.lookup (*o, true /* typed */, false /* aliased */).first,
        &var,
        &s->vars);
    };

    // Return true if a value is from this scope (either target type/pattern-
    // specific or ordinary).
    //
    auto belongs = [&s, target] (const lookup_type& l) -> bool
    {
      if (target)
      {
        for (auto& p1: s->target_vars)
          for (auto& p2: p1.second)
            if (l.vars == &p2.second)
              return true;
      }

      return l.vars == &s->vars;
    };

    // While looking for the cache we also detect if none of the overrides
    // apply. In this case the result is simply the original value (if any).
    //
    bool apply (false);

    for (s = this; s != nullptr; s = s->parent_scope ())
    {
      // If we are still looking for the cache, see if the original comes from
      // this scope. We check this before the overrides since it can come from
      // the target type/patter-specific variables, which is "more inner" than
      // normal scope variables (see lookup_original()).
      //
      if (inner_vars == nullptr && orig.defined () && belongs (orig))
      {
        inner_vars = orig.vars;
        inner_proj = s->root_scope ();
      }

      for (const variable* o (var.overrides.get ());
           o != nullptr;
           o = o->overrides.get ())
      {
        if (inner_vars != nullptr && !applies (o, inner_vars, inner_proj))
          continue;

        auto l (lookup (o));

        if (l.defined ())
        {
          if (inner_vars == nullptr)
          {
            inner_vars = l.vars;
            inner_proj = s->root_scope ();
          }

          apply = true;
          break;
        }
      }

      // We can stop if we found the cache and at least one override applies.
      //
      if (inner_vars != nullptr && apply)
        break;
    }

    if (!apply)
      return override_info {original, orig.defined ()};

    assert (inner_vars != nullptr);

    // If for some reason we are not in a project, use the cache from the
    // global scope.
    //
    if (inner_proj == nullptr)
      inner_proj = &ctx.global_scope;

    // Now find our "stem", that is, the value to which we will be appending
    // suffixes and prepending prefixes. This is either the original or the
    // __override, provided it applies. We may also not have either.
    //
    lookup_type stem;
    size_t stem_depth (0);
    const scope* stem_proj (nullptr);
    const variable* stem_ovr (nullptr); // __override if found and applies.

    // Again the special case of a target/rule-specific variable.
    //
    if (targetspec)
    {
      stem = orig;
      stem_depth = orig_depth;
      stem_proj = root_scope ();
    }

    // Depth at which we found the override (with implied target/rule-specific
    // lookup counts).
    //
    size_t ovr_depth (target ? (rule ? 3 : 2) : 0);

    for (s = this; s != nullptr; s = s->parent_scope ())
    {
      bool done (false);

      // First check if the original is from this scope.
      //
      if (orig.defined () && belongs (orig))
      {
        stem = orig;
        stem_depth = orig_depth;
        stem_proj = s->root_scope ();
        // Keep searching.
      }

      ++ovr_depth;

      // Then look for an __override that applies.
      //
      // Note that the override list is in the reverse order of appearance and
      // so we will naturally see the most recent override first.
      //
      for (const variable* o (var.overrides.get ());
           o != nullptr;
           o = o->overrides.get ())
      {
        // If we haven't yet found anything, then any override will still be
        // "visible" even if it doesn't apply.
        //
        if (stem.defined () && !applies (o, stem.vars, stem_proj))
          continue;

        auto l (lookup (o, "__override"));

        if (l.defined ())
        {
          stem = move (l);
          stem_depth = ovr_depth;
          stem_proj = s->root_scope ();
          stem_ovr = o;
          done = true;
          break;
        }
      }

      if (done)
        break;
    }

    // Check the cache.
    //
    variable_override_cache& cache (
      inner_proj == &ctx.global_scope
      ? ctx.global_override_cache
      : inner_proj->root_extra->override_cache);

    pair<value&, ulock> entry (
      cache.insert (
        ctx,
        make_pair (&var, inner_vars),
        stem,
        0, // Overrides are immutable.
        var));

    value& cv (entry.first);
    bool cl (entry.second.owns_lock ());

    // If cache miss/invalidation, update the value.
    //
    if (cl)
    {
      // Note: very similar logic as in the target type/pattern specific cache
      // population code above.
      //

      // Un-typify the cache. This can be necessary, for example, if we are
      // changing from one value-typed stem to another.
      //
      if (!stem.defined () || cv.type != stem->type)
      {
        cv = nullptr;
        cv.type = nullptr; // Un-typify.
      }

      if (stem.defined ())
        cv = *stem;

      // Typify the cache value. If the stem is the original, then the type
      // would get propagated automatically. But the stem could also be the
      // override, which is kept untyped. Or the stem might not be there at
      // all while we still need to apply prefixes/suffixes in the type-aware
      // way.
      //
      if (cv.type == nullptr && var.type != nullptr)
        typify (cv, *var.type, &var);
    }

    // Now apply override prefixes and suffixes (if updating the cache). Also
    // calculate the vars and depth of the result, which will be those of the
    // stem or prefix/suffix that applies, whichever is the innermost.
    //
    // Note: we could probably cache this information instead of recalculating
    // it every time.
    //
    size_t depth (stem_depth);
    const variable_map* vars (stem.vars);
    const scope* proj (stem_proj);

    ovr_depth = target ? (rule ? 3 : 2) : 0;

    for (s = this; s != nullptr; s = s->parent_scope ())
    {
      ++ovr_depth;

      // The override list is in the reverse order of appearance so we need to
      // iterate backwards in order to apply things in the correct order.
      //
      // We also need to skip any append/prepend overrides that appear before
      // __override (in the command line order), provided it is from this
      // scope.
      //
      bool skip (stem_ovr != nullptr && stem_depth == ovr_depth);

      for (const variable* o (var.overrides->aliases); // Last override.
           o != nullptr;
           o = (o->aliases != var.overrides->aliases ? o->aliases : nullptr))
      {
        if (skip)
        {
          if (stem_ovr == o) // Keep skipping until after we see __override.
            skip = false;

          continue;
        }

        // First see if this override applies. This is tricky: what if the
        // stem is a "visible" override from an outer project?  Shouldn't its
        // overrides apply? Sure sounds logical. So we use the project of the
        // stem's scope.
        //
        if (vars != nullptr && !applies (o, vars, proj))
          continue;

        // Note that we keep override values as untyped names even if the
        // variable itself is typed. We also pass the original variable for
        // diagnostics.
        //
        auto lp (lookup (o, "__prefix"));
        auto ls (lookup (o, "__suffix"));

        if (cl)
        {
          // Note: if we have both, then one is already in the stem.
          //
          if (lp) // No sense to prepend/append if NULL.
          {
            cv.prepend (names (cast<names> (lp)), &var);
          }
          else if (ls)
          {
            cv.append (names (cast<names> (ls)), &var);
          }
        }

        if (lp.defined () || ls.defined ())
        {
          // If we had no stem, use the first override as a surrogate stem.
          //
          if (vars == nullptr)
          {
            depth = ovr_depth;
            vars = &s->vars;
            proj = s->root_scope ();
          }
          // Otherwise, pick the innermost location between the stem and
          // prefix/suffix.
          //
          else if (ovr_depth < depth)
          {
            depth = ovr_depth;
            vars = &s->vars;
          }
        }
      }
    }

    // Use the location of the innermost value that contributed as the
    // location of the result.
    //
    return override_info {
      make_pair (lookup_type (&cv, &var, vars), depth),
      orig.defined () && stem == orig};
  }

  value& scope::
  append (const variable& var)
  {
    // Note that here we want the original value without any overrides
    // applied.
    //
    auto l (lookup_original (var).first);

    if (l.defined () && l.belongs (*this)) // Existing var in this scope.
      return vars.modify (l); // Ok since this is original.

    value& r (assign (var)); // NULL.

    if (l.defined ())
      r = *l; // Copy value (and type) from the outer scope.

    return r;
  }

  const target_type* scope::
  find_target_type (const string& tt) const
  {
    // Search the project's root scope then the global scope.
    //
    if (const scope* rs = root_scope ())
    {
      if (const target_type* r = rs->root_extra->target_types.find (tt))
        return r;
    }

    return ctx.global_target_types.find (tt);
  }

  // Find target type from file name.
  //
  static const target_type*
  find_target_type_file (const scope& s, const string& n)
  {
    // Pretty much the same logic as in find_target_type() above.
    //
    if (const scope* rs = s.root_scope ())
    {
      if (const target_type* r = rs->root_extra->target_types.find_file (n))
        return r;
    }

    return s.ctx.global_target_types.find_file (n);
  }

  pair<const target_type*, optional<string>> scope::
  find_target_type (name& n, const location& loc, const target_type* tt) const
  {
    // NOTE: see also functions-name.cxx:filter() if changing anything here.

    optional<string> ext;

    string& v (n.value);

    // If the name is typed, resolve the target type it and bail out if not
    // found. Otherwise, we know in the end it will resolve to something (if
    // nothing else, either dir{} or file{}), so we can go ahead and process
    // the name.
    //
    if (tt == nullptr)
    {
      if (n.typed ())
      {
        tt = find_target_type (n.type);

        if (tt == nullptr)
          return make_pair (tt, move (ext));
      }
      else
      {
        // Empty name as well as '.' and '..' signify a directory. Note that
        // this logic must be consistent with other places (grep for "..").
        //
        if (v.empty () || v == "." || v == "..")
          tt = &dir::static_type;
      }
    }

    // Directories require special name processing. If we find that more
    // targets deviate, then we should make this target type-specific.
    //
    if (tt != nullptr && (tt->is_a<dir> () || tt->is_a<fsdir> ()))
    {
      // The canonical representation of a directory name is with empty
      // value.
      //
      if (!v.empty ())
      {
        n.dir /= dir_path (v); // Move name value to dir.
        v.clear ();
      }
    }
    else if (!v.empty ())
    {
      // Split the path into its directory part (if any) the name part, and
      // the extension (if any).
      //
      // See also parser::expand_name_pattern() if changing anything here.
      //
      try
      {
        n.canonicalize ();
      }
      catch (const invalid_path& e)
      {
        fail (loc) << "invalid path '" << e.path << "'";
      }
      catch (const invalid_argument&)
      {
        // This is probably too general of a place to ignore multiple
        // trailing slashes and treat it as a directory (e.g., we don't want
        // to encourage this sloppiness in buildfiles). We could, however,
        // do it for certain contexts, such as buildspec. Maybe a lax flag?
        //
        fail (loc) << "invalid name '" << v << "'";
      }

      // Extract the extension.
      //
      ext = target::split_name (v, loc);
    }

    // If the target type is still unknown, map it using the name/extension,
    // falling back to file{}.
    //
    if (tt == nullptr)
    {
      // We only consider files without extension for file name mapping.
      //
      if (!ext)
        tt = find_target_type_file (*this, v);

      //@@ TODO: derive type from extension.

      if (tt == nullptr)
        tt = &file::static_type;
    }

    // If the target type does not use extensions but one was specified,
    // factor it back into the name (this way we won't assert when printing
    // diagnostics; see to_stream(target_key) for details).
    //
    if (ext                              &&
        tt->fixed_extension   == nullptr &&
        tt->default_extension == nullptr)
    {
      v += '.';
      v += *ext;
      ext = nullopt;
    }

    return make_pair (tt, move (ext));
  }

  pair<const target_type&, optional<string>> scope::
  find_target_type (name& n, name& o,
                    const location& loc,
                    const target_type* tt) const
  {
    auto r (find_target_type (n, loc, tt));

    if (r.first == nullptr)
      fail (loc) << "unknown target type " << n.type << " in " << n;

    bool src (n.pair); // If out-qualified, then it is from src.
    if (src)
    {
      assert (n.pair == '@');

      if (!o.directory ())
        fail (loc) << "expected directory after '@'";
    }

    dir_path& dir (n.dir);

    const dir_path& sd (src_path ());
    const dir_path& od (out_path ());

    bool nabs (false);

    if (dir.empty ())
      dir = src ? sd : od; // Already normalized.
    else
    {
      if (dir.relative ())
        dir = (src ? sd : od) / dir;
      else if (src)
        nabs = true;

      dir.normalize ();
    }

    dir_path out;
    if (src)
    {
      bool oabs (o.dir.absolute ());

      out = oabs ? move (o.dir) : od / o.dir;
      out.normalize ();

      // Make sure out and src are parallel unless both were specified as
      // absolute. We make an exception for this case because out may be used
      // to "tag" imported targets (see cc::search_library()). So it's sort of
      // the "I know what I am doing" escape hatch (it would have been even
      // better to verify such a target is outside any project but that won't
      // be cheap).
      //
      // See similar code for prerequisites in parser::parse_dependency().
      //
      if (nabs && oabs)
        ;
      else if (root_->out_eq_src ()
               ? out == dir
               //
               // @@ PERF: could just compare leafs in place.
               //
               : (out.sub (root_->out_path ()) &&
                  dir.sub (root_->src_path ()) &&
                  out.leaf (root_->out_path ()) == dir.leaf (root_->src_path ())))
        ;
      else
        // @@ TMP change warn to fail after 0.16.0 release.
        //
        warn (loc) << "target output directory " << out
                   << " must be parallel to source directory " << dir;

      // If this target is in this project, then out must be empty if this is
      // in source build. We assume that if either src or out are relative,
      // then it belongs to this project.
      //
      if (root_->out_eq_src ())
      {
        if (!nabs || !oabs || out.sub (root_->out_path ()))
          out.clear ();
      }
    }
    o.dir = move (out); // Result.

    return pair<const target_type&, optional<string>> (
      *r.first, move (r.second));
  }

  target_key scope::
  find_target_key (names& ns,
                   const location& loc,
                   const target_type* tt) const
  {
    if (size_t n = ns.size ())
    {
      if (n == (ns[0].pair ? 2 : 1))
      {
        name dummy;
        return find_target_key (ns[0], n == 1 ? dummy : ns[1], loc, tt);
      }
    }

    fail (loc) << "invalid target name: " << ns << endf;
  }

  pair<const target_type&, optional<string>> scope::
  find_prerequisite_type (name& n, name& o,
                          const location& loc,
                          const target_type* tt) const
  {
    auto r (find_target_type (n, loc, tt));

    if (r.first == nullptr)
      fail (loc) << "unknown target type " << n.type << " in " << n;

    if (n.pair) // If out-qualified, then it is from src.
    {
      assert (n.pair == '@');

      if (!o.directory ())
        fail (loc) << "expected directory after '@'";
    }

    if (!n.dir.empty ())
      n.dir.normalize (false, true); // Current dir collapses to an empty one.

    if (!o.dir.empty ())
      o.dir.normalize (false, true); // Ditto.

    return pair<const target_type&, optional<string>> (
      *r.first, move (r.second));
  }

  prerequisite_key scope::
  find_prerequisite_key (names& ns,
                         const location& loc,
                         const target_type* tt) const
  {
    if (size_t n = ns.size ())
    {
      if (n == (ns[0].pair ? 2 : 1))
      {
        name dummy;
        return find_prerequisite_key (ns[0], n == 1 ? dummy : ns[1], loc, tt);
      }
    }

    fail (loc) << "invalid prerequisite name: " << ns << endf;
  }

  static target*
  derived_tt_factory (context& c,
                      const target_type& t, dir_path d, dir_path o, string n)
  {
    // Pass our type to the base factory so that it can detect that it is
    // being called to construct a derived target. This can be used, for
    // example, to decide whether to "link up" to the group.
    //
    // One exception: if we are derived from a derived target type, then this
    // logic would lead to infinite recursion. So in this case get the
    // ultimate base.
    //
    const target_type* bt (t.base);
    for (; bt->factory == &derived_tt_factory; bt = bt->base) ;

    target* r (bt->factory (c, t, move (d), move (o), move (n)));
    r->derived_type = &t;
    return r;
  }

  pair<reference_wrapper<const target_type>, bool> scope::
  derive_target_type (const string& name,
                      const target_type& base,
                      target_type::flag flags)
  {
    assert (root_scope () == this);

    // Base target type uses extensions.
    //
    bool ext (base.fixed_extension   != nullptr ||
              base.default_extension != nullptr);

    // @@ Looks like we may need the ability to specify a fixed extension
    //    (which will be used to compare existing targets and not just
    //    search for existing files that is handled by the target_type::
    //    extension hook). See the file_factory() for details. We will
    //    probably need to specify it as part of the define directive (and
    //    have the ability to specify empty and NULL).
    //
    //    Currently, if we define myfile{}: file{}, then myfile{foo} and
    //    myfile{foo.x} are the same target.

    // Note: copies flags.
    //
    unique_ptr<target_type> dt (
      new target_type {
        nullptr, // Will be patched in by insert() below.
        &base,
        &derived_tt_factory,
        base.fixed_extension,
        base.default_extension,
        base.pattern,
        base.print,
        base.search,
        base.flags | flags});

#if 0
    // @@ We should probably inherit the fixed extension unless overriden with
    // another fixed? But then any derivation from file{} will have to specify
    // (or override) the fixed extension? But what is the use of deriving from
    // a fixed extension target and not overriding its extension? Some kind of
    // alias. Fuzzy.
    //
    dt->fixed_extension = nullptr /*&target_extension_fix<???>*/; // @@ TODO

    // Override default extension/pattern derivation function: we most likely
    // don't want to use the same default as our base (think cli: file). But,
    // if our base doesn't use extensions, then most likely neither do we
    // (think foo: alias).
    //
    dt->default_extension =
      ext && dt->fixed_extension == nullptr
      ? &target_extension_var<nullptr>
      : nullptr;

    dt->pattern =
      dt->fixed_extension != nullptr ? nullptr /*&target_pattern_fix<???>*/ :
      dt->default_extension != nullptr ? &target_pattern_var<nullptr> :
      nullptr;

    // There is actually a difference between "fixed fixed" (like man1{}) and
    // "fixed but overridable" (like file{}). Fuzzy: feels like there are
    // different kinds of "fixed" (file{} vs man{} vs man1{}).
    //
    dt->print =
      dt->fixed_extension != nullptr
      ? &target_print_0_ext_verb  // Fixed extension, no use printing.
      : nullptr;                  // Normal.
#endif

    // An attempt to clarify the above mess:
    //
    // 1. If we have a "really fixed" extension (like man1{}) then we keep
    //    it (including pattern and print functions).
    //
    // 2. Otherwise, we make it target_extension_var.
    //
    // Note that this still mis-fires for the following scenarios:
    //
    // file{} -- What if the user does not set the default extension expecting
    //           similar semantics as file{} or man{} itself. Maybe explicit
    //           via attribute (i.e., inherit from base)?
    //
    // @@ Get the fallback extension from base target_extension_var
    //    somehow (we know the base target type so could just call it)?
    //
    if (ext)
    {
      if (dt->fixed_extension == nullptr                ||
          dt->fixed_extension == &target_extension_none ||
          dt->fixed_extension == &target_extension_must)
      {
        dt->fixed_extension = nullptr;
        dt->default_extension = &target_extension_var<nullptr>;
        dt->pattern = &target_pattern_var<nullptr>;
        dt->print = nullptr;
      }
    }
    else
    {
      dt->fixed_extension = nullptr;
      dt->default_extension = nullptr;
      dt->pattern = nullptr;
      dt->print = nullptr;
    }

    return root_extra->target_types.insert (name, move (dt));
  }

  const target_type& scope::
  derive_target_type (const target_type& et)
  {
    assert (root_scope () == this);
    unique_ptr<target_type> dt (
      new target_type {
        nullptr, // Will be patched in by insert() below.
        et.base,
        &derived_tt_factory,
        et.fixed_extension,
        et.default_extension,
        et.pattern,
        et.print,
        et.search,
        et.flags});
    return root_extra->target_types.insert (et.name, move (dt)).first;
  }

  // scope_map
  //

  auto scope_map::
  insert_out (const dir_path& k, bool root) -> iterator
  {
    auto er (map_.emplace (k, scopes ()));

    if (er.second)
      er.first->second.push_back (nullptr);

    if (er.first->second.front () == nullptr)
    {
      er.first->second.front () = new scope (ctx, true /* shared */);
      er.second = true;
    }

    scope& s (*er.first->second.front ());

    // If this is a new scope, update the parent chain.
    //
    if (er.second)
    {
      scope* p (nullptr);

      // Update scopes of which we are a new parent/root (unless this is the
      // global scope). Also find our parent while at it.
      //
      if (map_.size () > 1)
      {
        // The first entry is ourselves.
        //
        auto r (map_.find_sub (k));
        for (++r.first; r.first != r.second; ++r.first)
        {
          if (scope* c = r.first->second.front ())
          {
            // The first scope of which we are a parent is the least
            // (shortest) one which means there is no other scope between it
            // and our parent.
            //
            if (p == nullptr)
              p = c->parent_;

            if (root && c->root_ == p->root_) // No intermediate root.
              c->root_ = &s;

            if (p == c->parent_) // No intermediate parent.
              c->parent_ = &s;
          }
        }

        // We couldn't get the parent from one of its old children so we have
        // to find it ourselves.
        //
        if (p == nullptr)
          p = &find_out (k.directory ());
      }

      s.parent_ = p;
      s.root_ = root ? &s : (p != nullptr ? p->root_ : nullptr);
    }
    else if (root && !s.root ())
    {
      // Upgrade to root scope.
      //
      auto r (map_.find_sub (k));
      for (++r.first; r.first != r.second; ++r.first)
      {
        if (scope* c = r.first->second.front ())
        {
          if (c->root_ == s.root_) // No intermediate root.
            c->root_ = &s;
        }
      }

      s.root_ = &s;
    }

    return er.first;
  }

  auto scope_map::
  insert_src (scope& s, const dir_path& k) -> iterator
  {
    auto er (map_.emplace (k, scopes ()));

    if (er.second)
      er.first->second.push_back (nullptr); // Owning out path entry.

    // It doesn't feel like this function can possibly be called multiple
    // times for the same scope and path so we skip the duplicate check.
    //
    er.first->second.push_back (&s);

    return er.first;
  }

  scope& scope_map::
  find_out (const dir_path& k)
  {
    assert (k.normalized (false)); // Allow non-canonical dir separators.

    // This one is tricky: if we found an entry that doesn't contain the
    // out path scope, then we need to consider outer scopes.
    //
    auto i (map_.find_sup_if (k,
                              [] (const pair<const dir_path, scopes>& v)
                              {
                                return v.second.front () != nullptr;
                              }));

    assert (i != map_.end ()); // Should have at least global scope.
    return *i->second.front ();
  }

  auto scope_map::
  find (const dir_path& k, bool sno) const -> pair<scopes::const_iterator,
                                                   scopes::const_iterator>
  {
    assert (k.normalized (false));
    auto i (map_.find_sup (k));
    assert (i != map_.end ());

    auto b (i->second.begin ());
    auto e (i->second.end ());

    // Skip NULL first element if requested.
    //
    if (sno && *b == nullptr)
      ++b;

    assert (b != e);
    return make_pair (b, e);
  }
}
