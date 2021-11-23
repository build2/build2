// file      : libbuild2/dyndep.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/dyndep.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
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
    // In particular, this function is used to make sure header dependencies
    // are up to date.
    //
    // There would normally be a lot of headers for every source file (think
    // all the system headers) and just calling execute_direct() on all of
    // them can get expensive. At the same time, most of these headers are
    // existing files that we will never be updating (again, system headers,
    // for example) and the rule that will match them is the fallback
    // file_rule. That rule has an optimization: it returns noop_recipe (which
    // causes the target state to be automatically set to unchanged) if the
    // file is known to be up to date. So we do the update "smartly".
    //
    const path_target* pt (t.is_a<path_target> ());

    if (pt == nullptr)
      ts = timestamp_unknown;

    target_state os (t.matched_state (a));

    if (os == target_state::unchanged)
    {
      if (ts == timestamp_unknown)
        return false;
      else
      {
        // We expect the timestamp to be known (i.e., existing file).
        //
        timestamp mt (pt->mtime ());
        assert (mt != timestamp_unknown);
        return mt > ts;
      }
    }
    else
    {
      // We only want to return true if our call to execute() actually caused
      // an update. In particular, the target could already have been in
      // target_state::changed because of the dynamic dependency extraction
      // run for some other target.
      //
      // @@ MT perf: so we are going to switch the phase and execute for
      //    any generated header.
      //
      phase_switch ps (t.ctx, run_phase::execute);
      target_state ns (execute_direct (a, t));

      if (ns != os && ns != target_state::unchanged)
      {
        l6 ([&]{trace << "updated " << t
                      << "; old state " << os
                      << "; new state " << ns;});
        return true;
      }
      else
        return ts != timestamp_unknown ? pt->newer (ts, ns) : false;
    }
  }

  optional<bool> dyndep_rule::
  inject_file (tracer& trace, const char* what,
               action a, target& t,
               const file& pt,
               timestamp mt,
               bool f)
  {
    // Even if failing we still use try_match() in order to issue consistent
    // (with other places) diagnostics (rather than the generic "not rule to
    // update ...").
    //
    if (!try_match (a, pt).first)
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
    t.prerequisite_targets[a].push_back (&pt);

    return r;
  }

  // Reverse-lookup target type(s) from file name/extension.
  //
  // If the list of base target types is specified, then only these types and
  // those derived from them are considered. Otherwise, any file-based type is
  // considered but not the file type itself.
  //
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

      // See if this path is inside a project with an out-of-tree build and is
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

  pair<const file*, bool> dyndep_rule::
  enter_file (tracer& trace, const char* what,
              action a, const scope& bs, target& t,
              path&& f, bool cache, bool norm,
              const function<map_extension_func>& map_extension,
              const target_type& fallback,
              const function<prefix_map_func>& get_pfx_map,
              const srcout_map& so_map)
  {
    // Find or maybe insert the target. The directory is only moved from if
    // insert is true. Note that it must be normalized.
    //
    auto find = [&trace, what, &t,
                 &map_extension, &fallback] (dir_path&& d,
                                             path&& f,
                                             bool insert) -> const file*
    {
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
      // example, because both C and X-language headers use the same .h
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
      {
        const scope& bs (**t.ctx.scopes.find (d).first);
        if (const scope* rs = bs.root_scope ())
        {
          if (map_extension != nullptr)
            tts = map_extension (bs, n, e);

          if (!bs.out_eq_src () && d.sub (bs.src_path ()))
            out = out_src (d, *rs);
        }
      }

      // If it is outside any project, or the project doesn't have such an
      // extension, use the fallback target type.
      //
      if (tts.empty ())
      {
        // If the project doesn't "know" this extension then we can't possibly
        // find an explicit target of this type.
        //
        if (!insert)
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
        // existing file) and go straight for the target object since we
        // need to find the target explicitly spelled out.
        //
        // Also, it doesn't feel like we should be able to resolve an
        // absolute path with a spelled-out extension to multiple targets.
        //
        for (const target_type* tt: tts)
        {
          if ((r = t.ctx.targets.find (*tt, d, out, n, e, trace)) != nullptr)
            break;
          else
            l6 ([&]{trace << "no targe with target type " << tt->name;});
        }

        // Note: we can't do this because of the in-source builds where there
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
      }

      // @@ OPT: move d, out, n
      //
      if (r == nullptr && insert)
        r = &search (t, *tts[0], d, out, n, &e, nullptr);

      return static_cast<const file*> (r);
    };

    // If it's not absolute then it either does not (yet) exist or is a
    // relative ""-include (see init_args() for details). Reduce the second
    // case to absolute.
    //
    // Note: we now always use absolute path to the translation unit so this
    // no longer applies. But let's keep it for posterity.
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

    // If still relative then it does not exist.
    //
    if (f.relative ())
    {
      // This is probably as often an error as an auto-generated file, so
      // trace at level 4.
      //
      l4 ([&]{trace << "non-existent " << what << " '" << f << "'";});

      f.normalize ();

      // The relative path might still contain '..' (e.g., ../foo.hxx;
      // presumably ""-include'ed). We don't attempt to support auto-
      // generated files with such inclusion styles.
      //
      if (get_pfx_map != nullptr && f.normalized ())
      {
        const prefix_map& pfx_map (get_pfx_map (a, bs, t));

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
          dir_path d (f.directory ());
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
              pt = find (pd / d, f.leaf (), !i->first.empty ());
              if (pt != nullptr)
              {
                f = pd / f;
                l4 ([&]{trace << "mapped as auto-generated " << f;});
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
    else
    {
      // Normalize the path unless it comes from the depdb, in which case
      // we've already done that (normally). This is also where we handle
      // src-out remap (again, not needed if cached).
      //
      if (!cache || norm)
        normalize_external (f, what);

      if (!cache)
      {
        if (!so_map.empty ())
        {
          // Find the most qualified prefix of which we are a sub-path.
          //
          auto i (so_map.find_sup (f));
          if (i != so_map.end ())
          {
            // Ok, there is an out tree for this file. Remap to a path from
            // the out tree and see if there is a target for it. Note that the
            // value in so_map is not necessarily canonical.
            //
            dir_path d (i->second);
            d /= f.leaf (i->first).directory ();
            d.canonicalize ();

            pt = find (move (d), f.leaf (), false); // d is not moved from.

            if (pt != nullptr)
            {
              path p (d / f.leaf ());
              l4 ([&]{trace << "remapping " << f << " to " << p;});
              f = move (p);
              remapped = true;
            }
          }
        }
      }

      if (pt == nullptr)
      {
        l6 ([&]{trace << "entering " << f;});
        pt = find (f.directory (), f.leaf (), true);
      }
    }

    return make_pair (pt, remapped);
  }
}
