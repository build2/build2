// file      : libbuild2/dyndep.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/dyndep.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/search.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  bool dyndep_rule::
  update (tracer& trace, action a, const target& t, timestamp ts)
  {
    return update_during_match (trace, a, t, ts);
  }

  optional<bool> dyndep_rule::
  inject_file (tracer& trace, const char* what,
               action a, target& t,
               const file& pt,
               timestamp mt,
               bool f,
               bool adhoc,
               uintptr_t data)
  {
    // Even if failing we still use try_match_sync() in order to issue
    // consistent (with other places) diagnostics (rather than the generic
    // "not rule to update ...").
    //
    if (!try_match_sync (a, pt).first)
    {
      if (!f)
        return nullopt;

      diag_record dr;
      dr << fail << what << ' ' << pt << " not found and no rule to "
         << "generate it";

      if (verb < 4)
        dr << info << "re-run with --verbose=4 for more information";
    }

    bool r (update (trace, a, pt, mt));

    // Add to our prerequisite target list.
    //
    t.prerequisite_targets[a].emplace_back (&pt, adhoc, data);

    return r;
  }

  // Check if the specified prerequisite is updated during match by any other
  // prerequisites of the specified target, recursively.
  //
  static bool
  updated_during_match (action a, const target& t, size_t pts_n,
                        const target& pt)
  {
    const auto& pts (t.prerequisite_targets[a]);

    for (size_t i (0); i != pts_n; ++i)
    {
      const prerequisite_target& p (pts[i]);

      // If include_target flag is specified, then p.data contains the
      // target pointer.
      //
      if (const target* xt =
          (p.target != nullptr ? p.target :
           ((p.include & prerequisite_target::include_target) != 0
            ? reinterpret_cast<target*> (p.data)
            : nullptr)))
      {
        if (xt == &pt && (p.include & prerequisite_target::include_udm) != 0)
          return true;

        if (size_t n = xt->prerequisite_targets[a].size ())
        {
          if (updated_during_match (a, *xt, n, pt))
            return true;
        }
      }
    }

    return false;
  }

  optional<bool> dyndep_rule::
  inject_existing_file (tracer& trace, const char* what,
                        action a, target& t, size_t pts_n,
                        const file& pt,
                        timestamp mt,
                        bool f,
                        bool adhoc,
                        uintptr_t data)
  {
    if (!try_match_sync (a, pt).first)
    {
      if (!f)
        return nullopt;

      diag_record dr;
      dr << fail << what << ' ' << pt << " not found and no rule to "
         << "generate it";

      if (verb < 4)
        dr << info << "re-run with --verbose=4 for more information";
    }

    recipe_function* const* rf (pt[a].recipe.target<recipe_function*> ());
    if (rf == nullptr || *rf != &noop_action)
    {
      if (pts_n == 0 || !updated_during_match (a, t, pts_n, pt))
      {
        fail << what << ' ' << pt << " has non-noop recipe" <<
          info << "consider listing it as static prerequisite of " << t;
      }
    }

    bool r (update (trace, a, pt, mt));

    // Add to our prerequisite target list.
    //
    t.prerequisite_targets[a].emplace_back (&pt, adhoc, data);

    return r;
  }

  void dyndep_rule::
  verify_existing_file (tracer&, const char* what,
                        action a, const target& t, size_t pts_n,
                        const file& pt)
  {
    diag_record dr;

    if (pt.matched (a, memory_order_acquire))
    {
      recipe_function* const* rf (pt[a].recipe.target<recipe_function*> ());
      if (rf == nullptr || *rf != &noop_action)
      {
        if (pts_n == 0 || !updated_during_match (a, t, pts_n, pt))
        {
          dr << fail << what << ' ' << pt << " has non-noop recipe";
        }
      }
    }
    else if (pt.decl == target_decl::real)
    {
      // Note that this target could not possibly be updated during match
      // since it's not matched.
      //
      dr << fail << what << ' ' << pt << " is explicitly declared as "
         << "target and may have non-noop recipe";
    }

    if (!dr.empty ())
      dr << info << "consider listing it as static prerequisite of " << t;
  }

  small_vector<const target_type*, 2> dyndep_rule::
  map_extension (const scope& bs,
                 const string& n, const string& e,
                 const target_type* const* tts)
  {
    // We will just have to try all of the possible ones, in the "most
    // likely to match" order.
    //
    auto test = [&bs, &n, &e] (const target_type& tt) -> bool
    {
      if (tt.default_extension != nullptr)
      {
        // Call the extension derivation function. Here we know that it will
        // only use the target type and name from the target key so we can
        // pass bogus values for the rest.
        //
        target_key tk {&tt, nullptr, nullptr, &n, nullopt};

        // This is like prerequisite search.
        //
        optional<string> de (tt.default_extension (tk, bs, nullptr, true));

        return de && *de == e;
      }

      return false;
    };

    small_vector<const target_type*, 2> r;

    if (tts != nullptr)
    {
      // @@ What if these types are not known by this project? Maybe this
      //    should just be unified with the below loop? Need to make sure
      //    we don't rely on the order in which they are returned.
      //
      for (const target_type* const* p (tts); *p != nullptr; ++p)
        if (test (**p))
          r.push_back (*p);
    }

    // Next try target types derived from any of the base types (or file if
    // there are no base types).
    //
    const target_type_map& ttm (bs.root_scope ()->root_extra->target_types);

    for (auto i (ttm.type_begin ()), e (ttm.type_end ()); i != e; ++i)
    {
      const target_type& dt (i->second);

      if (tts != nullptr)
      {
        for (const target_type* const* p (tts); *p != nullptr; ++p)
        {
          const target_type& bt (**p);

          if (dt.is_a (bt))
          {
            if (dt != bt && test (dt))
              r.push_back (&dt);

            break;
          }
        }
      }
      else
      {
        // Anything file-derived but not the file itself.
        //
        if (dt.is_a<file> () && dt != file::static_type && test (dt))
          r.push_back (&dt);
      }
    }

    return r;
  }

  void dyndep_rule::
  append_prefix (tracer& trace, prefix_map& m, const target& t, dir_path d)
  {
    // If the target directory is a sub-directory of the include directory,
    // then the prefix is the difference between the two. Otherwise, leave it
    // empty.
    //
    // The idea here is to make this "canonical" setup work auto-magically
    // (using C/C++ #include's as an example):
    //
    // 1. We include all headers with a prefix, e.g., <foo/bar>.
    //
    // 2. The library target is in the foo/ sub-directory, e.g., /tmp/foo/.
    //
    // 3. The poptions variable contains -I/tmp.
    //
    dir_path p (t.dir.sub (d) ? t.dir.leaf (d) : dir_path ());

    // We use the target's directory as out_base but that doesn't work well
    // for targets that are stashed in subdirectories. So as a heuristics we
    // are going to also enter the outer directories of the original prefix.
    // It is, however, possible, that another directory after this one will
    // produce one of these outer prefixes as its original prefix in which
    // case we should override it.
    //
    // So we are going to assign the original prefix priority value 0
    // (highest) and then increment it for each outer prefix.
    //
    auto enter = [&trace, &m] (dir_path p, dir_path d, size_t prio)
    {
      auto j (m.lower_bound (p)), e (m.end ());

      if (j != e && j->first != p)
        j = e;

      if (j == m.end ())
      {
        if (verb >= 4)
          trace << "new mapping for prefix '" << p << "'\n"
                << "  new mapping to      " << d << " priority " << prio;

        m.emplace (move (p), prefix_value {move (d), prio});
      }
      else if (p.empty ())
      {
        // For prefixless we keep all the entries since for them we have an
        // extra check (target must be explicitly spelled out in a buildfile).
        //
        if (verb >= 4)
          trace << "additional mapping for prefix '" << p << "'\n"
                << "  new mapping to      " << d << " priority " << prio;

        // Find the position where to insert according to the priority.
        // For equal priorities we use the insertion order.
        //
        do
        {
          if (j->second.priority > prio)
            break;
        }
        while (++j != e && j->first == p);

        m.emplace_hint (j, move (p), prefix_value {move (d), prio});
      }
      else
      {
        prefix_value& v (j->second);

        // We used to reject duplicates but it seems this can be reasonably
        // expected to work according to the order of, say, -I options.
        //
        // Seeing that we normally have more "specific" -I paths first, (so
        // that we don't pick up installed headers, etc), we ignore it.
        //
        if (v.directory == d)
        {
          if (v.priority > prio)
            v.priority = prio;
        }
        else if (v.priority <= prio)
        {
          if (verb >= 4)
            trace << "ignoring mapping for prefix '" << p << "'\n"
                  << "  existing mapping to " << v.directory
                  << " priority " << v.priority << '\n'
                  << "  another mapping to  " << d << " priority " << prio;
        }
        else
        {
          if (verb >= 4)
            trace << "overriding mapping for prefix '" << p << "'\n"
                  << "  existing mapping to " << v.directory
                  << " priority " << v.priority << '\n'
                  << "  new mapping to      " << d << " priority " << prio;

          v.directory = move (d);
          v.priority = prio;
        }
      }
    };

    // Enter all outer prefixes, including prefixless.
    //
    // The prefixless part is fuzzy but seems to be doing the right thing
    // ignoring/overriding-wise, at least in cases where one of the competing
    // include search paths is a subdirectory of another.
    //
    for (size_t prio (0);; ++prio)
    {
      bool e (p.empty ());
      enter ((e ? move (p) : p), (e ? move (d) : d), prio);
      if (e)
        break;
      p = p.directory ();
    }
  }

  bool dyndep_rule::srcout_builder::
  next (dir_path&& d)
  {
    // Ignore any paths containing '.', '..' components. Allow any directory
    // separators though (think -I$src_root/foo on Windows).
    //
    if (d.absolute () && d.normalized (false))
    {
      // If we have a candidate out_base, see if this is its src_base.
      //
      if (prev_ != nullptr)
      {
        const dir_path& bp (prev_->src_path ());

        if (d.sub (bp))
        {
          if (diff_.empty () || d.leaf (bp) == diff_)
          {
            // We've got a pair.
            //
            map_.emplace (move (d), prev_->out_path () / diff_);
            prev_ = nullptr; // Taken.
            return true;
          }
        }

        // Not a pair. Fall through to consider as out_base.
        //
        prev_ = nullptr;
      }

      // See if this path is inside a project with an out of source build and is
      // in the out directory tree.
      //
      const scope& bs (ctx_.scopes.find_out (d));
      if (bs.root_scope () != nullptr)
      {
        if (!bs.out_eq_src ())
        {
          const dir_path& bp (bs.out_path ());

          bool e;
          if ((e = (d == bp)) || d.sub (bp))
          {
            prev_ = &bs;
            if (e)
              diff_.clear ();
            else
              diff_ = d.leaf (bp);
          }
        }
      }
    }
    else
      prev_ = nullptr;

    return false;
  }

  static pair<const file*, bool>
  enter_file_impl (
    tracer& trace, const char* what,
    action a, const scope& bs, const target& t,
    path& fp, bool cache, bool norm,
    bool insert,
    bool dynamic,
    const function<dyndep_rule::map_extension_func>& map_extension,
    const target_type& fallback,
    const function<dyndep_rule::prefix_map_func>& get_pfx_map,
    const dyndep_rule::srcout_map& so_map)
  {
    // NOTE: see enter_header() caching logic if changing anyting here with
    //       regards to the target and base scope usage.

    assert (!insert || t.ctx.phase == run_phase::match);

    // Find or maybe insert the target.
    //
    // If insert is false, then don't consider dynamically-created targets
    // (i.e., those that are not real or implied) unless dynamic is true, in
    // which case return the target that would have been inserted.
    //
    // The directory is only moved from if insert is true. Note that it must
    // be absolute and normalized.
    //
    auto find = [&trace, what, &bs, &t,
                 &map_extension,
                 &fallback] (dir_path&& d,
                             path&& f,
                             bool insert,
                             bool dynamic = false) -> const file*
    {
      context& ctx (t.ctx);

      // Split the file into its name part and extension. Here we can assume
      // the name part is a valid filesystem name.
      //
      // Note that if the file has no extension, we record an empty extension
      // rather than NULL (which would signify that the default extension
      // should be added).
      //
      string e (f.extension ());
      string n (move (f).string ());

      if (!e.empty ())
        n.resize (n.size () - e.size () - 1); // One for the dot.

      // See if this directory is part of any project and if so determine
      // the target type.
      //
      // While at it also determine if this target is from the src or out
      // tree of said project.
      //
      dir_path out;

      // It's possible the extension-to-target type mapping is ambiguous (for
      // example, because both C and C++-language headers use the same .h
      // extension). In this case we will first try to find one that matches
      // an explicit target (similar logic to when insert is false).
      //
      small_vector<const target_type*, 2> tts;

      // Note that the path can be in out or src directory and the latter
      // can be associated with multiple scopes. So strictly speaking we
      // need to pick one that is "associated" with us. But that is still a
      // TODO (see scope_map::find() for details) and so for now we just
      // pick the first one (it's highly unlikely the source file extension
      // mapping will differ based on the configuration).
      //
      // Note that we also need to remember the base scope for search() below
      // (failed that, search_existing_file() will refuse to look).
      //
      const scope* s (nullptr);
      {
        // While we cannot accurately associate in the general case, we can do
        // so if the path belongs to this project.
        //
        const scope& rs (*bs.root_scope ());
        bool src (false);
        if (d.sub (rs.out_path ()) ||
            (src = (!rs.out_eq_src () && d.sub (rs.src_path ()))))
        {
          if (map_extension != nullptr)
            tts = map_extension (bs, n, e);

          if (src)
            out = out_src (d, rs);

          s = &bs;
        }
        else
        {
          const scope& bs (**ctx.scopes.find (d).first);
          if (const scope* rs = bs.root_scope ())
          {
            if (map_extension != nullptr)
              tts = map_extension (bs, n, e);

            if (!rs->out_eq_src () && d.sub (rs->src_path ()))
              out = out_src (d, *rs);

            s = &bs;
          }
        }
      }

      // If it is outside any project, or the project doesn't have such an
      // extension, use the fallback target type.
      //
      if (tts.empty ())
      {
        // If the project doesn't "know" this extension then we can't possibly
        // find a real or implied target of this type.
        //
        if (!insert && !dynamic)
        {
          l6 ([&]{trace << "unknown " << what << ' ' << n << " extension '"
                        << e << "'";});
          return nullptr;
        }

        tts.push_back (&fallback);
      }

      // Find or insert target.
      //
      // Note that in case of the target type ambiguity we first try to find
      // an explicit target that resolves this ambiguity.
      //
      const target* r (nullptr);

      if (!insert || tts.size () > 1)
      {
        // Note that we skip any target type-specific searches (like for an
        // existing file) and go straight for the target object since we need
        // to find the target explicitly spelled out.
        //
        // Also, it doesn't feel like we should be able to resolve an absolute
        // path with a spelled-out extension to multiple targets.
        //
        const target* f (nullptr);

        for (size_t i (0), m (tts.size ()); i != m; ++i)
        {
          const target_type& tt (*tts[i]);

          if (const target* x = ctx.targets.find (tt, d, out, n, e, trace))
          {
            // What would be the harm in reusing a dynamically-inserted target
            // if there is no buildfile-mentioned one? Probably none (since it
            // can't be updated) except that it will be racy: sometimes we
            // will reuse the dynamic, sometimes we will insert a new one. And
            // we don't like racy.
            //
            // Note that we can't only check for real targets and must include
            // implied ones because pre-entered members of a target group
            // (e.g., cli.cxx) are implied.
            //
            if (operator>= (x->decl, target_decl::implied)) // @@ VC14
            {
              r = x;
              break;
            }
            else
            {
              // Cache the dynamic target corresponding to tts[0] since that's
              // what we will be inserting (see below).
              //
              if ((insert || dynamic) && i == 0)
                f = x;

              l6 ([&]{trace << "dynamic target with target type " << tt.name;});
            }
          }
          else
            l6 ([&]{trace << "no target with target type " << tt.name;});
        }

        // Note: we can't do this because of the in source builds where there
        // won't be explicit targets for non-generated files.
        //
        // This should be harmless, however, since in our world generated file
        // are spelled-out as explicit targets. And if not, we will still get
        // an error, just a bit less specific.
        //
#if 0
        if (r == nullptr && insert)
        {
          f = d / n;
          if (!e.empty ())
          {
            f += '.';
            f += e;
          }

          diag_record dr (fail);
          dr << "ambiguous mapping of " << what ' ' << f << " to target type";
          for (const target_type* tt: tts)
            dr << info << "could be " << tt->name << "{}";
          dr << info << "spell-out its target to resolve this ambiguity";
        }
#endif

        if (r == nullptr && f != nullptr)
          r = f;
      }

      if (r == nullptr && insert)
      {
        // Like search(t, pk) but don't fail if the target is in src.
        //
        // While it may seem like there is not much difference, the caller may
        // actually do more than just issue more specific diagnostics. For
        // example, it may defer the failure to the tool diagnostics.
        //
#if 0
        r = &search (t, *tts[0], d, out, n, &e, s);
#else
        prerequisite_key pk {nullopt, {tts[0], &d, &out, &n, move (e)}, s};

        r = pk.tk.type->search (ctx, &t, pk);

        if (r == nullptr && pk.tk.out->empty ())
        {
          auto p (ctx.scopes.find (d, false));
          if (*p.first != nullptr || ++p.first == p.second)
            r = &create_new_target (ctx, pk);
        }
#endif
      }

      return static_cast<const file*> (r);
    };

    // If it's not absolute then it either does not (yet) exist or is a
    // relative ""-include (see init_args() for details). Reduce the second
    // case to absolute.
    //
    // Note: we now always use absolute path to the translation unit so this
    // no longer applies. But let's keep it for posterity.
    //
    // Also note that we now assume (see cc::compile_rule::enter_header()) a
    // relative path signifies a generated header.
    //
#if 0
    if (f.relative () && rels.relative ())
    {
      // If the relative source path has a directory component, make sure it
      // matches since ""-include will always start with that (none of the
      // compilers we support try to normalize this path). Failed that we may
      // end up searching for a generated header in a random (working)
      // directory.
      //
      const string& fs (f.string ());
      const string& ss (rels.string ());

      size_t p (path::traits::rfind_separator (ss));

      if (p == string::npos || // No directory.
          (fs.size () > p + 1 &&
           path::traits::compare (fs.c_str (), p, ss.c_str (), p) == 0))
      {
        path t (work / f); // The rels path is relative to work.

        if (exists (t))
          f = move (t);
      }
    }
#endif

    const file* pt (nullptr);
    bool remapped (false);

    // If relative then it does not exist.
    //
    if (fp.relative ())
    {
      // This is probably as often an error as an auto-generated file, so
      // trace at level 4.
      //
      l4 ([&]{trace << "non-existent " << what << " '" << fp << "'";});

      if (get_pfx_map != nullptr)
      {
        fp.normalize ();

        // The relative path might still contain '..' (e.g., ../foo.hxx;
        // presumably ""-include'ed). We don't attempt to support auto-
        // generated files with such inclusion styles.
        //
        if (fp.normalized ())
        {
          const dyndep_rule::prefix_map& pfx_map (get_pfx_map (a, bs, t));

          // First try the whole file. Then just the directory.
          //
          // @@ Has to be a separate map since the prefix can be the same as
          //    the file name.
          //
          // auto i (pfx_map->find (f));

          // Find the most qualified prefix of which we are a sub-path.
          //
          if (!pfx_map.empty ())
          {
            dir_path d (fp.directory ());
            auto p (pfx_map.sup_range (d));

            if (p.first != p.second)
            {
              // Note that we can only have multiple entries for the
              // prefixless mapping.
              //
              dir_path pd; // Reuse.
              for (auto i (p.first); i != p.second; ++i)
              {
                // Note: value in pfx_map is not necessarily canonical.
                //
                pd = i->second.directory;
                pd.canonicalize ();

                l4 ([&]{trace << "try prefix '" << d << "' mapped to " << pd;});

                // If this is a prefixless mapping, then only use it if we can
                // resolve it to an existing target (i.e., it is explicitly
                // spelled out in a buildfile). @@ Hm, I wonder why, it's not
                // like we can generate any file without an explicit target.
                // Maybe for diagnostics (i.e., we will actually try to build
                // something there instead of just saying no mapping).
                //
                if (i->first.empty ())
                  pt = find (pd / d, fp.leaf (), false);
                else
                  pt = find (pd / d, fp.leaf (), insert, dynamic);

                if (pt != nullptr)
                {
                  fp = pd / fp;
                  l4 ([&]{trace << "mapped as auto-generated " << fp;});
                  break;
                }
                else
                  l4 ([&]{trace << "no explicit target in " << pd;});
              }
            }
            else
              l4 ([&]{trace << "no prefix map entry for '" << d << "'";});
          }
          else
            l4 ([&]{trace << "prefix map is empty";});
        }
      }
    }
    else
    {
      // Normalize the path unless it is already normalized. This is also
      // where we handle src-out remap which is not needed if cached.
      //
      if (!norm)
        normalize_external (fp, what);

      if (!cache)
      {
        if (!so_map.empty ())
        {
          // Find the most qualified prefix of which we are a sub-path.
          //
          auto i (so_map.find_sup (fp));
          if (i != so_map.end ())
          {
            // Ok, there is an out tree for this file. Remap to a path from
            // the out tree and see if there is a target for it. Note that the
            // value in so_map is not necessarily canonical.
            //
            dir_path d (i->second);
            d /= fp.leaf (i->first).directory ();
            d.canonicalize ();

            pt = find (move (d), fp.leaf (), false); // d is not moved from.

            if (pt != nullptr)
            {
              path p (d / fp.leaf ());
              l4 ([&]{trace << "remapping " << fp << " to " << p;});
              fp = move (p);
              remapped = true;
            }
          }
        }
      }

      if (pt == nullptr)
      {
        l6 ([&]{trace << (insert ? "entering " : "finding ") << fp;});
        pt = find (fp.directory (), fp.leaf (), insert, dynamic);
      }
    }

    return make_pair (pt, remapped);
  }

  pair<const file*, bool> dyndep_rule::
  enter_file (tracer& trace, const char* what,
              action a, const scope& bs, target& t,
              path& fp, bool cache, bool norm,
              const function<map_extension_func>& map_ext,
              const target_type& fallback,
              const function<prefix_map_func>& pfx_map,
              const srcout_map& so_map)
  {
    return enter_file_impl (trace, what,
                            a, bs, t,
                            fp, cache, norm,
                            true /* insert */, false,
                            map_ext, fallback, pfx_map, so_map);
  }

  pair<const file*, bool> dyndep_rule::
  find_file (tracer& trace, const char* what,
             action a, const scope& bs, const target& t,
             path& fp, bool cache, bool norm,
             bool dynamic,
             const function<map_extension_func>& map_ext,
             const target_type& fallback,
             const function<prefix_map_func>& pfx_map,
             const srcout_map& so_map)
  {
    return enter_file_impl (trace, what,
                            a, bs, t,
                            fp, cache, norm,
                            false /* insert */, dynamic,
                            map_ext, fallback, pfx_map, so_map);
  }

  static pair<const file&, bool>
  inject_group_member_impl (action a, const scope& bs, mtime_target& g,
                            path f, string n, string e,
                            const target_type& tt,
                            const function<dyndep_rule::group_filter_func>& fl)
  {
    // NOTE: see adhoc_rule_regex_pattern::apply_group_members() for a variant
    //       of the same code.

    // Note that we used to directly match such a member with group_recipe.
    // But that messes up our dependency counts since we don't really know
    // whether someone will execute such a member.
    //
    // So instead we now just link the member up to the group and rely on the
    // special semantics in match_rule_impl() for groups with the dyn_members
    // flag.
    //
    assert ((g.type ().flags & target_type::flag::dyn_members) ==
            target_type::flag::dyn_members);

    // We expect that nobody else can insert these members (seems reasonable
    // seeing that their names are dynamically discovered).
    //
    auto l (search_new_locked (
              bs.ctx,
              tt,
              f.directory (),
              dir_path (), // Always in out.
              move (n),
              &e,
              &bs));

    const file& t (l.first.as<file> ()); // Note: non-const only if have lock.

    // We don't need to match the group recipe directy from ad hoc
    // recipes/rules due to the special semantics for explicit group members
    // in match_rule_impl(). This is what skip_match is for.
    //
    if (l.second)
    {
      l.first.group = &g;
      l.second.unlock ();
      t.path (move (f));
      return pair<const file&, bool> (t, true);
    }
    else
    {
      if (fl != nullptr && !fl (g, t))
        return pair<const file&, bool> (t, false);
    }

    // Check if we already belong to this group. Note that this not a mere
    // optimization since we may be in the member->group->member chain and
    // trying to lock the member the second time would deadlock (this can be
    // triggered, for example, by dist, which sort of depends on such members
    // directly... which was not quite correct and is now fixed).
    //
    if (t.group == &g) // Note: atomic.
      t.path (move (f));
    else
    {
      // This shouldn't normally fail since we are the only ones that should
      // know about this target (otherwise why is it dynamicaly discovered).
      // However, nothing prevents the user from depending on such a target,
      // however misguided.
      //
      target_lock tl (lock (a, t));

      if (!tl)
        fail << "group " << g << " member " << t << " is already matched" <<
          info << "dynamically extracted group members cannot be used as "
               << "prerequisites directly, only via group";

      if (t.group == nullptr)
        tl.target->group = &g;
      else if (t.group != &g)
        fail << "group " << g << " member " << t
             << " is already member of group " << *t.group;

      t.path (move (f));
    }

    return pair<const file&, bool> (t, true);
  }

  pair<const file&, bool> dyndep_rule::
  inject_group_member (action a, const scope& bs, mtime_target& g,
                       path f,
                       const target_type& tt,
                       const function<group_filter_func>& filter)
  {
    path n (f.leaf ());
    string e (n.extension ());
    n.make_base ();

    return inject_group_member_impl (a, bs, g,
                                     move (f), move (n).string (), move (e),
                                     tt,
                                     filter);
  }

  static const target_type&
  map_target_type (const char* what,
                   const scope& bs,
                   const path& f, const string& n, const string& e,
                   const function<dyndep_rule::map_extension_func>& map_ext,
                   const target_type& fallback)
  {
    // Map extension to the target type, falling back to the fallback type.
    //
    small_vector<const target_type*, 2> tts;
    if (map_ext != nullptr)
      tts = map_ext (bs, n, e);

    // Not sure what else we can do in this case.
    //
    if (tts.size () > 1)
    {
      diag_record dr (fail);

      dr << "mapping of " << what << " target path " << f
         << " to target type is ambiguous";

      for (const target_type* tt: tts)
        dr << info << "can be " << tt->name << "{}";
    }

    const target_type& tt (tts.empty () ? fallback : *tts.front ());

    if (!tt.is_a<file> ())
    {
      fail << what << " target path " << f << " mapped to non-file-based "
           << "target type " << tt.name << "{}";
    }

    return tt;
  }

  pair<const file&, bool> dyndep_rule::
  inject_group_member (const char* what,
                       action a, const scope& bs, mtime_target& g,
                       path f,
                       const function<map_extension_func>& map_ext,
                       const target_type& fallback,
                       const function<group_filter_func>& filter)
  {
    path n (f.leaf ());
    string e (n.extension ());
    n.make_base ();

    // Map extension to the target type, falling back to the fallback type.
    //
    const target_type& tt (
      map_target_type (what, bs, f, n.string (), e, map_ext, fallback));

    return inject_group_member_impl (a, bs, g,
                                     move (f), move (n).string (), move (e),
                                     tt,
                                     filter);
  }

  pair<const file&, bool>
  inject_adhoc_group_member_impl (action, const scope& bs, target& t,
                                  path f, string n, string e,
                                  const target_type& tt)
  {
    // Assume nobody else can insert these members (seems reasonable seeing
    // that their names are dynamically discovered).
    //
    auto l (search_new_locked (
              bs.ctx,
              tt,
              f.directory (),
              dir_path (), // Always in out.
              move (n),
              &e,
              &bs));

    file* ft (&l.first.as<file> ()); // Note: non-const only if locked.

    // Skip if this is one of the static targets (or a duplicate of the
    // dynamic target).
    //
    // In particular, we expect to skip all the targets that we could not lock
    // (e.g., in case all of this has already been done for the previous
    // operation in a batch; make sure to test `update update update` and
    // `update clean update ...` batches if changing anything here).
    //
    // While at it also find the ad hoc members list tail.
    //
    const_ptr<target>* tail (&t.adhoc_member);
    for (target* m (&t); m != nullptr; m = m->adhoc_member)
    {
      if (ft == m)
      {
        tail = nullptr;
        break;
      }

      tail = &m->adhoc_member;
    }

    if (tail == nullptr)
      return pair<const file&, bool> (*ft, false);

    if (!l.second)
      fail << "dynamic target " << *ft << " already exists and cannot be "
           << "made ad hoc member of group " << t;

    ft->group = &t;
    l.second.unlock ();

    // We need to be able to distinguish static targets from dynamic (see the
    // static set hashing in adhoc_buildscript_rule::apply() for details).
    //
    assert (ft->decl != target_decl::real);

    *tail = ft;
    ft->path (move (f));

    return pair<const file&, bool> (*ft, true);
  }

  pair<const file&, bool> dyndep_rule::
  inject_adhoc_group_member (action a, const scope& bs, target& t,
                             path f,
                             const target_type& tt)
  {
    path n (f.leaf ());
    string e (n.extension ());
    n.make_base ();

    return inject_adhoc_group_member_impl (
      a, bs, t, move (f), move (n).string (), move (e), tt);
  }

  pair<const file&, bool> dyndep_rule::
  inject_adhoc_group_member (const char* what,
                             action a, const scope& bs, target& t,
                             path f,
                             const function<map_extension_func>& map_ext,
                             const target_type& fallback)
  {
    path n (f.leaf ());
    string e (n.extension ());
    n.make_base ();

    // Map extension to the target type, falling back to the fallback type.
    //
    const target_type& tt (
      map_target_type (what, bs, f, n.string (), e, map_ext, fallback));


    return inject_adhoc_group_member_impl (
      a, bs, t, move (f), move (n).string (), move (e), tt);
  }
}
