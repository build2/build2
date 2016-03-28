// file      : build2/variable.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/variable>

#include <cstring> // memcmp()

#include <build2/diagnostics>

using namespace std;

namespace build2
{
  // value
  //
  value& value::
  operator= (nullptr_t)
  {
    if (!null ())
    {
      if (type == nullptr)
        as<names> ().~names ();
      else if (type->dtor != nullptr)
        type->dtor (*this);

      state = value_state::null;
    }

    return *this;
  }

  value::
  value (value&& v)
      : type (v.type), state (v.state)
  {
    if (!null ())
    {
      if (type == nullptr)
        as<names> () = move (v).as<names> ();
      else if (type->copy_ctor != nullptr)
        type->copy_ctor (*this, v, true);
      else
        data_ = v.data_; // Copy as POD.
    }
  }

  value::
  value (const value& v)
      : type (v.type), state (v.state)
  {
    if (!null ())
    {
      if (type == nullptr)
        as<names> () = v.as<names> ();
      else if (type->copy_ctor != nullptr)
        type->copy_ctor (*this, v, false);
      else
        data_ = v.data_; // Copy as POD.
    }
  }

  value& value::
  operator= (value&& v)
  {
    assert (type == nullptr || type == v.type);

    if (this != &v)
    {
      // Prepare the receiving value.
      //
      if (type == nullptr && v.type != nullptr)
      {
        if (!null ())
          *this = nullptr;

        type = v.type;
      }

      // Now our types are the same. If the receiving value is NULL, then call
      // copy_ctor() instead of copy_assign().
      //
      if (type == nullptr)
        as<names> () = move (v).as<names> ();
      else if (auto f = null () ? type->copy_ctor : type->copy_assign)
        f (*this, v, true);
      else
        data_ = v.data_; // Assign as POD.

      state = v.state;
    }

    return *this;
  }

  value& value::
  operator= (const value& v)
  {
    assert (type == nullptr || type == v.type);

    if (this != &v)
    {
      // Prepare the receiving value.
      //
      if (type == nullptr && v.type != nullptr)
      {
        if (!null ())
        {
          reinterpret_cast<names&> (data_).~names ();
          state = value_state::null;
        }

        type = v.type;
      }

      // Now our types are the same. If the receiving value is NULL, then call
      // copy_ctor() instead of copy_assign().
      //
      if (type == nullptr)
        as<names> () = v.as<names> ();
      else if (auto f = null () ? type->copy_ctor : type->copy_assign)
        f (*this, v, false);
      else
        data_ = v.data_; // Assign as POD.

      state = v.state;
    }

    return *this;
  }

  void value::
  assign (names&& ns, const variable& var)
  {
    assert (type == nullptr || type->assign != nullptr);

    bool r;

    if (type == nullptr)
    {
      names* p;

      if (null ())
        p = new (&data_) names (move (ns));
      else
      {
        p = &as<names> ();
        *p = move (ns);
      }

      r = !p->empty ();
    }
    else
      r = type->assign (*this, move (ns), var);

    state = r ? value_state::filled : value_state::empty;
  }

  void value::
  append (names&& ns, const variable& var)
  {
    bool r;

    if (type == nullptr)
    {
      names* p;

      if (null ())
        p = new (&data_) names (move (ns));
      else
      {
        p = &as<names> ();

        if (p->empty ())
          *p = move (ns);
        else if (!ns.empty ())
        {
          p->insert (p->end (),
                     make_move_iterator (ns.begin ()),
                     make_move_iterator (ns.end ()));
        }
      }

      r = !p->empty ();
    }
    else
    {
      if (type->append == nullptr)
        fail << type->name << " value in variable " << var.name
             << " cannot be appended to";

      r = type->append (*this, move (ns), var);
    }

    state = r ? value_state::filled : value_state::empty;
  }

  void value::
  prepend (names&& ns, const variable& var)
  {
    bool r;

    if (type == nullptr)
    {
      names* p;

      if (null ())
        p = new (&data_) names (move (ns));
      else
      {
        p = &as<names> ();

        if (p->empty ())
          *p = move (ns);
        else if (!ns.empty ())
        {
          ns.insert (ns.end (),
                     make_move_iterator (p->begin ()),
                     make_move_iterator (p->end ()));
          p->swap (ns);
        }
      }

      r = !p->empty ();
    }
    else
    {
      if (type->prepend == nullptr)
        fail << type->name << " value in variable " << var.name
             << " cannot be prepended to";

      r = type->prepend (*this, move (ns), var);
    }

    state = r ? value_state::filled : value_state::empty;
  }

  bool
  operator== (const value& x, const value& y)
  {
    assert (x.type == y.type);

    if (x.state != y.state)
      return false;

    if (x.null ())
      return true;

    if (x.type == nullptr)
      return x.as<names> () == y.as<names> ();

    if (x.type->compare == nullptr)
      return memcmp (&x.data_, &y.data_, x.type->size) == 0;

    return x.type->compare (x, y) == 0;
  }

  void
  typify (value& v, const value_type& t, const variable& var)
  {
    if (v.type == nullptr)
    {
      if (!v.null ())
      {
        // Note: the order in which we do things here is important.
        //
        names ns (move (v).as<names> ());
        v = nullptr;
        v.type = &t;
        v.assign (move (ns), var);
      }
      else
        v.type = &t;
    }
    else if (v.type != &t)
    {
      fail << "variable " << var.name << " type mismatch" <<
        info << "value type is " << v.type->name <<
        info << (&t == var.type ? "variable" : "new") << " type is " << t.name;
    }
  }

  // bool value
  //
  bool value_traits<bool>::
  convert (name&& n, name* r)
  {
    if (r == nullptr && n.simple ())
    {
      const string& s (n.value);

      if (s == "true")
        return true;

      if (s == "false")
        return false;

      // Fall through.
    }

    throw invalid_argument (string ());
  }

  const value_type value_traits<bool>::value_type
  {
    "bool",
    sizeof (bool),
    nullptr,                      // No dtor (POD).
    nullptr,                      // No copy_ctor (POD).
    nullptr,                      // No copy_assign (POD).
    &simple_assign<bool, false>,  // No empty value.
    &simple_append<bool, false>,
    &simple_append<bool, false>,  // Prepend same as append.
    &simple_reverse<bool>,
    nullptr,              // No cast (cast data_ directly).
    nullptr               // No compare (compare as POD).
  };

  // uint64_t value
  //
  uint64_t value_traits<uint64_t>::
  convert (name&& n, name* r)
  {
    if (r == nullptr && n.simple ())
    {
      try
      {
        // May throw invalid_argument or out_of_range.
        //
        return stoull (n.value);
      }
      catch (const out_of_range&)
      {
        // Fall through.
      }
    }

    throw invalid_argument (string ());
  }

  const value_type value_traits<uint64_t>::value_type
  {
    "uint64",
    sizeof (uint64_t),
    nullptr,                          // No dtor (POD).
    nullptr,                          // No copy_ctor (POD).
    nullptr,                          // No copy_assign (POD).
    &simple_assign<uint64_t, false>,  // No empty value.
    &simple_append<uint64_t, false>,
    &simple_append<uint64_t, false>,  // Prepend same as append.
    &simple_reverse<uint64_t>,
    nullptr,                          // No cast (cast data_ directly).
    nullptr                           // No compare (compare as POD).
  };

  // string value
  //
  string value_traits<string>::
  convert (name&& n, name* r)
  {
    // The goal is to reverse the name into its original representation. The
    // code is a bit convoluted because we try to avoid extra allocations for
    // the common cases (unqualified, unpaired simple name or directory).
    //

    // We can only convert project-qualified simple and directory names.
    //
    if (!(n.simple (true) || n.directory (true)) ||
        !(r == nullptr || r->simple (true) || r->directory (true)))
      throw invalid_argument (string ());

    string s;

    if (n.directory (true))
    {
      s = move (n.dir).string (); // Move string out of path.

      // Add / back to the end of the path unless it is already there. Note
      // that the string cannot be empty (n.directory () would have been
      // false).
      //
      if (!dir_path::traits::is_separator (s[s.size () - 1]))
        s += '/';
    }
    else
      s.swap (n.value);

    // Convert project qualification to its string representation.
    //
    if (n.qualified ())
    {
      string p (*n.proj);
      p += '%';
      p += s;
      p.swap (s);
    }

    // The same for the RHS of a pair, if we have one.
    //
    if (r != nullptr)
    {
      s += '@';

      if (r->qualified ())
      {
        s += *r->proj;
        s += '%';
      }

      if (r->directory (true))
      {
        s += r->dir.string ();

        if (!dir_path::traits::is_separator (s[s.size () - 1]))
          s += '/';
      }
      else
        s += r->value;
    }

    return s;
  }

  const value_type value_traits<string>::value_type
  {
    "string",
    sizeof (string),
    &default_dtor<string>,
    &default_copy_ctor<string>,
    &default_copy_assign<string>,
    &simple_assign<string, true>, // Allow empty strings.
    &simple_append<string, true>,
    &simple_prepend<string, true>,
    &simple_reverse<string>,
    nullptr,                      // No cast (cast data_ directly).
    &simple_compare<string>
  };

  // path value
  //
  path value_traits<path>::
  convert (name&& n, name* r)
  {
    if (r == nullptr)
    {
      // A directory path is a path.
      //
      if (n.directory ())
        return move (n.dir);

      if (n.simple ())
      {
        try
        {
          return path (move (n.value));
        }
        catch (const invalid_path&) {} // Fall through.
      }

      // Fall through.
    }

    throw invalid_argument (string ());
  }

  const value_type value_traits<path>::value_type
  {
    "path",
    sizeof (path),
    &default_dtor<path>,
    &default_copy_ctor<path>,
    &default_copy_assign<path>,
    &simple_assign<path, true>, // Allow empty paths.
    &simple_append<path, true>,
    &simple_prepend<path, true>,
    &simple_reverse<path>,
    nullptr,                    // No cast (cast data_ directly).
    &simple_compare<path>
  };

  // dir_path value
  //
  dir_path value_traits<dir_path>::
  convert (name&& n, name* r)
  {
    if (r == nullptr)
    {
      if (n.directory ())
        return move (n.dir);

      if (n.simple ())
      {
        try
        {
          return dir_path (move (n.value));
        }
        catch (const invalid_path&) {} // Fall through.
      }

      // Fall through.
    }

    throw invalid_argument (string ());
  }

  const value_type value_traits<dir_path>::value_type
  {
    "dir_path",
    sizeof (dir_path),
    &default_dtor<dir_path>,
    &default_copy_ctor<dir_path>,
    &default_copy_assign<dir_path>,
    &simple_assign<dir_path, true>, // Allow empty paths.
    &simple_append<dir_path, true>,
    &simple_prepend<dir_path, true>,
    &simple_reverse<dir_path>,
    nullptr,                        // No cast (cast data_ directly).
    &simple_compare<dir_path>
  };

  // name value
  //
  name value_traits<name>::
  convert (name&& n, name* r)
  {
    if (r != nullptr)
      throw invalid_argument (string ());

    return move (n);
  }

  static names_view
  name_reverse (const value& v, names&)
  {
    return names_view (&v.as<name> (), 1);
  }

  const value_type value_traits<name>::value_type
  {
    "name",
    sizeof (name),
    &default_dtor<name>,
    &default_copy_ctor<name>,
    &default_copy_assign<name>,
    &simple_assign<name, true>, // Allow empty names.
    nullptr,                    // Append not supported.
    nullptr,                    // Prepend not supported.
    &name_reverse,
    nullptr,                    // No cast (cast data_ directly).
    &simple_compare<name>
  };

  // variable_pool
  //
  variable_pool var_pool;

  // variable_map
  //
  const value* variable_map::
  find (const variable& var) const
  {
    auto i (m_.find (var));
    const value* r (i != m_.end () ? &i->second : nullptr);

    // First access after being assigned a type?
    //
    if (r != nullptr && var.type != nullptr && r->type != var.type)
      typify (const_cast<value&> (*r), *var.type, var);

    return  r;
  }

  value* variable_map::
  find (const variable& var)
  {
    auto i (m_.find (var));
    value* r (i != m_.end () ? &i->second : nullptr);

    // First access after being assigned a type?
    //
    if (r != nullptr && var.type != nullptr && r->type != var.type)
      typify (*r, *var.type, var);

    return  r;
  }

  pair<reference_wrapper<value>, bool> variable_map::
  assign (const variable& var)
  {
    auto r (m_.emplace (var, value (var.type)));
    value& v (r.first->second);

    // First access after being assigned a type?
    //
    if (!r.second && var.type != nullptr && v.type != var.type)
      typify (v, *var.type, var);

    return make_pair (reference_wrapper<value> (v), r.second);
  }

  // variable_type_map
  //
  lookup<const value> variable_type_map::
  lookup (const target_type& type,
          const string& name,
          const variable& var) const
  {
    using result = build2::lookup<const value>;

    // Search across target type hierarchy.
    //
    for (auto tt (&type); tt != nullptr; tt = tt->base)
    {
      auto i (find (*tt));

      if (i == end ())
        continue;

      // Try to match the pattern, starting from the longest values
      // so that the more "specific" patterns (i.e., those that cover
      // fewer characters with the wildcard) take precedence. See
      // tests/variable/type-pattern.
      //
      const variable_pattern_map& m (i->second);

      for (auto j (m.rbegin ()); j != m.rend (); ++j)
      {
        const string& p (j->first);

        size_t nn (name.size ());
        size_t pn (p.size ());

        if (nn < pn - 1) // One for '*'.
          continue;

        size_t w (p.find ('*'));
        assert (w != string::npos);

        // Compare prefix.
        //
        if (w != 0 &&
            name.compare (0, w, p, 0, w) != 0)
          continue;

        ++w;     // First suffix character.
        pn -= w; // Suffix length.

        // Compare suffix.
        //
        if (pn != 0 &&
            name.compare (nn - pn, pn, p, w, pn) != 0)
          continue;

        // Ok, this pattern matches. But is there a variable?
        //
        if (const value* v = j->second.find (var))
        {
          //@@ TODO: should we detect ambiguity? 'foo-*' '*-foo' and
          //   'foo-foo'? Right now the last defined will be used.
          //
          return result (v, &j->second);
        }
      }
    }

    return result ();
  }
}
