// file      : build/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/target>

#include <cassert>

#include <butl/filesystem>

#include <build/scope>
#include <build/search>
#include <build/algorithm>
#include <build/diagnostics>

using namespace std;

namespace build
{
  // target_type
  //
  bool target_type::
  is_a (const type_index& id) const
  {
    for (const target_type* p (this); p != nullptr; p = p->base)
      if (p->id == id)
        return true;

    return false;
  }

  // target_state
  //
  static const char* target_state_[] = {
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
    recipe_ = std::move (r);

    // Also reset the target state. If this is a noop recipe, then
    // mark the target unchanged so that we don't waste time executing
    // the recipe.
    //
    raw_state = target_state::unknown;

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
    return scopes.find (dir);
  }

  scope& target::
  root_scope () const
  {
    // This is tricky to cache so we do the lookup for now.
    //
    scope* r (scopes.find (dir).root_scope ());
    assert (r != nullptr);
    return *r;
  }

  lookup<const value> target::
  operator[] (const variable& var) const
  {
    using result = lookup<const value>;

    if (auto p = vars.find (var))
      return result (p, &vars);

    if (group != nullptr)
    {
      if (auto p = group->vars.find (var))
        return result (p, &group->vars);
    }

    // Cannot simply delegate to scope's operator[] since we also
    // need to check target type/pattern-specific variables.
    //
    for (const scope* s (&base_scope ()); s != nullptr; s = s->parent_scope ())
    {
      if (!s->target_vars.empty ())
      {
        auto find_specific = [&var, s] (const target& t) -> result
        {
          // Search across target type hierarchy.
          //
          for (auto tt (&t.type ()); tt != nullptr; tt = tt->base)
          {
            auto i (s->target_vars.find (*tt));

            if (i == s->target_vars.end ())
              continue;

            //@@ TODO: match pattern. For now, we only handle '*'.

            auto j (i->second.find ("*"));

            if (j == i->second.end ())
              continue;

            if (auto p = j->second.find (var))
              return result (p, &j->second);
          }

          return result ();
        };

        if (auto p = find_specific (*this))
          return p;

        if (group != nullptr)
        {
          if (auto p = find_specific (*group))
            return p;
        }
      }

      if (auto p = s->vars.find (var))
        return result (p, &s->vars);
    }

    return result ();
  }

  value& target::
  append (const variable& var)
  {
    auto l (operator[] (var));

    if (l && l.belongs (*this)) // Existing variable in this target.
      return const_cast<value&> (*l);

    value& r (assign (var));

    if (l)
      r = *l; // Copy value from the outer scope.

    return r;
  }

  ostream&
  operator<< (ostream& os, const target& t)
  {
    return os << target_key {&t.type (), &t.dir, &t.name, &t.ext};
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
      const string* ext (*k.ext);
      if (t.ext != ext)
      {
        level4 ([&]{
            diag_record r (trace);
            r << "assuming target " << t << " is the same as the one with ";
            if (ext == nullptr)
              r << "unspecified extension";
            else if (ext->empty ())
              r << "no extension";
            else
              r << "extension " << *ext;
          });

        if (ext != nullptr)
          t.ext = ext;
      }
    }

    return i;
  }

  pair<target&, bool> target_set::
  insert (const target_type& tt,
          dir_path dir,
          string name,
          const string* ext,
          tracer& trace)
  {
    iterator i (find (target_key {&tt, &dir, &name, &ext}, trace));
    bool r (i == end ());

    if (r)
    {
      unique_ptr<target> pt (tt.factory (move (dir), move (name), ext));
      i = map_.emplace (
        make_pair (target_key {&tt, &pt->dir, &pt->name, &pt->ext},
                   move (pt))).first;
    }

    return pair<target&, bool> (**i, r);
  }

  ostream&
  operator<< (ostream& os, const target_key& k)
  {
    // If the name is empty, then we want to print the directory
    // inside {}, e.g., dir{bar/}, not bar/dir{}.
    //
    bool n (!k.name->empty ());
    string d (diag_relative (*k.dir, false));

    if (n)
      os << d;

    os << k.type->name << '{';

    if (n)
    {
      os << *k.name;

      if (*k.ext != nullptr && !(*k.ext)->empty ())
        os << '.' << **k.ext;
    }
    else
      os << d;

    os << '}';

    return os;
  }

  // path_target
  //
  void path_target::
  derive_path (const char* de, const char* np, const char* ns)
  {
    string n;

    if (np != nullptr)
      n += np;

    n += name;

    if (ns != nullptr)
      n += ns;

    // Update the extension.
    //
    // See also search_existing_file() if updating anything here.
    //
    if (ext == nullptr)
    {
      // If provided by the caller, then use that.
      //
      if (de != nullptr)
        ext = &extension_pool.find (de);
      //
      // Otherwis see if the target type has function that will
      // give us the default extension.
      //
      else if (auto f = type ().extension)
        ext = &f (key (), base_scope ()); // Already from the pool.
      else
        fail << "no default extension for target " << *this;
    }

    // Add the extension.
    //
    if (!ext->empty ())
    {
      n += '.';
      n += *ext;
    }

    path_type p (dir / path_type (move (n)));
    const path_type& ep (path ());

    if (ep.empty ())
      path (p);
    else if (p != ep)
      fail << "path mismatch for target " << *this <<
        info << "assigned '" << ep << "'" <<
        info << "derived  '" << p << "'";
  }

  // file_target
  //
  timestamp file::
  load_mtime () const
  {
    const path_type& f (path ());
    assert (!f.empty ());
    return file_mtime (f);
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

    // Then look for an existing file in this target-type-specific
    // list of paths (@@ TODO: comes from the variable).
    //
    if (pk.tk.dir->relative ())
    {
      dir_paths sp;
      sp.push_back (src_out (pk.scope->path (), *pk.scope)); // src_base
      return search_existing_file (pk, sp);
    }
    else
      return nullptr;
  }

  static target*
  search_alias (const prerequisite_key& pk)
  {
    // For an alias we don't want to silently create a target since it
    // will do nothing and it most likely not what the user intended.
    //
    target* t (search_existing_target (pk));

    if (t == nullptr)
      fail << "no explicit target for prerequisite " << pk;

    return t;
  }

  // type info
  //

  const target_type target::static_type
  {
    typeid (target),
    "target",
    nullptr,
    nullptr,
    nullptr,
    &search_target,
    false
  };

  const target_type mtime_target::static_type
  {
    typeid (mtime_target),
    "mtime_target",
    &target::static_type,
    nullptr,
    nullptr,
    &search_target,
    false
  };

  const target_type path_target::static_type
  {
    typeid (path_target),
    "path_target",
    &mtime_target::static_type,
    nullptr,
    nullptr,
    &search_target,
    false
  };

  template <typename T>
  static target*
  file_factory (dir_path d, string n, const string* e)
  {
    // The file target type doesn't imply any extension. So if one
    // wasn't specified, set it to empty rather than unspecified.
    // In other words, we always treat file{foo} as file{foo.}.
    //
    return new T (move (d),
                  move (n),
                  (e != nullptr ? e : &extension_pool.find ("")));
  }

  constexpr const char file_ext[] = "";
  const target_type file::static_type
  {
    typeid (file),
    "file",
    &path_target::static_type,
    &file_factory<file>,
    &target_extension_fix<file_ext>,
    &search_file,
    false
  };

  const target_type alias::static_type
  {
    typeid (alias),
    "alias",
    &target::static_type,
    &target_factory<alias>,
    nullptr, // Should never need.
    &search_alias,
    false
  };

  const target_type dir::static_type
  {
    typeid (dir),
    "dir",
    &alias::static_type,
    &target_factory<dir>,
    nullptr, // Should never need.
    &search_alias,
    false
  };

  const target_type fsdir::static_type
  {
    typeid (fsdir),
    "fsdir",
    &target::static_type,
    &target_factory<fsdir>,
    nullptr, // Should never need.
    &search_target,
    false
  };

  static const std::string&
  buildfile_target_extension (const target_key& tk, scope&)
  {
    // If the name is special 'buildfile', then there is no extension,
    // otherwise it is .build.
    //
    return extension_pool.find (*tk.name == "buildfile" ? "" : "build");
  }

  const target_type buildfile::static_type
  {
    typeid (buildfile),
    "buildfile",
    &file::static_type,
    &file_factory<buildfile>,
    &buildfile_target_extension,
    &search_file,
    false
  };

  constexpr const char doc_ext[] = "";
  const target_type doc::static_type
  {
    typeid (doc),
    "doc",
    &file::static_type,
    &file_factory<doc>,
    &target_extension_fix<doc_ext>,
    &search_file,
    false
  };

  static target*
  man_factory (dir_path d, string n, const string* e)
  {
    if (e == nullptr)
      fail << "man target '" << n << "' must include extension (man section)";

    return new man (move (d), move (n), e);
  }

  const target_type man::static_type
  {
    typeid (man),
    "man",
    &doc::static_type,
    &man_factory,
    nullptr,        // Should be specified explicitly.
    &search_file,
    false
  };

  constexpr const char man1_ext[] = "1";
  const target_type man1::static_type
  {
    typeid (man1),
    "man1",
    &man::static_type,
    &file_factory<man1>,
    &target_extension_fix<man1_ext>,
    &search_file,
    false
  };
}
