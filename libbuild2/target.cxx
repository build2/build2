// file      : libbuild2/target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/target.hxx>

#include <cstring> // strcmp()

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/search.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  // target_type
  //
  bool target_type::
  is_a (const char* n) const
  {
    for (const target_type* b (this); b != nullptr; b = b->base)
      if (strcmp (b->name, n) == 0)
        return true;

    return false;
  }

  // target_key
  //
  void target_key::
  as_name (names& r) const
  {
    string v;
    if (!name->empty ())
    {
      v = *name;
      // @@ TMP: see also other calls to combine_name() -- need to fix.
      //
      target::combine_name (v, ext, false /* @@ TMP: what to do? */);
    }
    else
      assert (!ext || ext->empty ()); // Unspecified or none.

    r.emplace_back (*dir, type->name, move (v));

    if (!out->empty ())
    {
      r.back ().pair = '@';
      r.emplace_back (*out);
    }
  }

  // target_state
  //
  static const char* const target_state_[] =
  {
    "<invalid>", // Absent/invalid (see target_state for details).
    "unknown",
    "unchanged",
    "postponed",
    "busy",
    "changed",
    "failed",
    "group"
  };

  string
  to_string (target_state ts)
  {
    return target_state_[static_cast<uint8_t> (ts)];
  }

  // target
  //
  const target::prerequisites_type target::empty_prerequisites_;

  target::
  ~target ()
  {
  }

  const string& target::
  ext (string v)
  {
    ulock l (ctx.targets.mutex_);

    // Once the extension is set, it is immutable. However, it is possible
    // that someone has already "branded" this target with a different
    // extension.
    //
    optional<string>& e (*ext_);

    if (!e)
      e = move (v);
    else if (*e != v)
    {
      string o (*e);
      l.unlock ();

      fail << "conflicting extensions '" << o << "' and '" << v << "' "
           << "for target " << *this;
    }

    return *e;
  }

  group_view target::
  group_members (action) const
  {
    // Not a group or doesn't expose its members.
    //
    return group_view {nullptr, 0};
  }

  const scope& target::
  base_scope_impl () const
  {
    // If this target is from the src tree, use its out directory to find
    // the scope.
    //
    const scope& s (ctx.scopes.find_out (out_dir ()));

    // Cache unless we are in the load phase.
    //
    if (ctx.phase != run_phase::load)
    {
      const scope* e (nullptr);
      if (!base_scope_.compare_exchange_strong (
            e,
            &s,
            memory_order_release,
            memory_order_consume))
        assert (e == &s);
    }

    return s;
  }

  pair<lookup, size_t> target::
  lookup_original (const variable& var,
                   bool target_only,
                   const scope* bs,
                   bool locked) const
  {
    pair<lookup_type, size_t> r (lookup_type (), 0);

    ++r.second;
    {
      auto p (vars.lookup (var));
      if (p.first != nullptr)
        r.first = lookup_type (*p.first, p.second, vars);
    }

    const target* g1 (nullptr);
    const target* g2 (nullptr);

    if (!r.first)
    {
      ++r.second;

      // While we went back to not treating the first member as a group for
      // variable lookup, let's keep this logic in case one day we end up with
      // a separate ad hoc group target.
      //
#if 0
      // In case of an ad hoc group, we may have to look in two groups.
      //
      if ((g1 = group) != nullptr)
      {
        auto p (g1->vars.lookup (var));
        if (p.first != nullptr)
          r.first = lookup_type (*p.first, p.second, g1->vars);
        else
        {
          if ((g2 = g1->group) != nullptr)
          {
            auto p (g2->vars.lookup (var));
            if (p.first != nullptr)
              r.first = lookup_type (*p.first, p.second, g2->vars);
          }
        }
      }
#else
      // Skip looking up in the ad hoc group, which is semantically the
      // first/primary member.
      //
      if ((g1 = group == nullptr
           ? nullptr
           : group->adhoc_group () ? group->group : group))
      {
        auto p (g1->vars.lookup (var));
        if (p.first != nullptr)
          r.first = lookup_type (*p.first, p.second, g1->vars);
      }
#endif
    }

    // Delegate to scope's lookup_original().
    //
    if (!r.first)
    {
      if (!target_only)
      {
        auto key = [locked] (const target* t)
        {
          return locked ? t->key_locked () : t->key ();
        };

        target_key tk (key (this));
        target_key g1k (g1 != nullptr ? key (g1) : target_key {});
        target_key g2k (g2 != nullptr ? key (g2) : target_key {});

        if (bs == nullptr)
          bs = &base_scope ();

        auto p (bs->lookup_original (var,
                                     &tk,
                                     g1 != nullptr ? &g1k : nullptr,
                                     g2 != nullptr ? &g2k : nullptr));

        r.first = move (p.first);
        r.second = r.first ? r.second + p.second : p.second;
      }
      else
        r.second = size_t (~0);
    }

    return r;
  }

  value& target::
  append (const variable& var, const scope* bs)
  {
    // Note: see also prerequisite::append() if changing anything here.

    // Note that here we want the original value without any overrides
    // applied.
    //
    auto l (lookup_original (var, false, bs).first);

    if (l.defined () && l.belongs (*this)) // Existing var in this target.
      return vars.modify (l); // Ok since this is original.

    value& r (assign (var)); // NULL.

    if (l.defined ())
      r = *l; // Copy value (and type) from the outer scope.

    return r;
  }

  value& target::
  append_locked (const variable& var, const scope* bs)
  {
    auto l (lookup_original (var, false, bs, true /* locked */).first);

    if (l.defined () && l.belongs (*this)) // Existing var in this target.
      return vars.modify (l); // Ok since this is original.

    value& r (assign (var)); // NULL.

    if (l.defined ())
      r = *l; // Copy value (and type) from the outer scope.

    return r;
  }

  pair<lookup, size_t> target::opstate::
  lookup_original (const variable& var, bool target_only) const
  {
    pair<lookup_type, size_t> r (lookup_type (), 0);

    ++r.second;
    {
      auto p (vars.lookup (var));
      if (p.first != nullptr)
        r.first = lookup_type (*p.first, p.second, vars);
    }

    // Delegate to target's lookup_original().
    //
    if (!r.first)
    {
      auto p (target_->lookup_original (var, target_only));

      r.first = move (p.first);
      r.second = r.first ? r.second + p.second : p.second;
    }

    return r;
  }

  optional<string> target::
  split_name (string& v, const location& loc)
  {
    assert (!v.empty ());

    // Normally, we treat the rightmost dot as an extension separator (but see
    // find_extension() for the exact semantics) and if none exists, then we
    // assume the extension is not specified. There are, however, special
    // cases that override this rule:
    //
    // - We treat triple dots as the "chosen extension separator" (used to
    //   resolve ambiguity as to which dot is the separator, for example,
    //   libfoo...u.a). If they are trailing triple dots, then this signifies
    //   the "unspecified (default) extension" (used when the extension in the
    //   name is not "ours", for example, cxx{foo.test...} for foo.test.cxx)
    //   Having multiple triple dots is illegal.
    //
    // - Otherwise, we treat a single trailing dot as the "specified no
    // - extension".
    //
    // - Finally, double dots are used as an escape sequence to make sure the
    //   dot is not treated as an extension separator (or as special by any of
    //   the above rules, for example, libfoo.u..a). In case of trailing
    //   double dots, we naturally assume there is no default extension.
    //
    // An odd number of dots other than one or three is illegal. This means,
    // in particular, that it's impossible to specify a base/extension pair
    // where either the base ends with a dot or the extension begins with one
    // (or both). We are ok with that.
    //
    // Dot-only sequences are illegal. Note though, that dir{.} and dir{..}
    // are handled ad hoc outside this function and are valid.

    // Note that we cannot unescape dots in-place before we validate the name
    // since it can be required for diagnostics. Thus, the plan is as follows:
    //
    // - Iterate right to left, searching for the extension dot, validating
    //   the name, and checking if any dots are escaped.
    //
    // - Split the name.
    //
    // - Unescape the dots in the name and/or extension, if required.

    // Search for an extension dot, validate the name, and check for escape
    // sequences.
    //
    optional<size_t> edp; // Extension dot position.
    size_t edn (0);       // Extension dot representation lenght (1 or 3).

    bool escaped (false);
    bool dot_only (true);
    size_t n (v.size ());

    // Iterate right to left until the beginning of the string or a directory
    // separator is encountered.
    //
    // At the end of the loop p will point to the beginning of the leaf.
    //
    size_t p (n - 1);

    for (;; --p)
    {
      char c (v[p]);

      if (c == '.')
      {
        // Find the first dot in the sequence.
        //
        size_t i (p);
        for (; i != 0 && v[i - 1] == '.'; --i) ;

        size_t sn (p - i + 1); // Sequence length.

        if (sn == 3)          // Triple dots?
        {
          if (edp && edn == 3)
            fail (loc) << "multiple triple dots in target name '" << v << "'";

          edp = i;
          edn = 3;
        }
        else if (sn == 1)     // Single dot?
        {
          if (!edp)
          {
            edp = i;
            edn = 1;
          }
        }
        else if (sn % 2 == 0) // Escape sequence?
          escaped = true;
        else
          fail (loc) << "invalid dot sequence in target name '" << v << "'";

        p = i; // Position to the first dot in the sequence.
      }
      else if (path::traits_type::is_separator (c))
      {
        // Position to the beginning of the leaf and bail out.
        //
        ++p;
        break;
      }
      else
        dot_only = false;

      if (p == 0)
        break;
    }

    if (dot_only)
      fail (loc) << "invalid target name '" << v << "'";

    // The leading dot cannot be an extension dot. Thus, the leading triple
    // dots are invalid and the leading single dot is not considered as such.
    //
    if (edp && *edp == p)
    {
      if (edn == 3)
        fail (loc) << "leading triple dots in target name '" << v << "'";

      edp = nullopt;
    }

    // Split the name.
    //
    optional<string> r;

    if (edp)
    {
      if (*edp != n - edn)          // Non-trailing dot?
        r = string (v, *edp + edn);
      else if (edn == 1)            // Trailing single dot?
        r = string ();
      //else if (edn == 3)          // Trailing triple dots?
      //  r = nullopt;

      v.resize (*edp);
    }
    else if (v.back () == '.')      // Trailing escaped dot?
      r = string ();

    if (!escaped)
      return r;

    // Unescape the dots.
    //
    auto unescape = [] (string& s, size_t b = 0)
    {
      size_t n (s.size ());
      for (size_t i (b); i != n; ++i)
      {
        if (s[i] == '.')
        {
          // Find the end of the dot sequence.
          //
          size_t j (i + 1);
          for (; j != n && s[j] == '.'; ++j) ;

          size_t sn (j - i); // Sequence length.

          // Multiple dots can only represent an escape sequence now.
          //
          if (sn != 1)
          {
            assert (sn % 2 == 0);

            size_t dn (sn / 2);   // Number of dots to remove.
            s.erase (i + dn, dn);

            i += dn - 1; // Position to the last dot in the sequence.
            n -= dn;     // Adjust string size counter.
          }
        }
      }
    };

    unescape (v, p);

    if (r)
      unescape (*r);

    return r;
  }

  // Escape the name according to the rules described in split_name(). The
  // idea is that we should be able to roundtrip things.
  //
  // Note though, that multiple representations can end up with the same
  // name, for example libfoo.u..a and libfoo...u.a. We will always resolve
  // ambiguity with the triple dot and only escape those dots that otherwise
  // can be misinterpreted (dot sequences, etc).
  //
  void target::
  combine_name (string& v, const optional<string>& e, bool de)
  {
    // Escape all dot sequences since they can be misinterpreted as escape
    // sequences and return true if the result contains an unescaped dot that
    // can potentially be considered an extension dot.
    //
    // In the name mode only consider the basename, escape the trailing dot
    // (since it can be misinterpreted as the 'no extension' case), and don't
    // treat the basename leading dot as the potential extension dot.
    //
    auto escape = [] (string& s, bool name) -> bool
    {
      if (s.empty ())
        return false;

      bool r (false);
      size_t n (s.size ());

      // Iterate right to left until the beginning of the string or a
      // directory separator is encountered.
      //
      for (size_t p (n - 1);; --p)
      {
        char c (s[p]);

        if (c == '.')
        {
          // Find the first dot in the sequence.
          //
          size_t i (p);
          for (; i != 0 && s[i - 1] == '.'; --i) ;

          size_t sn (p - i + 1); // Sequence length.

          bool esc (sn != 1); // Escape the sequence.
          bool ext (sn == 1); // An extension dot, potentially.

          if (name)
          {
            if (i == n - 1)
              esc = true;

            if (ext && (i == 0 || path::traits_type::is_separator (s[i - 1])))
              ext = false;
          }

          if (esc)
            s.insert (p + 1, sn, '.'); // Double them.

          if (ext)
            r = true;

          p = i; // Position to the first dot in the sequence.
        }
        else if (path::traits_type::is_separator (c))
        {
          assert (name);
          break;
        }

        if (p == 0)
          break;
      }

      return r;
    };

    bool ed (escape (v, true /* name */));

    if (v.back () == '.') // Name had (before escaping) trailing dot.
    {
      assert (e && e->empty ());
    }
    else if (e)
    {
      // Separate the name and extension with the triple dots if the extension
      // contains potential extension dots.
      //
      string ext (*e);
      v += escape (ext, false /* name */) ?  "..." : ".";
      v += ext; // Empty or not.
    }
    else if (de && ed)
      v += "...";
  }

  // include()
  //
  // See var_include documentation for details on what's going on here.
  //
  include_type
  include_impl (action a,
                const target& t,
                const prerequisite& p,
                const target* m,
                lookup* rl)
  {
    context& ctx (t.ctx);

    include_type r (include_type::normal);
    {
      lookup l (p.vars[ctx.var_include]);

      if (l.defined ())
      {
        if (l->null)
        {
          // @@ TMP (added in 0.16.0).
          //
          warn << "null " << *ctx.var_include << " variable value specified "
               << "for prerequisite " << p <<
            info << "treated as undefined for backwards compatibility" <<
            info << "this warning will become error in the future";
        }
        else
        {
          const string& v (cast<string> (*l));

          if      (v == "false")   r = include_type::excluded;
          else if (v == "true")    r = include_type::normal;
          else if (v == "adhoc")   r = include_type::adhoc;
          else if (v == "posthoc") r = include_type::posthoc;
          else
            fail << "invalid " << *ctx.var_include << " variable value '"
                 << v << "' specified for prerequisite " << p;
        }
      }
    }

    // Handle operation-specific override (see var_include documentation
    // for details).
    //
    lookup l;
    optional<bool> r1; // Absent means something other than true|false.

    names storage;
    names_view ns;
    const variable* ovar (nullptr);

    if (r != include_type::excluded)
    {
      // Instead of going via potentially expensive target::base_scope(), use
      // the prerequisite's scope; while it may not be the same as the
      // targets's base scope, they must have the same root scope.
      //
      const scope& rs (*p.scope.root_scope ());

      ovar = rs.root_extra->operations[
        (a.outer ()
         ? ctx.current_outer_oif
         : ctx.current_inner_oif)->id].ovar;

      if (ovar != nullptr)
      {
        l = p.vars[*ovar];

        if (l.defined ())
        {
          if (l->null)
            fail << "null " << *ovar << " variable value specified for "
                 << "prerequisite " << p;

          // Maybe we should optimize this for the common cases (bool, path,
          // name)? But then again we don't expect many such overrides. Plus
          // will complicate the diagnostics below.
          //
          ns = reverse (*l, storage, true /* reduce */);

          if (ns.size () == 1)
          {
            const name& n (ns[0]);

            if (n.simple ())
            {
              const string& v (n.value);

              if (v == "false")
                r1 = false;
              else if (v == "true")
                r1 = true;
            }
          }

          if (r1 && !*r1)
            r = include_type::excluded;
        }
      }
    }

    // Call the meta-operation override, if any (currently used by dist).
    //
    if (r != include_type::normal || l)
    {
      if (auto f = ctx.current_mif->include)
        r = f (a, t, prerequisite_member {p, m}, r, l);
    }

    if (l)
    {
      if (rl != nullptr)
        *rl = l;
      else if (!r1)
      {
        // Note: we have to delay this until the meta-operation callback above
        // had a chance to override it.
        //
        fail << "unrecognized " << *ovar << " variable value '" << ns
             << "' specified for prerequisite " << p;
      }
    }

    return r;
  }

  // target_set
  //
  const target* target_set::
  find (const target_key& k, tracer& trace) const
  {
    bool load (ctx.phase == run_phase::load);

    slock sl (mutex_, defer_lock); if (!load) sl.lock ();
    map_type::const_iterator i (map_.find (k));

    if (i == map_.end ())
      return nullptr;

    const target& t (*i->second);
    optional<string>& ext (i->first.ext);

    if (ext != k.ext)
    {
      ulock ul; // Keep locked for trace.

      if (k.ext)
      {
        // To update the extension we have to re-lock for exclusive access.
        // Between us releasing the shared lock and acquiring unique the
        // extension could change and possibly a new target that matches the
        // key could be inserted. In this case we simply re-run find ().
        // Naturally, can't happen during load.
        //
        if (!load)
        {
          sl.unlock ();
          ul = ulock (mutex_);

          if (ext) // Someone set the extension.
          {
            ul.unlock ();
            return find (k, trace);
          }
        }
      }

      l5 ([&]{
          diag_record r (trace);
          r << "assuming target ";
          to_stream (r.os,
                     target_key {&t.type (), &t.dir, &t.out, &t.name, ext},
                     stream_verb_max); // Always print the extension.
          r << " is the same as the one with ";

          if (!k.ext)
            r << "unspecified extension";
          else if (k.ext->empty ())
            r << "no extension";
          else
            r << "extension " << *k.ext;
        });

      if (k.ext)
        ext = k.ext;
    }

    return &t;
  }

  pair<target&, ulock> target_set::
  insert_locked (const target_type& tt,
                 dir_path dir,
                 dir_path out,
                 string name,
                 optional<string> ext,
                 target_decl decl,
                 tracer& trace,
                 bool skip_find,
                 bool need_lock)
  {
    target_key tk {&tt, &dir, &out, &name, move (ext)};
    target* t (skip_find ? nullptr : const_cast<target*> (find (tk, trace)));

    if (t == nullptr)
    {
      // We sometimes call insert() even if we expect to find an existing
      // target in order to keep the same code (see cc/search_library()).
      //
      assert (ctx.phase != run_phase::execute);

      optional<string> e (
        tt.fixed_extension != nullptr
        ? string (tt.fixed_extension (tk, nullptr /* root scope */))
        : move (tk.ext));

      t = tt.factory (ctx, tt, move (dir), move (out), move (name));

      // Re-lock for exclusive access. In the meantime, someone could have
      // inserted this target so emplace() below could return false, in which
      // case we proceed pretty much like find() except already under the
      // exclusive lock.
      //
      ulock ul (mutex_, defer_lock);
      if (ctx.phase != run_phase::load || need_lock)
        ul.lock ();

      auto p (map_.emplace (target_key {&tt, &t->dir, &t->out, &t->name, e},
                            unique_ptr<target> (t)));

      map_type::iterator i (p.first);

      if (p.second)
      {
#if 0
        {
          size_t n (map_.bucket_count ());
          if (n > buckets_)
          {
            text << "target_set buckets: " << buckets_ << " -> " << n
                 << " (" << map_.size () << ")";
            buckets_ = n;
          }
        }
#endif

        t->ext_ = &i->first.ext;
        t->decl = decl;
        t->state.inner.target_ = t;
        t->state.outer.target_ = t;
        t->state.inner.vars.target_ = t;
        t->state.outer.vars.target_ = t;

        if (ctx.phase != run_phase::load && !need_lock)
          ul.unlock ();

        return pair<target&, ulock> (*t, move (ul));
      }

      // The "tail" of find().
      //
      t = i->second.get ();
      optional<string>& ext (i->first.ext);

      if (ext != e)
      {
        l5 ([&]{
            diag_record r (trace);
            r << "assuming target ";
            to_stream (
              r.os,
              target_key {&t->type (), &t->dir, &t->out, &t->name, ext},
              stream_verb_max); // Always print the extension.
            r << " is the same as the one with ";

            if (!e)
              r << "unspecified extension";
            else if (e->empty ())
              r << "no extension";
            else
              r << "extension " << *e;
          });

        if (e)
          ext = e;
      }

      // Fall through (continue as if the first find() returned this target).
    }

    // Without resorting to something like atomic we can only upgrade the
    // declaration to real (which is expected to only happen during the load
    // phase).
    //
    if (decl == target_decl::real)
    {
      assert (ctx.phase == run_phase::load);

      if (t->decl != target_decl::real)
        t->decl = decl;
    }

    return pair<target&, ulock> (*t, ulock ());
  }

  static const optional<string> unknown_ext ("?");

  bool
  to_stream (ostream& os,
             const target_key& k,
             optional<stream_verbosity> osv,
             bool name_only)
  {
    // Note: similar code in print_diag_impl(vector<target_key>).

    stream_verbosity sv (osv ? *osv : stream_verb (os));
    uint16_t dv (sv.path);
    uint16_t ev (sv.extension);

    // If the name is empty, then we want to print the last component of the
    // directory inside {}, e.g., dir{bar/}, not bar/dir{}.
    //
    bool n (!k.name->empty ());

    const target_type& tt (*k.type);

    dir_path rds; // Storage.
    if (!name_only)
    {
      // Note: relative() returns empty for './'.
      //
      if (dv < 1)
        rds = relative (*k.dir);

      const dir_path& rd (dv < 1 ? rds : *k.dir);    // Relative.
      const dir_path& pd (n ? rd : rd.directory ()); // Parent.

      if (!pd.empty ())
      {
        if (dv < 1)
          os << diag_relative (pd);
        else
          to_stream (os, pd, true /* representation */);
      }

      os << tt.name << '{';
    }

    if (n)
    {
      const optional<string>* ext (nullptr); // NULL or present.

      // If the extension derivation functions are NULL, then it means this
      // target type doesn't use extensions.
      //
      if (tt.fixed_extension != nullptr || tt.default_extension != nullptr)
      {
        // For verbosity level 0 we don't print the extension. For 1 we print
        // it if there is one. For 2 we print 'foo.?' if it hasn't yet been
        // assigned and 'foo.' if it is assigned as "no extension" (empty).
        //
        if (ev > 0 && (ev > 1 || (k.ext && !k.ext->empty ())))
        {
          ext = k.ext ? &k.ext : &unknown_ext;
        }
      }
      else
        assert (!k.ext || k.ext->empty ()); // Unspecified or none.

      // Escape dots in the name/extension to resolve potential ambiguity.
      //
      if (k.name->find ('.') == string::npos &&
          (ext == nullptr || (*ext)->find ('.') == string::npos))
      {
        os << *k.name;

        if (ext != nullptr)
          os << '.' << **ext;
      }
      else
      {
        string n (*k.name);
        target::combine_name (n,
                              ext != nullptr ? *ext : nullopt_string,
                              false /* default_extension */);
        os << n;
      }
    }
    else
    {
      if (name_only && dv < 1) // Already done if !name_only.
        rds = relative (*k.dir);

      const dir_path& rd (dv < 1 ? rds : *k.dir);

      to_stream (os,
                 rd.empty () ? dir_path (".") : rd.leaf (),
                 true /* representation */);
    }

    if (!name_only)
    {
      os << '}';

      // If this target is from src, print its out.
      //
      if (!k.out->empty ())
      {
        if (dv < 1)
        {
          // Don't print '@./'.
          //
          const string& o (diag_relative (*k.out, false));

          if (!o.empty ())
            os << '@' << o;
        }
        else
          os << '@' << *k.out;
      }
    }

    return n; // Regular if we had the name.
  }

  ostream&
  operator<< (ostream& os, const target_key& k)
  {
    if (auto p = k.type->print)
      p (os, k, false /* name_only */);
    else
      to_stream (os, k, stream_verb (os));

    return os;
  }

  // mtime_target
  //
  timestamp mtime_target::
  mtime () const
  {
    // Figure out from which target we should get the value.
    //
    const mtime_target* t (this);

    switch (ctx.phase)
    {
    case run_phase::load: break;
    case run_phase::match:
      {
        // Similar logic to target::matched().
        //
        const opstate& s (state[action () /* inner */]);

        // Note: use acquire for group_state().
        //
        size_t c (s.task_count.load (memory_order_acquire));
        size_t b (ctx.count_base ()); // Note: cannot do (c - b)!

        if (!(c == (b + offset_applied)  ||
              c == (b + offset_executed) ||
              (c >= (b + offset_busy)    &&
               s.match_extra.cur_options_.load (memory_order_relaxed) != 0)))
          break;
      }
      // Fall through.
    case run_phase::execute:
      {
        if (group_state (action () /* inner */))
          t = &group->as<mtime_target> ();

        break;
      }
    }

    return timestamp (duration (t->mtime_.load (memory_order_consume)));
  }

  // path_target
  //
  const string* path_target::
  derive_extension (bool search, const char* de)
  {
    // See also search_existing_file() if updating anything here.

    // Should be no default extension if searching.
    //
    assert (!search || de == nullptr);

    // The target should use extensions and they should not be fixed.
    //
    assert (de == nullptr || type ().default_extension != nullptr);

    if (const string* p = ext ())
      // Note that returning by reference is now MT-safe since once the
      // extension is specified, it is immutable.
      //
      return p;
    else
    {
      optional<string> e;

      // If the target type has the default extension function then try that
      // first. The reason for preferring it over what's been provided by the
      // caller is that this function will often use the 'extension' variable
      // which the user can use to override extensions. But since we pass the
      // provided default extension, the target type can override this logic
      // (see the exe{} target type for a use case).
      //
      if (auto f = type ().default_extension)
        e = f (key (), base_scope (), de, search);

      if (!e)
      {
        if (de != nullptr)
          e = de;
        else
        {
          if (search)
            return nullptr;

          fail << "no default extension for target " << *this << endf;
        }
      }

      return &ext (move (*e));
    }
  }

  const path& path_target::
  derive_path (const char* de, const char* np, const char* ns, const char* ee)
  {
    return derive_path_with_extension (derive_extension (de), np, ns, ee);
  }

  const path& path_target::
  derive_path_with_extension (const string& e,
                              const char* np,
                              const char* ns,
                              const char* ee)
  {
    path_type p (dir);

    if (np == nullptr || np[0] == '\0')
      p /= name;
    else
    {
      p /= np;
      p += name;
    }

    if (ns != nullptr)
      p += ns;

    return derive_path_with_extension (move (p), e, ee);
  }

  const path& path_target::
  derive_path (path_type p, const char* de, const char* ee)
  {
    return derive_path_with_extension (move (p), derive_extension (de), ee);
  }

  const path& path_target::
  derive_path_with_extension (path_type p, const string& e, const char* ee)
  {
    if (!e.empty ())
    {
      p += '.';
      p += e;
    }

    if (ee != nullptr)
    {
      p += '.';
      p += ee;
    }

    return path (move (p));
  }

  // Search functions.
  //

  const target*
  target_search (context& ctx, const target*, const prerequisite_key& pk)
  {
    // The default behavior is to look for an existing target in the
    // prerequisite's directory scope.
    //
    return search_existing_target (ctx, pk, true /* out_only */);
  }

  const target*
  file_search (context& ctx, const target* t, const prerequisite_key& pk)
  {
    // First see if there is an existing target in the out or src tree.
    //
    if (const target* e = search_existing_target (ctx,
                                                  pk,
                                                  false /* out_only */))
      return e;

    // Then look for an existing file in the src tree.
    //
    return t != nullptr ? search_existing_file (ctx, pk) : nullptr;
  }

  extern const char target_extension_none_[] = "";

  const char*
  target_extension_none (const target_key& k, const scope* s)
  {
    return target_extension_fix<target_extension_none_> (k, s);
  }

  const char*
  target_extension_must (const target_key& tk, const scope*)
  {
    if (!tk.ext)
      fail << tk.type->name << " target " << tk << " must include extension";

    return tk.ext->c_str ();
  }

  bool
  target_print_0_ext_verb (ostream& os, const target_key& k, bool no)
  {
    stream_verbosity sv (stream_verb (os));
    if (sv.extension == 1) sv.extension = 0; // Remap 1 to 0.
    return to_stream (os, k, sv, no);
  }

  bool
  target_print_1_ext_verb (ostream& os, const target_key& k, bool no)
  {
    stream_verbosity sv (stream_verb (os));
    if (sv.extension == 0) sv.extension = 1; // Remap 0 to 1.
    return to_stream (os, k, sv, no);
  }

  // type info
  //

  const target_type target::static_type
  {
    "target",
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &target_search,
    target_type::flag::none,
  };

  const target_type mtime_target::static_type
  {
    "mtime_target",
    &target::static_type,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &target_search,
    target_type::flag::none
  };

  const target_type path_target::static_type
  {
    "path_target",
    &mtime_target::static_type,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &target_search,
    target_type::flag::none
  };

  const target_type file::static_type
  {
    "file",
    &path_target::static_type,
    &target_factory<file>,
    &target_extension_none,
    nullptr, /* default_extension */
    nullptr, /* pattern */
    &target_print_1_ext_verb, // Print extension even at verbosity level 0.
    &file_search,
    target_type::flag::none
  };

  // group
  //
  group_view group::
  group_members (action a) const
  {
    if (members_on == 0) // Not yet discovered.
      return group_view {nullptr, 0};

    // Members discovered during anything other than perform_update are only
    // good for that operation. For example, we only return the static members
    // ("representative sample") for perform_configure.
    //
    // We also re-discover the members on each update and clean not to
    // overcomplicate the already twisted adhoc_buildscript_rule::apply()
    // logic.
    //
    if (members_on != ctx.current_on)
    {
      if (members_action != perform_update_id ||
          a == perform_update_id ||
          a == perform_clean_id)
        return group_view {nullptr, 0};
    }

    // Note that we may have no members (e.g., perform_configure and there are
    // no static members). However, whether std::vector returns a non-NULL
    // pointer in this case is undefined.
    //
    size_t n (members.size ());
    return group_view {
      n != 0
      ? members.data ()
      : reinterpret_cast<const target* const*> (this),
      n};
  }

  const target_type group::static_type
  {
    "group",
    &mtime_target::static_type,
    &target_factory<group>,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &target_search,
    //
    // Note that the dyn_members semantics is used not only to handle
    // depdb-dyndep --dyn-target, but also pattern rule-static members.
    //
    target_type::flag::group | target_type::flag::dyn_members
  };

  // alias
  //
  static const target*
  alias_search (context& ctx, const target* t, const prerequisite_key& pk)
  {
    // For an alias we don't want to silently create a target since it will do
    // nothing and it most likely not what the user intended (but omit this
    // check when searching for an existing target since presumably a new one
    // won't be created in this case).
    //
    // But, allowing implied aliases seems harmless since all the alias does
    // is pull its prerequisites. And they are handy to use as metadata
    // carriers.
    //
    // Doesn't feel like an alias in the src tree makes much sense.
    //
    const target* e (search_existing_target (ctx, pk, true /* out_only */));

    if ((e == nullptr ||
         !(operator>= (e->decl, target_decl::implied))) && t != nullptr)
      fail << "no explicit target for " << pk;

    return e;
  }

  const target_type alias::static_type
  {
    "alias",
    &target::static_type,
    &target_factory<alias>,
    nullptr, // Extension not used.
    nullptr,
    nullptr,
    nullptr,
    &alias_search,
    target_type::flag::none
  };

  // dir
  //
  bool dir::
  check_implied (const scope& rs, const dir_path& d)
  {
    try
    {
      for (const dir_entry& e: dir_iterator (d, dir_iterator::detect_dangling))
      {
        switch (e.type ())
        {
        case entry_type::directory:
          {
            if (check_implied (rs, d / path_cast<dir_path> (e.path ())))
              return true;

            break;
          }
        case entry_type::regular:
          {
            if (e.path () == rs.root_extra->buildfile_file)
              return true;

            break;
          }
        case entry_type::unknown:
          {
            bool sl (e.ltype () == entry_type::symlink);

            warn << "skipping "
                 << (sl ? "dangling symlink" : "inaccessible entry") << ' '
                 << d / e.path ();

            break;
          }
        default:
          break;
        }
      }
    }
    catch (const system_error& e)
    {
      fail << "unable to iterate over " << d << ": " << e << endf;
    }

    return false;
  }

  prerequisites dir::
  collect_implied (const scope& bs)
  {
    prerequisites_type r;
    const dir_path& d (bs.src_path ());

    try
    {
      for (const dir_entry& e: dir_iterator (d, dir_iterator::detect_dangling))
      {
        if (e.type () == entry_type::directory)
        {
          r.push_back (
            prerequisite (dir::static_type,
                          dir_path (e.path ().representation ()), // Relative.
                          dir_path (), // In the out tree.
                          string (),
                          nullopt,
                          bs));
        }
        else if (e.type () == entry_type::unknown)
        {
          bool sl (e.ltype () == entry_type::symlink);

          warn << "skipping "
               << (sl ? "dangling symlink" : "inaccessible entry") << ' '
               << d / e.path ();
        }
      }
    }
    catch (const system_error& e)
    {
      fail << "unable to iterate over " << d << ": " << e;
    }

    return r;
  }

  static const target*
  dir_search (context& ctx, const target* t, const prerequisite_key& pk)
  {
    tracer trace ("dir_search");

    // The first step is like in alias_search(): looks for an existing target
    // (but unlike alias, no implied, think `test/: install=false`).
    //
    // Likewise, dir{} in the src tree doesn't make much sense.
    //
    const target* e (search_existing_target (ctx, pk, true /* out_only */));

    if (e != nullptr && e->decl == target_decl::real)
      return e;

    // The search for an existing target can also be done during execute so
    // none of the below code applied. Note: return implied instead of NULL
    // (to be consistent with search_new(), for example).
    //
    if (t == nullptr)
      return e;

    // If not found (or is implied), then try to load the corresponding
    // buildfile (which would normally define this target). Failed that, see
    // if we can assume an implied buildfile which would be equivalent to:
    //
    // ./: */
    //
    const scope& s (*pk.scope);
    const dir_path& d (*pk.tk.dir);

    // Note: this code is a custom version of parser::parse_include().

    // Calculate the new out_base. If the directory is absolute then we assume
    // it is already normalized.
    //
    dir_path out_base (d.relative ()
                       ? (s.out_path () / d).normalize ()
                       : d);

    // In our world modifications to the scope structure during search & match
    // should be "pure append" in the sense that they should not affect any
    // existing targets that have already been searched & matched.
    //
    // A straightforward way to enforce this is to not allow any existing
    // targets to be inside any newly created scopes (except, perhaps for the
    // directory target itself which we know hasn't been searched yet). This,
    // however, is not that straightforward to implement: we would need to
    // keep a directory prefix map for all the targets (e.g., in target_set).
    // Also, a buildfile could load from a directory that is not a
    // subdirectory of out_base. So for now we just assume that this is so.
    // And so it is.
    //
    bool retest (false);

    assert (ctx.phase == run_phase::match);
    {
      // Switch the phase to load.
      //
      phase_switch ps (ctx, run_phase::load);

      // This is subtle: while we were fussing around another thread may have
      // loaded the buildfile. So re-test now that we are in an exclusive
      // phase.
      //
      if (e == nullptr)
        e = search_existing_target (ctx, pk, true);

      if (e != nullptr && e->decl == target_decl::real)
        retest = true;
      else
      {
        // Ok, no luck, switch the scope.
        //
        // Note that we don't need to do anything for the project's
        // environment: source_once() will take care of it itself and
        // search_implied() is not affected.
        //
        pair<scope&, scope*> sp (
          switch_scope (*s.rw ().root_scope (), out_base));

        if (sp.second != nullptr) // Ignore scopes out of any project.
        {
          scope& base (sp.first);
          scope& root (*sp.second);

          const dir_path& src_base (base.src_path ());

          path bf (src_base / root.root_extra->buildfile_file);

          if (exists (bf))
          {
            l5 ([&]{trace << "loading buildfile " << bf << " for " << pk;});
            retest = source_once (root, base, bf);
          }
          else if (exists (src_base))
          {
            e = dir::search_implied (base, pk, trace);
            retest = (e != nullptr);
          }
        }
      }
    }

    assert (ctx.phase == run_phase::match);

    // If we loaded/implied the buildfile, examine the target again.
    //
    if (retest)
    {
      if (e == nullptr)
        e = search_existing_target (ctx, pk, true);

      if (e != nullptr && e->decl == target_decl::real)
        return e;
    }

    fail << "no explicit target for " << pk << endf;
  }

  static bool
  dir_pattern (const target_type&,
               const scope&,
               string& v,
               optional<string>&,
               const location&,
               bool r)
  {
    // Add/strip trailing directory separator unless already there.
    //
    bool d (path::traits_type::is_separator (v.back ()));

    if (r)
    {
      assert (d);
      v.resize (v.size () - 1);
    }
    else if (!d)
    {
      v += path::traits_type::directory_separator;
      return true;
    }

    return false;
  }

  const target_type dir::static_type
  {
    "dir",
    &alias::static_type,
    &target_factory<dir>,
    nullptr,              // Extension not used.
    nullptr,
    &dir_pattern,
    nullptr,
    &dir_search,
    target_type::flag::none
  };

  const target_type fsdir::static_type
  {
    "fsdir",
    &target::static_type,
    &target_factory<fsdir>,
    nullptr,              // Extension not used.
    nullptr,
    &dir_pattern,
    nullptr,
    &target_search,
    target_type::flag::none
  };

  static optional<string>
  exe_target_extension (const target_key&,
                        const scope&,
                        const char* e,
                        bool search)
  {
    // If we are searching for an executable that is not a target, then use
    // the host machine executable extension. Otherwise, if this is a target,
    // then we expect the rule to supply the target machine extension. But if
    // it doesn't, then fallback to no extension (e.g., a script).
    //
    return string (!search
                   ? (e != nullptr ? e : "")
                   :
#ifdef _WIN32
                   "exe"
#else
                   ""
#endif
    );
  }

#ifdef _WIN32
  static bool
  exe_target_pattern (const target_type&,
                      const scope&,
                      string& v,
                      optional<string>& e,
                      const location& l,
                      bool r)
  {
    if (r)
    {
      assert (e);
      e = nullopt;
    }
    else
    {
      e = target::split_name (v, l);

      if (!e)
      {
        e = "exe";
        return true;
      }
    }

    return false;
  }
#endif

  const target_type exe::static_type
  {
    "exe",
    &file::static_type,
    &target_factory<exe>,
    nullptr, /* fixed_extension */
    &exe_target_extension,
#ifdef _WIN32
    &exe_target_pattern,
#else
    nullptr,
#endif
    nullptr,
    &file_search, // Note: can also be a script in src.
    target_type::flag::none
  };

  static const char*
  buildfile_target_extension (const target_key& tk, const scope* root)
  {
    // If the name is the special 'buildfile', then there is no extension,
    // otherwise it is 'build' (or 'build2file' and 'build2' in the
    // alternative naming scheme).

    // Let's try hard not to need the root scope by trusting the extensions
    // we were given.
    //
    // BTW, one way to get rid of all this root scope complication is to
    // always require explicit extension specification for buildfiles. Since
    // they are hardly ever mentioned explicitly, this should probably be ok.
    //
    if (tk.ext)
      return tk.ext->c_str ();

    if (root == nullptr)
    {
      // The same login as in target::root_scope().
      //
      // Note: we are guaranteed the scope is never NULL for prerequisites
      // (where out/dir could be relative and none of this will work).
      //
      // @@ CTX TODO
#if 0
      root = scopes.find (tk.out->empty () ? *tk.dir : *tk.out).root_scope ();
#endif

      if (root == nullptr || root->root_extra == nullptr)
        fail << "unable to determine extension for buildfile target " << tk;
    }

    return *tk.name == root->root_extra->buildfile_file.string ()
      ? ""
      : root->root_extra->build_ext.c_str ();
  }

  static bool
  buildfile_target_pattern (const target_type&,
                            const scope& base,
                            string& v,
                            optional<string>& e,
                            const location& l,
                            bool r)
  {
    if (r)
    {
      assert (e);
      e = nullopt;
    }
    else
    {
      e = target::split_name (v, l);

      if (!e)
      {
        const scope* root (base.root_scope ());

        if (root == nullptr || root->root_extra == nullptr)
          fail (l) << "unable to determine extension for buildfile pattern";

        if (v != root->root_extra->buildfile_file.string ())
        {
          e = root->root_extra->build_ext;
          return true;
        }
      }
    }

    return false;
  }

  const target_type buildfile::static_type
  {
    "buildfile",
    &file::static_type,
    &target_factory<buildfile>,
    &buildfile_target_extension,
    nullptr, /* default_extension */
    &buildfile_target_pattern,
    nullptr,
    &file_search,
    target_type::flag::none
  };

  static const char*
  buildscript_target_extension (const target_key& tk, const scope*)
  {
    // If the name is special 'buildscript', then there is no extension,
    // otherwise it is .buildscript.
    //
    return *tk.name == "buildscript" ? "" : "buildscript";
  }

  static bool
  buildscript_target_pattern (const target_type&,
                             const scope&,
                             string& v,
                             optional<string>& e,
                             const location& l,
                             bool r)
  {
    if (r)
    {
      assert (e);
      e = nullopt;
    }
    else
    {
      e = target::split_name (v, l);

      if (!e && v != "buildscript")
      {
        e = "buildscript";
        return true;
      }
    }

    return false;
  }

  const target_type buildscript::static_type
  {
    "buildscript",
    &file::static_type,
    &target_factory<buildscript>,
    &buildscript_target_extension,
    nullptr,  /* default_extension */
    &buildscript_target_pattern,
    nullptr,
    &file_search,
    target_type::flag::none
  };

  const target_type doc::static_type
  {
    "doc",
    &file::static_type,
    &target_factory<doc>,
    &target_extension_none,              // Same as file (no extension).
    nullptr, /* default_extension */
    nullptr, /* pattern */               // Same as file.
    &target_print_1_ext_verb,            // Same as file.
    &file_search,
    target_type::flag::none
  };

  const target_type legal::static_type
  {
    "legal",
    &doc::static_type,
    &target_factory<legal>,
    &target_extension_none,              // Same as file (no extension).
    nullptr, /* default_extension */
    nullptr, /* pattern */               // Same as file.
    &target_print_1_ext_verb,            // Same as file.
    &file_search,
    target_type::flag::none
  };

  const target_type man::static_type
  {
    "man",
    &doc::static_type,
    &target_factory<man>,
    &target_extension_must,   // Should be specified explicitly.
    nullptr, /* default_extension */
    nullptr,
    &target_print_1_ext_verb, // Print extension even at verbosity level 0.
    &file_search,
    target_type::flag::none
  };

  extern const char man1_ext[] = "1"; // VC14 rejects constexpr.

  const target_type man1::static_type
  {
    "man1",
    &man::static_type,
    &target_factory<man1>,
    &target_extension_fix<man1_ext>,
    nullptr,  /* default_extension */
    &target_pattern_fix<man1_ext>,
    &target_print_0_ext_verb, // Fixed extension, no use printing.
    &file_search,
    target_type::flag::none
  };

  static const char*
  manifest_target_extension (const target_key& tk, const scope*)
  {
    // If the name is special 'manifest', then there is no extension,
    // otherwise it is .manifest.
    //
    return *tk.name == "manifest" ? "" : "manifest";
  }

  static bool
  manifest_target_pattern (const target_type&,
                           const scope&,
                           string& v,
                           optional<string>& e,
                           const location& l,
                           bool r)
  {
    if (r)
    {
      assert (e);
      e = nullopt;
    }
    else
    {
      e = target::split_name (v, l);

      if (!e && v != "manifest")
      {
        e = "manifest";
        return true;
      }
    }

    return false;
  }

  const target_type manifest::static_type
  {
    "manifest",
    &doc::static_type,
    &target_factory<manifest>,
    &manifest_target_extension,
    nullptr, /* default_extension */
    &manifest_target_pattern,
    nullptr,
    &file_search,
    target_type::flag::none
  };
}
