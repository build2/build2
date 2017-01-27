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

namespace build2
{
  // target_key
  //
  const optional<string> target_key::nullext = nullopt;

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
  static const char* const target_state_[] = {
    "unknown", "unchanged", "postponed", "changed", "failed", "group"};

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

  target::
  ~target ()
  {
    clear_data ();
  }

  void target::
  recipe (action_type a, recipe_type r)
  {
    assert (a > action || !recipe_);

    bool override (a == action && recipe_); // See action::operator<.

    // Only noop_recipe can be overridden.
    //
    if (override)
    {
      recipe_function** f (recipe_.target<recipe_function*> ());
      assert (f != nullptr && *f == &noop_action);
    }

    action = a;
    recipe_ = move (r);

    raw_state = target_state::unknown;

    // If this is a noop recipe, then mark the target unchanged so that we
    // don't waste time executing the recipe.
    //
    if (recipe_function** f = recipe_.target<recipe_function*> ())
    {
      if (*f == &noop_action)
        raw_state = target_state::unchanged;
    }

    // This one is tricky: we don't want to reset the dependents count
    // if we are merely overriding with a "stronger" recipe.
    //
    if (!override)
      dependents = 0;
  }

  void target::
  reset (action_type)
  {
    clear_data ();
    prerequisite_targets.clear ();
  }

  group_view target::
  group_members (action_type) const
  {
    assert (false); // Not a group or doesn't expose its members.
    return group_view {nullptr, 0};
  }

  scope& target::
  base_scope () const
  {
    // If this target is from the src tree, use its out directory to find
    // the scope.
    //
    return scopes.find (out_dir ());
  }

  scope& target::
  root_scope () const
  {
    // This is tricky to cache so we do the lookup for now.
    //
    scope* r (base_scope ().root_scope ());
    assert (r != nullptr);
    return *r;
  }

  pair<lookup, size_t> target::
  find_original (const variable& var, bool target_only) const
  {
    pair<lookup, size_t> r (lookup (), 0);

    ++r.second;
    if (auto p = vars.find (var))
      r.first = lookup (p, &vars);

    target* g (nullptr);

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
      return const_cast<value&> (*l); // Ok since this is original.

    value& r (assign (var)); // NULL.

    if (l.defined ())
      r = *l; // Copy value (and type) from the outer scope.

    return r;
  }

  // target_set
  //
  target_set targets;

  auto target_set::
  find (const target_key& k, tracer& trace) const -> iterator
  {
    iterator i (map_.find (k));

    if (i != end ())
    {
      target& t (**i);

      // Update the extension if the existing target has it unspecified.
      //
      const optional<string>& ext (k.ext);
      if (t.ext != ext)
      {
        l5 ([&]{
            diag_record r (trace);
            r << "assuming target " << t << " is the same as the one with ";
            if (!ext)
              r << "unspecified extension";
            else if (ext->empty ())
              r << "no extension";
            else
              r << "extension " << *ext;
          });

        if (ext)
          t.ext = ext;
      }
    }

    return i;
  }

  pair<target&, bool> target_set::
  insert (const target_type& tt,
          dir_path dir,
          dir_path out,
          string name,
          optional<string> ext,
          bool implied,
          tracer& trace)
  {
    iterator i (find (target_key {&tt, &dir, &out, &name, ext}, trace));
    bool r (i == end ());

    if (r)
    {
      unique_ptr<target> pt (
        tt.factory (tt, move (dir), move (out), move (name), move (ext)));

      pt->implied = implied;

      i = map_.emplace (
        make_pair (target_key {&tt, &pt->dir, &pt->out, &pt->name, pt->ext},
                   move (pt))).first;
    }
    else if (!implied)
    {
      // Clear the implied flag.
      //
      target& t (**i);

      if (t.implied)
        t.implied = false;
    }

    return pair<target&, bool> (**i, r);
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

  // path_target
  //
  const string& path_target::
  derive_extension (const char* de, bool search)
  {
    // See also search_existing_file() if updating anything here.
    //
    assert (de == nullptr || type ().extension != nullptr);

    if (!ext)
    {
      // If the target type has the extension function then try that first.
      // The reason for preferring it over what's been provided by the caller
      // is that this function will often use the 'extension' variable which
      // the user can use to override extensions.
      //
      if (auto f = type ().extension)
        ext = f (key (), base_scope (), search);

      if (!ext)
      {
        if (de != nullptr)
          ext = de;
        else
          fail << "no default extension for target " << *this;
      }
    }

    return *ext;
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
    derive_extension (de);

    if (!ext->empty ())
    {
      p += '.';
      p += *ext;
    }

    const path_type& ep (path ());

    if (ep.empty ())
      path (move (p));
    else if (p != ep)
      fail << "path mismatch for target " << *this <<
        info << "existing '" << ep << "'" <<
        info << "derived  '" << p << "'";

    return path ();
  }

  // file_target
  //
  timestamp file::
  load_mtime () const
  {
    const path_type& f (path ());
    return f.empty () ? timestamp_unknown : file_mtime (f);
  }

  // Search functions.
  //

  target*
  search_target (const prerequisite_key& pk)
  {
    // The default behavior is to look for an existing target in the
    // prerequisite's directory scope.
    //
    return search_existing_target (pk);
  }

  target*
  search_file (const prerequisite_key& pk)
  {
    // First see if there is an existing target.
    //
    if (target* t = search_existing_target (pk))
      return t;

    // Then look for an existing file in the src tree.
    //
    return pk.tk.dir->relative () ? search_existing_file (pk) : nullptr;
  }

  optional<string>
  target_extension_null (const target_key&, scope&, bool)
  {
    return nullopt;
  }

  optional<string>
  target_extension_assert (const target_key&, scope&, bool)
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
    &search_target,
    false
  };

  const target_type mtime_target::static_type
  {
    "mtime_target",
    &target::static_type,
    nullptr,
    nullptr,
    nullptr,
    &search_target,
    false
  };

  const target_type path_target::static_type
  {
    "path_target",
    &mtime_target::static_type,
    nullptr,
    nullptr,
    nullptr,
    &search_target,
    false
  };

  template <typename T, const char* ext>
  static target*
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
    return new T (
      move (d),
      move (o),
      move (n),
      (e || ext == nullptr || tt.factory != &file_factory<T, ext>
       ? move (e)
       : string (ext)));
  }

  extern const char file_ext_var[] = "extension"; // VC14 rejects constexpr.
  extern const char file_ext_def[] = "";

  const target_type file::static_type
  {
    "file",
    &path_target::static_type,
    &file_factory<file, file_ext_def>,
    &target_extension_var<file_ext_var, file_ext_def>,
    &target_print_1_ext_verb, // Print extension even at verbosity level 0.
    &search_file,
    false
  };

  static target*
  search_alias (const prerequisite_key& pk)
  {
    // For an alias we don't want to silently create a target since it will do
    // nothing and it most likely not what the user intended.
    //
    target* t (search_existing_target (pk));

    if (t == nullptr || t->implied)
      fail << "no explicit target for prerequisite " << pk;

    return t;
  }

  const target_type alias::static_type
  {
    "alias",
    &target::static_type,
    &target_factory<alias>,
    nullptr, // Extension not used.
    nullptr,
    &search_alias,
    false
  };

  static target*
  search_dir (const prerequisite_key& pk)
  {
    tracer trace ("search_dir");

    // The first step is like in search_alias(): looks for an existing target.
    //
    target* t (search_existing_target (pk));

    if (t != nullptr && !t->implied)
      return t;

    // If not found (or is implied), then try to load the corresponding
    // buildfile which would normally define this target.
    //
    const dir_path& d (*pk.tk.dir);

    // We only do this for relative paths.
    //
    if (d.relative ())
    {
      // Note: this code is a custom version of parser::parse_include().

      scope& s (*pk.scope);

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
      {
        // Relock for exclusive access and change to the load phase.
        //
        rlock rl (model_lock);
        phase_guard pg (run_phase::load);

        pair<scope&, scope*> sp (switch_scope (*s.root_scope (), out_base));

        if (sp.second != nullptr) // Ignore scopes out of any project.
        {
          scope& base (sp.first);
          scope& root (*sp.second);

          path bf (base.src_path () / "buildfile");

          if (exists (bf))
          {
            l5 ([&]{trace << "loading buildfile " << bf << " for " << pk;});
            retest = source_once (root, base, bf, root);
          }
        }
      }

      // If we loaded the buildfile, examine the target again.
      //
      if (retest)
      {
        if (t == nullptr)
          t = search_existing_target (pk);

        if (t != nullptr && !t->implied)
          return t;
      }
    }

    fail << "no explicit target for prerequisite " << pk <<
      info << "did you forget to include the corresponding buildfile?" << endf;
  }

  const target_type dir::static_type
  {
    "dir",
    &alias::static_type,
    &target_factory<dir>,
    nullptr, // Extension not used.
    nullptr,
    &search_dir,
    false
  };

  const target_type fsdir::static_type
  {
    "fsdir",
    &target::static_type,
    &target_factory<fsdir>,
    nullptr, // Extension not used.
    nullptr,
    &search_target,
    false
  };

  static optional<string>
  exe_extension (const target_key&, scope&, bool search)
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

  const target_type exe::static_type
  {
    "exe",
    &file::static_type,
    &target_factory<exe>,
    &exe_extension,
    nullptr,
    &search_file,
    false
  };

  static target*
  buildfile_factory (const target_type&,
                     dir_path d,
                     dir_path o,
                     string n,
                     optional<string> e)
  {
    if (!e)
      e = (n == "buildfile" ? string () : "build");

    return new buildfile (move (d), move (o), move (n), move (e));
  }

  static optional<string>
  buildfile_target_extension (const target_key& tk, scope&, bool)
  {
    // If the name is special 'buildfile', then there is no extension,
    // otherwise it is .build.
    //
    return *tk.name == "buildfile" ? string () : "build";
  }

  const target_type buildfile::static_type
  {
    "build",
    &file::static_type,
    &buildfile_factory,
    &buildfile_target_extension,
    nullptr,
    &search_file,
    false
  };

  const target_type doc::static_type
  {
    "doc",
    &file::static_type,
    &file_factory<doc, file_ext_def>, // No extension by default.
    &target_extension_var<file_ext_var, file_ext_def>, // Same as file.
    &target_print_1_ext_verb, // Same as file.
    &search_file,
    false
  };

  static target*
  man_factory (const target_type&,
               dir_path d,
               dir_path o,
               string n,
               optional<string> e)
  {
    if (!e)
      fail << "man target '" << n << "' must include extension (man section)";

    return new man (move (d), move (o), move (n), move (e));
  }

  const target_type man::static_type
  {
    "man",
    &doc::static_type,
    &man_factory,
    &target_extension_null, // Should be specified explicitly (see factory).
    &target_print_1_ext_verb, // Print extension even at verbosity level 0.
    &search_file,
    false
  };

  extern const char man1_ext[] = "1"; // VC14 rejects constexpr.

  const target_type man1::static_type
  {
    "man1",
    &man::static_type,
    &file_factory<man1, man1_ext>,
    &target_extension_fix<man1_ext>,
    &target_print_0_ext_verb, // Fixed extension, no use printing.
    &search_file,
    false
  };
}
