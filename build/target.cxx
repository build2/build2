// file      : build/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/target>

#include <build/scope>
#include <build/search>
#include <build/context>
#include <build/algorithm>   // execute_prerequisites()
#include <build/diagnostics>

using namespace std;

namespace build
{
  // recipe
  //
  target_state
  noop_recipe_function (action, target&)
  {
    assert (false); // We shouldn't be called, see target::recipe().
    return target_state::unchanged;
  }

  static target_state
  default_recipe_function (action a, target& t)
  {
    return current_mode == execution_mode::first
      ? execute_prerequisites (a, t)
      : reverse_execute_prerequisites (a, t);
  }

  const recipe empty_recipe;
  const recipe noop_recipe (&noop_recipe_function);
  const recipe default_recipe (&default_recipe_function);

  // target
  //
  scope& target::
  base_scope () const
  {
    return scopes.find (dir);
  }

  scope* target::
  root_scope () const
  {
    // This is tricky to cache so we do the lookup for now.
    //
    return scopes.find (dir).root_scope ();
  }

  ostream&
  operator<< (ostream& os, const target& t)
  {
    return os << target_key {&t.type (), &t.dir, &t.name, &t.ext};
  }

  static target*
  search_target (prerequisite& p)
  {
    // The default behavior is to look for an existing target in the
    // prerequisite's directory scope.
    //
    return search_existing_target (p);
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
          path dir,
          std::string name,
          const std::string* ext,
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
    os << k.type->name << '{';

    if (!k.dir->empty ())
    {
      string s (diag_relative (*k.dir));

      if (s != ".")
      {
        os << s;

        if (!k.name->empty () &&
            s.back () != path::traits::directory_separator)
          os << path::traits::directory_separator;
      }
    }

    os << *k.name;

    if (*k.ext != nullptr && !(*k.ext)->empty ())
      os << '.' << **k.ext;

    os << '}';

    return os;
  }

  //
  //
  target_type_map target_types;

  const target_type* target_type_map::
  find (name& n, const string*& ext) const
  {
    ext = nullptr;

    string& v (n.value);

    // First determine the target type.
    //
    const char* tt;
    if (n.type.empty ())
    {
      // Empty name or '.' and '..' signify a directory.
      //
      if (v.empty () || v == "." || v == "..")
        tt = "dir";
      else
        //@@ TODO: derive type from extension.
        //
        tt = "file";
    }
    else
      tt = n.type.c_str ();

    auto i (find (tt));
    if (i == end ())
      return nullptr;

    const target_type& ti (i->second);

    // Directories require special name processing. If we find that more
    // targets deviate, then we should make this target-type-specific.
    //
    if (ti.id == dir::static_type.id || ti.id == fsdir::static_type.id)
    {
      // The canonical representation of a directory name is with empty
      // value.
      //
      if (!v.empty ())
      {
        n.dir /= path (v); // Move name value to dir.
        v.clear ();
      }
    }
    else
    {
      // Split the path into its directory part (if any) the name part,
      // and the extension (if any). We cannot assume the name part is
      // a valid filesystem name so we will have to do the splitting
      // manually.
      //
      path::size_type i (path::traits::rfind_separator (v));

      if (i != string::npos)
      {
        n.dir /= path (v, i != 0 ? i : 1); // Special case: "/".
        v = string (v, i + 1, string::npos);
      }

      // Extract the extension.
      //
      string::size_type j (path::traits::find_extension (v));

      if (j != string::npos)
      {
        ext = &extension_pool.find (v.c_str () + j + 1);
        v.resize (j);
      }
    }

    return &ti;
  }

  // path_target
  //
  timestamp path_target::
  load_mtime () const
  {
    assert (!path_.empty ());
    return path_mtime (path_);
  }

  // file target
  //

  static target*
  search_file (prerequisite& p)
  {
    // First see if there is an existing target.
    //
    if (target* t = search_existing_target (p))
      return t;

    // Then look for an existing file in this target-type-specific
    // list of paths (@@ TODO: comes from the variable).
    //
    if (p.dir.relative ())
    {
      paths sp;
      sp.push_back (src_out (p.scope.path (), p.scope)); // src_base

      return search_existing_file (p, sp);
    }
    else
      return nullptr;
  }

  // dir target
  //
  static target*
  search_alias (prerequisite& p)
  {
    // For an alias/action we don't want to silently create a target
    // since it will do nothing and it most likely not what the author
    // intended.
    //
    target* t (search_existing_target (p));

    if (t == nullptr)
      fail << "no explicit target for prerequisite " << p;

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
    &search_target
  };

  const target_type mtime_target::static_type
  {
    typeid (mtime_target),
    "mtime_target",
    &target::static_type,
    nullptr,
    target::static_type.search
  };

  const target_type path_target::static_type
  {
    typeid (path_target),
    "path_target",
    &mtime_target::static_type,
    nullptr,
    mtime_target::static_type.search
  };

  const target_type file::static_type
  {
    typeid (file),
    "file",
    &path_target::static_type,
    &target_factory<file>,
    &search_file
  };

  const target_type dir::static_type
  {
    typeid (dir),
    "dir",
    &target::static_type,
    &target_factory<dir>,
    &search_alias
  };

  const target_type fsdir::static_type
  {
    typeid (fsdir),
    "fsdir",
    &target::static_type,
    &target_factory<fsdir>,
    target::static_type.search
  };
}
