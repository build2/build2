// file      : build2/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/target>

#include <butl/filesystem> // file_mtime()

#include <build2/file>
#include <build2/scope>
#include <build2/search>
#include <build2/algorithm>
#include <build2/filesystem>
#include <build2/diagnostics>

using namespace std;
using namespace butl;

namespace build2
{
  // target_type
  //
  bool target_type::
  is_a_base (const target_type& tt) const
  {
    for (const target_type* b (base); b != nullptr; b = b->base)
      if (*b == tt)
        return true;

    return false;
  }

  // target_state
  //
  static const char* const target_state_[] =
  {
    "unknown",
    "unchanged",
    "postponed",
    "busy",
    "changed",
    "failed",
    "group"
  };

  ostream&
  operator<< (ostream& os, target_state ts)
  {
    return os << target_state_[static_cast<uint8_t> (ts)];
  }

  // recipe
  //
  const recipe empty_recipe;
  const recipe noop_recipe (&noop_action);
  const recipe default_recipe (&default_action);
  const recipe group_recipe (&group_action);

  // target
  //
  const target::prerequisites_type target::empty_prerequisites_;

  target::
  ~target ()
  {
    clear_data ();
  }

  const string& target::
  ext (string v)
  {
    ulock l (targets.mutex_);

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
  group_members (action_type) const
  {
    assert (false); // Not a group or doesn't expose its members.
    return group_view {nullptr, 0};
  }

  const scope& target::
  base_scope () const
  {
    // If this target is from the src tree, use its out directory to find
    // the scope.
    //
    return scopes.find (out_dir ());
  }

  const scope& target::
  root_scope () const
  {
    // This is tricky to cache so we do the lookup for now.
    //
    const scope* r (base_scope ().root_scope ());
    assert (r != nullptr);
    return *r;
  }

  target_state target::
  state (action_type a) const
  {
    assert (phase == run_phase::match);

    // The tricky aspect here is that we my be re-matching the target for
    // another (overriding action). Since it is only valid to call this
    // function after the target has been matched (for this action), we assume
    // that if the target is busy, then it is being overriden (note that it
    // cannot be being executed since we are in the match phase).
    //
    // But that's not the end of it: in case of a re-match the task count
    // might have been reset to, say, applied. The only way to know for sure
    // that there isn't another match "underneath" is to compare actions. But
    // that can only be done safely if we lock the target. At least we will be
    // quick (and we don't need to wait since if it's busy, we know it is a
    // re-match). This logic is similar to lock_impl().
    //
    size_t b (target::count_base ());
    size_t e (task_count.load (memory_order_acquire));

    size_t exec (b + target::offset_executed);
    size_t lock (b + target::offset_locked);
    size_t busy (b + target::offset_busy);

    for (;;)
    {
      for (; e == lock; e = task_count.load (memory_order_acquire))
        this_thread::yield ();

      if (e >= busy)
        return target_state::unchanged; // Override in progress.

      // Unlike lock_impl(), we are only called after being matched for this
      // action so if we see executed, then it means executed for this action
      // (or noop).
      //
      if (e == exec)
        return group_state () ? group->state_ : state_;

      // Try to grab the spin-lock.
      //
      if (task_count.compare_exchange_strong (
            e,
            lock,
            memory_order_acq_rel,  // Synchronize on success.
            memory_order_acquire)) // Synchronize on failure.
      {
        break;
      }

      // Task count changed, try again.
    }

    // We have the spin-lock. Quickly get the matched action and unlock.
    //
    action_type ma (action);
    bool failed (state_ == target_state::failed);
    task_count.store (e, memory_order_release);

    if (ma > a) // Overriden.
      return failed ? target_state::failed: target_state::unchanged;

    // Otherwise we should have a matched target.
    //
    assert (ma == a && (e == b + target::offset_applied || e == exec));

    return group_state () ? group->state_ : state_;
  }

  pair<lookup, size_t> target::
  find_original (const variable& var, bool target_only) const
  {
    pair<lookup, size_t> r (lookup (), 0);

    ++r.second;
    if (auto p = vars.find (var))
      r.first = lookup (p, &vars);

    const target* g (nullptr);

    if (!r.first)
    {
      ++r.second;

      // Skip looking up in the ad hoc group, which is semantically the
      // first/primary member.
      //
      if ((g = group == nullptr
           ? nullptr
           : group->adhoc_group () ? group->group : group))
      {
        if (auto p = g->vars.find (var))
          r.first = lookup (p, &g->vars);
      }
    }

    // Delegate to scope's find_original().
    //
    if (!r.first)
    {
      if (!target_only)
      {
        auto p (base_scope ().find_original (
                  var,
                  &type (),
                  &name,
                  g != nullptr ? &g->type () : nullptr,
                  g != nullptr ? &g->name : nullptr));

        r.first = move (p.first);
        r.second = r.first ? r.second + p.second : p.second;
      }
      else
        r.second = size_t (~0);
    }

    return r;
  }

  value& target::
  append (const variable& var)
  {
    // Note that here we want the original value without any overrides
    // applied.
    //
    lookup l (find_original (var).first);

    if (l.defined () && l.belongs (*this)) // Existing var in this target.
      return vars.modify (l); // Ok since this is original.

    value& r (assign (var)); // NULL.

    if (l.defined ())
      r = *l; // Copy value (and type) from the outer scope.

    return r;
  }

  // target_set
  //
  target_set targets;

  const target* target_set::
  find (const target_key& k, tracer& trace) const
  {
    slock sl (mutex_);
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
        //
        sl.unlock ();
        ul = ulock (mutex_);

        if (ext) // Someone set the extension.
        {
          ul.unlock ();
          return find (k, trace);
        }

        ext = k.ext;
      }

      l5 ([&]{
          diag_record r (trace);
          r << "assuming target ";
          to_stream (r.os,
                     target_key {&t.type (), &t.dir, &t.out, &t.name, nullopt},
                     0); // Don't print the extension.
          r << " is the same as the one with ";

          if (!k.ext)
            r << "unspecified extension";
          else if (k.ext->empty ())
            r << "no extension";
          else
            r << "extension " << *k.ext;
        });
    }

    return &t;
  }

  pair<target&, ulock> target_set::
  insert_locked (const target_type& tt,
                 dir_path dir,
                 dir_path out,
                 string name,
                 optional<string> ext,
                 bool implied,
                 tracer& trace)
  {
    target_key tk {&tt, &dir, &out, &name, move (ext)};
    target* t (const_cast<target*> (find (tk, trace)));

    if (t == nullptr)
    {
      // We sometimes call insert() even if we expect to find an existing
      // target in order to keep the same code (see cc/search_library()).
      //
      assert (phase != run_phase::execute);

      pair<target*, optional<string>> te (
        tt.factory (
          tt, move (dir), move (out), move (name), move (tk.ext)));

      t = te.first;

      // Re-lock for exclusive access. In the meantime, someone could have
      // inserted this target so emplace() below could return false, in which
      // case we proceed pretty much like find() except already under the
      // exclusive lock.
      //
      ulock ul (mutex_);

      auto p (
        map_.emplace (
          target_key {&tt, &t->dir, &t->out, &t->name, te.second},
          unique_ptr<target> (te.first)));

      map_type::iterator i (p.first);

      if (p.second)
      {
        t->ext_ = &i->first.ext;
        t->implied = implied;
        return pair<target&, ulock> (*t, move (ul));
      }

      // The "tail" of find().
      //
      t = i->second.get ();
      optional<string>& ext (i->first.ext);

      if (ext != te.second)
      {
        if (te.second)
          ext = te.second;

        l5 ([&]{
            diag_record r (trace);
            r << "assuming target ";
            to_stream (
              r.os,
              target_key {&t->type (), &t->dir, &t->out, &t->name, nullopt},
              0); // Don't print the extension.
            r << " is the same as the one with ";

            if (!te.second)
              r << "unspecified extension";
            else if (te.second->empty ())
              r << "no extension";
            else
              r << "extension " << *te.second;
          });
      }

      // Fall through (continue as if the first find() returned this target).
    }

    if (!implied)
    {
      // The implied flag can only be cleared during the load phase.
      //
      assert (phase == run_phase::load);

      // Clear the implied flag.
      //
      if (t->implied)
        t->implied = false;
    }

    return pair<target&, ulock> (*t, ulock ());
  }

  ostream&
  to_stream (ostream& os, const target_key& k, uint16_t ev)
  {
    // If the name is empty, then we want to print the directory
    // inside {}, e.g., dir{bar/}, not bar/dir{}.
    //
    bool n (!k.name->empty ());

    if (n)
    {
      // Avoid printing './' in './{...}'
      //
      if (stream_verb (os) < 2)
        os << diag_relative (*k.dir, false);
      else
        os << *k.dir;
    }

    os << k.type->name << '{';

    if (n)
    {
      os << *k.name;

      // If the extension derivation function is NULL, then it means this
      // target type doesn't use extensions.
      //
      if (k.type->extension != nullptr)
      {
        // For verbosity level 0 we don't print the extension. For 1 we print
        // it if there is one. For 2 we print 'foo.?' if it hasn't yet been
        // assigned and 'foo.' if it is assigned as "no extension" (empty).
        //
        if (ev > 0 && (ev > 1 || (k.ext && !k.ext->empty ())))
        {
          os << '.' << (k.ext ? *k.ext : "?");
        }
      }
      else
        assert (!k.ext);
    }
    else
      os << *k.dir;

    os << '}';

    // If this target is from src, print its out.
    //
    if (!k.out->empty ())
    {
      if (stream_verb (os) < 2)
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

    return os;
  }

  ostream&
  operator<< (ostream& os, const target_key& k)
  {
    if (auto p = k.type->print)
      p (os, k);
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

    switch (phase)
    {
    case run_phase::load: break;
    case run_phase::match:
      {
        // Similar logic to state(action) except here we don't distinguish
        // between original/overridden actions (an overridable action is by
        // definition a noop and should never need to query the mtime).
        //
        size_t c (task_count.load (memory_order_acquire)); // For group_state()

        // Wait out the spin lock to get the meaningful count.
        //
        for (size_t lock (target::count_locked ());
             c == lock;
             c = task_count.load (memory_order_acquire))
          this_thread::yield ();

        if (c != target::count_applied () && c != target::count_executed ())
          break;

        // Fall through.
      }
    case run_phase::execute:
      {
        if (group_state ())
          t = &group->as<mtime_target> ();

        break;
      }
    }

    return timestamp (duration (t->mtime_.load (memory_order_consume)));
  }

  // path_target
  //
  const string& path_target::
  derive_extension (const char* de, bool search)
  {
    // See also search_existing_file() if updating anything here.
    //
    assert (de == nullptr || type ().extension != nullptr);

    if (const string* p = ext ())
      // Note that returning by reference is now MT-safe since once the
      // extension is specified, it is immutable.
      //
      return *p;
    else
    {
      optional<string> e;

      // If the target type has the extension function then try that first.
      // The reason for preferring it over what's been provided by the caller
      // is that this function will often use the 'extension' variable which
      // the user can use to override extensions.
      //
      if (auto f = type ().extension)
        e = f (key (), base_scope (), search);

      if (!e)
      {
        if (de != nullptr)
          e = de;
        else
          fail << "no default extension for target " << *this;
      }

      return ext (move (*e));
    }
  }

  const path& path_target::
  derive_path (const char* de, const char* np, const char* ns)
  {
    path_type p (dir);

    if (np == nullptr)
      p /= name;
    else
    {
      p /= np;
      p += name;
    }

    if (ns != nullptr)
      p += ns;

    return derive_path (move (p), de);
  }

  const path& path_target::
  derive_path (path_type p, const char* de)
  {
    // Derive and add the extension if any.
    //
    {
      const string& e (derive_extension (de));

      if (!e.empty ())
      {
        p += '.';
        p += e;
      }
    }

    path (move (p));
    return path_;
  }

  // Search functions.
  //

  const target*
  target_search (const target&, const prerequisite_key& pk)
  {
    // The default behavior is to look for an existing target in the
    // prerequisite's directory scope.
    //
    return search_existing_target (pk);
  }

  const target*
  file_search (const target&, const prerequisite_key& pk)
  {
    // First see if there is an existing target.
    //
    if (const target* t = search_existing_target (pk))
      return t;

    // Then look for an existing file in the src tree.
    //
    return pk.tk.dir->relative () ? search_existing_file (pk) : nullptr;
  }

  optional<string>
  target_extension_null (const target_key&, const scope&, bool)
  {
    return nullopt;
  }

  optional<string>
  target_extension_assert (const target_key&, const scope&, bool)
  {
    assert (false); // Attempt to obtain the default extension.
    throw failed ();
  }

  void
  target_print_0_ext_verb (ostream& os, const target_key& k)
  {
    uint16_t v (stream_verb (os));
    to_stream (os, k, v < 2 ? 0 : v); // Remap 1 to 0.
  }

  void
  target_print_1_ext_verb (ostream& os, const target_key& k)
  {
    uint16_t v (stream_verb (os));
    to_stream (os, k, v < 1 ? 1 : v); // Remap 0 to 1.
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
    &target_search,
    false
  };

  const target_type mtime_target::static_type
  {
    "mtime_target",
    &target::static_type,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &target_search,
    false
  };

  const target_type path_target::static_type
  {
    "path_target",
    &mtime_target::static_type,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &target_search,
    false
  };

  template <typename T, const char* ext>
  static pair<target*, optional<string>>
  file_factory (const target_type& tt,
                dir_path d,
                dir_path o,
                string n,
                optional<string> e)
  {
    // A generic file target type doesn't imply any extension while a very
    // specific one (say man1) may have a fixed extension. So if one wasn't
    // specified and this is not a dynamically derived target type, then set
    // it to fixed ext rather than unspecified. For file{} we make it empty
    // which means we treat file{foo} as file{foo.}.
    //
    if (!e && ext != nullptr && tt.factory == &file_factory<T, ext>)
      e = string (ext);

    return make_pair (new T (move (d), move (o), move (n)), move (e));
  }

  extern const char file_ext_var[] = "extension"; // VC14 rejects constexpr.
  extern const char file_ext_def[] = "";

  const target_type file::static_type
  {
    "file",
    &path_target::static_type,
    &file_factory<file, file_ext_def>,
    &target_extension_var<file_ext_var, file_ext_def>,
    &target_pattern_var<file_ext_var, file_ext_def>,
    &target_print_1_ext_verb, // Print extension even at verbosity level 0.
    &file_search,
    false
  };

  static const target*
  alias_search (const target&, const prerequisite_key& pk)
  {
    // For an alias we don't want to silently create a target since it will do
    // nothing and it most likely not what the user intended.
    //
    const target* t (search_existing_target (pk));

    if (t == nullptr || t->implied)
      fail << "no explicit target for " << pk;

    return t;
  }

  const target_type alias::static_type
  {
    "alias",
    &target::static_type,
    &target_factory<alias>,
    nullptr, // Extension not used.
    nullptr,
    nullptr,
    &alias_search,
    false
  };

  static const target*
  dir_search (const target&, const prerequisite_key& pk)
  {
    tracer trace ("dir_search");

    // The first step is like in search_alias(): looks for an existing target.
    //
    const target* t (search_existing_target (pk));

    if (t != nullptr && !t->implied)
      return t;

    // If not found (or is implied), then try to load the corresponding
    // buildfile (which would normally define this target). Failed that, see
    // if we can assume an implied buildfile which would be equivalent to:
    //
    // ./: */
    //
    const dir_path& d (*pk.tk.dir);

    // We only do this for relative paths.
    //
    if (d.relative ())
    {
      // Note: this code is a custom version of parser::parse_include().

      const scope& s (*pk.scope);

      // Calculate the new out_base.
      //
      dir_path out_base (s.out_path () / d);
      out_base.normalize ();

      // In our world modifications to the scope structure during search &
      // match should be "pure append" in the sense that they should not
      // affect any existing targets that have already been searched &
      // matched.
      //
      // A straightforward way to enforce this is to not allow any existing
      // targets to be inside any newly created scopes (except, perhaps for
      // the directory target itself which we know hasn't been searched yet).
      // This, however, is not that straightforward to implement: we would
      // need to keep a directory prefix map for all the targets (e.g., in
      // target_set). Also, a buildfile could load from a directory that is
      // not a subdirectory of out_base. So for now we just assume that this
      // is so. And so it is.
      //
      bool retest (false);

      assert (phase == run_phase::match);
      {
        // Switch the phase to load.
        //
        phase_switch ps (run_phase::load);

        // This is subtle: while we were fussing around another thread may
        // have loaded the buildfile. So re-test now that we are in exclusive
        // phase.
        //
        if (t == nullptr)
          t = search_existing_target (pk);

        if (t != nullptr && !t->implied)
          retest = true;
        else
        {
          // Ok, no luck, switch the scope.
          //
          pair<scope&, scope*> sp (
            switch_scope (*s.rw ().root_scope (), out_base));

          if (sp.second != nullptr) // Ignore scopes out of any project.
          {
            scope& base (sp.first);
            scope& root (*sp.second);

            const dir_path& src_base (base.src_path ());

            path bf (src_base / "buildfile");

            if (exists (bf))
            {
              l5 ([&]{trace << "loading buildfile " << bf << " for " << pk;});
              retest = source_once (root, base, bf, root);
            }
            else if (exists (src_base))
            {
              t = dir::search_implied (base, pk, trace);
              retest = (t != nullptr);
            }
          }
        }
      }
      assert (phase == run_phase::match);

      // If we loaded/implied the buildfile, examine the target again.
      //
      if (retest)
      {
        if (t == nullptr)
          t = search_existing_target (pk);

        if (t != nullptr && !t->implied)
          return t;
      }
    }

    fail << "no explicit target for " << pk << endf;
  }

  static bool
  dir_pattern (const target_type&, const scope&, string& v, bool r)
  {
    // Add/strip trailing directory separator unless already there.
    //
    bool d (path::traits::is_separator (v.back ()));

    if (r)
    {
      assert (d);
      v.resize (v.size () - 1);
    }
    else if (!d)
    {
      v += path::traits::directory_separator;
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
    &dir_pattern,
    nullptr,
    &dir_search,
    false
  };

  const target_type fsdir::static_type
  {
    "fsdir",
    &target::static_type,
    &target_factory<fsdir>,
    nullptr,              // Extension not used.
    &dir_pattern,
    nullptr,
    &target_search,
    false
  };

  static optional<string>
  exe_target_extension (const target_key&, const scope&, bool search)
  {
    // If we are searching for an executable that is not a target, then
    // use the build machine executable extension. Otherwise, if this is
    // a target, then we expect the rule to use target machine extension.
    //
    return search
      ? optional<string> (
#ifdef _WIN32
        "exe"
#else
        ""
#endif
      )
      : nullopt;
  }

#ifdef _WIN32
  static bool
  exe_target_pattern (const target_type&, const scope&, string& v, bool r)
  {
    size_t p (path::traits::find_extension (v));

    if (r)
    {
      assert (p != string::npos);
      v.resize (p);
    }
    else if (p == string::npos)
    {
      v += ".exe";
      return true;
    }

    return false;
  }
#endif

  const target_type exe::static_type
  {
    "exe",
    &file::static_type,
    &target_factory<exe>,
    &exe_target_extension,
#ifdef _WIN32
    &exe_target_pattern,
#else
    nullptr,
#endif
    nullptr,
    &file_search,
    false
  };

  static pair<target*, optional<string>>
  buildfile_factory (const target_type&,
                     dir_path d,
                     dir_path o,
                     string n,
                     optional<string> e)
  {
    if (!e)
      e = (n == "buildfile" ? string () : "build");

    return make_pair (new buildfile (move (d), move (o), move (n)), move (e));
  }

  static optional<string>
  buildfile_target_extension (const target_key& tk, const scope&, bool)
  {
    // If the name is special 'buildfile', then there is no extension,
    // otherwise it is .build.
    //
    return *tk.name == "buildfile" ? string () : "build";
  }

  static bool
  buildfile_target_pattern (const target_type&,
                            const scope&,
                            string& v,
                            bool r)
  {
    size_t p (path::traits::find_extension (v));

    if (r)
    {
      assert (p != string::npos);
      v.resize (p);
    }
    else if (p == string::npos && v != "buildfile")
    {
      v += ".build";
      return true;
    }

    return false;
  }

  const target_type buildfile::static_type
  {
    "build",
    &file::static_type,
    &buildfile_factory,
    &buildfile_target_extension,
    &buildfile_target_pattern,
    nullptr,
    &file_search,
    false
  };

  // in
  //
  static const target*
  in_search (const target& xt, const prerequisite_key& cpk)
  {
    // If we have no extension then derive it from our target. The delegate
    // to file_search().
    //
    prerequisite_key pk (cpk);
    optional<string>& e (pk.tk.ext);

    if (!e)
    {
      if (const file* t = xt.is_a<file> ())
      {
        const string& te (t->derive_extension ());
        e = te + (te.empty () ? "" : ".") + "in";
      }
      else
        fail << "prerequisite " << pk << " for a non-file target " << xt;
    }

    return file_search (xt, pk);
  }

  static bool
  in_pattern (const target_type&, const scope&, string&, bool)
  {
    fail << "pattern in in{} prerequisite" << endf;
  }

  const target_type in::static_type
  {
    "in",
    &file::static_type,
    &file_factory<in, file_ext_def>, // No extension by default.
    &target_extension_assert,        // Should be taken care by search.
    &in_pattern,
    &target_print_1_ext_verb,        // Same as file.
    &in_search,
    false
  };

  const target_type doc::static_type
  {
    "doc",
    &file::static_type,
    &file_factory<doc, file_ext_def>, // No extension by default.
    &target_extension_var<file_ext_var, file_ext_def>, // Same as file.
    &target_pattern_var<file_ext_var, file_ext_def>,   // Same as file.
    &target_print_1_ext_verb, // Same as file.
    &file_search,
    false
  };

  static pair<target*, optional<string>>
  man_factory (const target_type&,
               dir_path d,
               dir_path o,
               string n,
               optional<string> e)
  {
    if (!e)
      fail << "man target '" << n << "' must include extension (man section)";

    return make_pair (new man (move (d), move (o), move (n)), move (e));
  }

  const target_type man::static_type
  {
    "man",
    &doc::static_type,
    &man_factory,
    &target_extension_null, // Should be specified explicitly (see factory).
    nullptr,
    &target_print_1_ext_verb, // Print extension even at verbosity level 0.
    &file_search,
    false
  };

  extern const char man1_ext[] = "1"; // VC14 rejects constexpr.

  const target_type man1::static_type
  {
    "man1",
    &man::static_type,
    &file_factory<man1, man1_ext>,
    &target_extension_fix<man1_ext>,
    &target_pattern_fix<man1_ext>,
    &target_print_0_ext_verb, // Fixed extension, no use printing.
    &file_search,
    false
  };
}
