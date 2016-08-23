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
  void value::
  reset ()
  {
    if (type == nullptr)
      as<names> ().~names ();
    else if (type->dtor != nullptr)
      type->dtor (*this);

    null = true;
  }

  value::
  value (value&& v)
      : type (v.type), null (v.null), extra (v.extra)
  {
    if (!null)
    {
      if (type == nullptr)
        new (&data_) names (move (v).as<names> ());
      else if (type->copy_ctor != nullptr)
        type->copy_ctor (*this, v, true);
      else
        data_ = v.data_; // Copy as POD.
    }
  }

  value::
  value (const value& v)
      : type (v.type), null (v.null), extra (v.extra)
  {
    if (!null)
    {
      if (type == nullptr)
        new (&data_) names (v.as<names> ());
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
        *this = nullptr;
        type = v.type;
      }

      // Now our types are the same. If the receiving value is NULL, then call
      // copy_ctor() instead of copy_assign().
      //
      if (v)
      {
        if (type == nullptr)
        {
          if (null)
            new (&data_) names (move (v).as<names> ());
          else
            as<names> () = move (v).as<names> ();
        }
        else if (auto f = null ? type->copy_ctor : type->copy_assign)
          f (*this, v, true);
        else
          data_ = v.data_; // Assign as POD.

        null = v.null;
      }
      else
        *this = nullptr;
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
        *this = nullptr;
        type = v.type;
      }

      // Now our types are the same. If the receiving value is NULL, then call
      // copy_ctor() instead of copy_assign().
      //
      if (v)
      {
        if (type == nullptr)
        {
          if (null)
            new (&data_) names (v.as<names> ());
          else
            as<names> () = v.as<names> ();
        }
        else if (auto f = null ? type->copy_ctor : type->copy_assign)
          f (*this, v, false);
        else
          data_ = v.data_; // Assign as POD.

        null = v.null;
      }
      else
        *this = nullptr;
    }

    return *this;
  }

  void value::
  assign (names&& ns, const variable* var)
  {
    assert (type == nullptr || type->assign != nullptr);

    if (type == nullptr)
    {
      if (null)
        new (&data_) names (move (ns));
      else
        as<names> () = move (ns);
    }
    else
      type->assign (*this, move (ns), var);

    null = false;
  }

  void value::
  append (names&& ns, const variable* var)
  {
    if (type == nullptr)
    {
      if (null)
        new (&data_) names (move (ns));
      else
      {
        names& p (as<names> ());

        if (p.empty ())
          p = move (ns);
        else if (!ns.empty ())
        {
          p.insert (p.end (),
                    make_move_iterator (ns.begin ()),
                    make_move_iterator (ns.end ()));
        }
      }
    }
    else
    {
      if (type->append == nullptr)
      {
        diag_record dr (fail);

        dr << "cannot append to " << type->name << " value";

        if (var != nullptr)
          dr << " in variable " << var->name;
      }

      type->append (*this, move (ns), var);
    }

    null = false;
  }

  void value::
  prepend (names&& ns, const variable* var)
  {
    if (type == nullptr)
    {
      if (null)
        new (&data_) names (move (ns));
      else
      {
        names& p (as<names> ());

        if (p.empty ())
          p = move (ns);
        else if (!ns.empty ())
        {
          ns.insert (ns.end (),
                     make_move_iterator (p.begin ()),
                     make_move_iterator (p.end ()));
          p.swap (ns);
        }
      }
    }
    else
    {
      if (type->prepend == nullptr)
      {
        diag_record dr (fail);

        dr << "cannot prepend to " << type->name << " value";

        if (var != nullptr)
          dr << " in variable " << var->name;
      }

      type->prepend (*this, move (ns), var);
    }

    null = false;
  }

  bool
  operator== (const value& x, const value& y)
  {
    bool xn (x.null);
    bool yn (y.null);

    assert (x.type == y.type ||
            (xn && x.type == nullptr) ||
            (yn && y.type == nullptr));

    if (xn || yn)
      return xn == yn;

    if (x.type == nullptr)
      return x.as<names> () == y.as<names> ();

    if (x.type->compare == nullptr)
      return memcmp (&x.data_, &y.data_, x.type->size) == 0;

    return x.type->compare (x, y) == 0;
  }

  bool
  operator< (const value& x, const value& y)
  {
    bool xn (x.null);
    bool yn (y.null);

    assert (x.type == y.type ||
            (xn && x.type == nullptr) ||
            (yn && y.type == nullptr));

    // NULL value is always less than non-NULL.
    //
    if (xn || yn)
      return xn > yn; // !xn < !yn

    if (x.type == nullptr)
      return x.as<names> () < y.as<names> ();

    if (x.type->compare == nullptr)
      return memcmp (&x.data_, &y.data_, x.type->size) < 0;

    return x.type->compare (x, y) < 0;
  }

  bool
  operator> (const value& x, const value& y)
  {
    bool xn (x.null);
    bool yn (y.null);

    assert (x.type == y.type ||
            (xn && x.type == nullptr) ||
            (yn && y.type == nullptr));

    // NULL value is always less than non-NULL.
    //
    if (xn || yn)
      return xn < yn; // !xn > !yn

    if (x.type == nullptr)
      return x.as<names> () > y.as<names> ();

    if (x.type->compare == nullptr)
      return memcmp (&x.data_, &y.data_, x.type->size) > 0;

    return x.type->compare (x, y) > 0;
  }

  void
  typify (value& v, const value_type& t, const variable* var)
  {
    if (v.type == nullptr)
    {
      if (v)
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
      diag_record dr (fail);

      dr << "type mismatch";

      if (var != nullptr)
        dr << " in variable " << var->name;

      dr << info << "value type is " << v.type->name;
      dr << info << (var != nullptr && &t == var->type ? "variable" : "new")
         << " type is " << t.name;
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

  const char* const value_traits<bool>::type_name = "bool";

  const value_type value_traits<bool>::value_type
  {
    type_name,
    sizeof (bool),
    nullptr,                      // No base.
    nullptr,                      // No dtor (POD).
    nullptr,                      // No copy_ctor (POD).
    nullptr,                      // No copy_assign (POD).
    &simple_assign<bool, false>,  // No empty value.
    &simple_append<bool, false>,
    &simple_append<bool, false>,  // Prepend same as append.
    &simple_reverse<bool>,
    nullptr,              // No cast (cast data_ directly).
    nullptr,              // No compare (compare as POD).
    nullptr               // Never empty.
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

  const char* const value_traits<uint64_t>::type_name = "uint64";

  const value_type value_traits<uint64_t>::value_type
  {
    type_name,
    sizeof (uint64_t),
    nullptr,                          // No base.
    nullptr,                          // No dtor (POD).
    nullptr,                          // No copy_ctor (POD).
    nullptr,                          // No copy_assign (POD).
    &simple_assign<uint64_t, false>,  // No empty value.
    &simple_append<uint64_t, false>,
    &simple_append<uint64_t, false>,  // Prepend same as append.
    &simple_reverse<uint64_t>,
    nullptr,                          // No cast (cast data_ directly).
    nullptr,                          // No compare (compare as POD).
    nullptr                           // Never empty.
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
      // Use either the precise or traditional representation depending on
      // whethe this is the original name (if it is, then this might not be
      // a path after all; think s/foo/bar/).
      //
      s = n.original
        ? move (n.dir).representation () // Move out of path.
        : move (n.dir).string ();
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
        if (r->original)
          s += move (r->dir).representation ();
        else
          s += r->dir.string ();
      }
      else
        s += r->value;
    }

    return s;
  }

  const char* const value_traits<string>::type_name = "string";

  const value_type value_traits<string>::value_type
  {
    type_name,
    sizeof (string),
    nullptr,                      // No base.
    &default_dtor<string>,
    &default_copy_ctor<string>,
    &default_copy_assign<string>,
    &simple_assign<string, true>, // Allow empty strings.
    &simple_append<string, true>,
    &simple_prepend<string, true>,
    &simple_reverse<string>,
    nullptr,                      // No cast (cast data_ directly).
    &simple_compare<string>,
    &default_empty<string>
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

  const char* const value_traits<path>::type_name = "path";

  const value_type value_traits<path>::value_type
  {
    type_name,
    sizeof (path),
    nullptr,                    // No base.
    &default_dtor<path>,
    &default_copy_ctor<path>,
    &default_copy_assign<path>,
    &simple_assign<path, true>, // Allow empty paths.
    &simple_append<path, true>,
    &simple_prepend<path, true>,
    &simple_reverse<path>,
    nullptr,                    // No cast (cast data_ directly).
    &simple_compare<path>,
    &default_empty<path>
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

  const char* const value_traits<dir_path>::type_name = "dir_path";

  const value_type value_traits<dir_path>::value_type
  {
    type_name,
    sizeof (dir_path),
    nullptr,                        // No base, or should it be path?
    &default_dtor<dir_path>,
    &default_copy_ctor<dir_path>,
    &default_copy_assign<dir_path>,
    &simple_assign<dir_path, true>, // Allow empty paths.
    &simple_append<dir_path, true>,
    &simple_prepend<dir_path, true>,
    &simple_reverse<dir_path>,
    nullptr,                        // No cast (cast data_ directly).
    &simple_compare<dir_path>,
    &default_empty<dir_path>
  };

  // abs_dir_path value
  //
  abs_dir_path value_traits<abs_dir_path>::
  convert (name&& n, name* r)
  {
    dir_path d (value_traits<dir_path>::convert (move (n), r));

    if (!d.empty ())
    {
      if (d.relative ())
        d.complete ();

      d.normalize (true); // Actualize.
    }

    return abs_dir_path (move (d));
  }

  const char* const value_traits<abs_dir_path>::type_name = "abs_dir_path";

  const value_type value_traits<abs_dir_path>::value_type
  {
    type_name,
    sizeof (abs_dir_path),
    &value_traits<dir_path>::value_type, // Assume direct cast works for both.
    &default_dtor<abs_dir_path>,
    &default_copy_ctor<abs_dir_path>,
    &default_copy_assign<abs_dir_path>,
    &simple_assign<abs_dir_path, true>,  // Allow empty paths.
    &simple_append<abs_dir_path, true>,
    nullptr,                             // No prepend.
    &simple_reverse<abs_dir_path>,
    nullptr,                             // No cast (cast data_ directly).
    &simple_compare<abs_dir_path>,
    &default_empty<abs_dir_path>
  };

  // name value
  //
  name value_traits<name>::
  convert (name&& n, name* r)
  {
    if (r != nullptr)
      throw invalid_argument (string ());

    n.original = false;
    return move (n);
  }

  static names_view
  name_reverse (const value& v, names&)
  {
    return names_view (&v.as<name> (), 1);
  }

  const char* const value_traits<name>::type_name = "name";

  const value_type value_traits<name>::value_type
  {
    type_name,
    sizeof (name),
    nullptr,                    // No base.
    &default_dtor<name>,
    &default_copy_ctor<name>,
    &default_copy_assign<name>,
    &simple_assign<name, true>, // Allow empty names.
    nullptr,                    // Append not supported.
    nullptr,                    // Prepend not supported.
    &name_reverse,
    nullptr,                    // No cast (cast data_ directly).
    &simple_compare<name>,
    &default_empty<name>
  };

  // process_path value
  //
  process_path value_traits<process_path>::
  convert (name&& n, name* r)
  {
    path rp (move (n.dir));
    if (rp.empty ())
      rp = path (move (n.value));
    else
      rp /= n.value;

    path ep;
    if (r != nullptr)
    {
      ep = move (r->dir);
      if (ep.empty ())
        ep = path (move (r->value));
      else
        ep /= r->value;
    }

    process_path pp (nullptr, move (rp), move (ep));
    pp.initial = pp.recall.string ().c_str ();
    return pp;
  }

  void
  process_path_copy_ctor (value& l, const value& r, bool m)
  {
    const auto& rhs (r.as<process_path> ());

    if (m)
      new (&l.data_) process_path (move (const_cast<process_path&> (rhs)));
    else
    {
      auto& lhs (
        *new (&l.data_) process_path (
          nullptr, path (rhs.recall), path (rhs.effect)));
      lhs.initial = lhs.recall.string ().c_str ();
    }
  }

  void
  process_path_copy_assign (value& l, const value& r, bool m)
  {
    auto& lhs (l.as<process_path> ());
    const auto& rhs (r.as<process_path> ());

    if (m)
      lhs = move (const_cast<process_path&> (rhs));
    else
    {
      lhs.recall = rhs.recall;
      lhs.effect = rhs.effect;
      lhs.initial = lhs.recall.string ().c_str ();
    }
  }

  static names_view
  process_path_reverse (const value& v, names& s)
  {
    auto& pp (v.as<process_path> ());
    s.reserve (pp.effect.empty () ? 1 : 2);

    s.push_back (name (pp.recall.directory (),
                       string (),
                       pp.recall.leaf ().string ()));

    if (!pp.effect.empty ())
    {
      s.back ().pair = '@';
      s.push_back (name (pp.effect.directory (),
                         string (),
                         pp.effect.leaf ().string ()));
    }

    return s;
  }

  const char* const value_traits<process_path>::type_name = "process_path";

  const value_type value_traits<process_path>::value_type
  {
    type_name,
    sizeof (process_path),
    nullptr,                            // No base.
    &default_dtor<process_path>,
    &process_path_copy_ctor,
    &process_path_copy_assign,
    &simple_assign<process_path, true>, // Allow empty values.
    nullptr,                            // Append not supported.
    nullptr,                            // Prepend not supported.
    &process_path_reverse,
    nullptr,                            // No cast (cast data_ directly).
    &simple_compare<process_path>,
    &default_empty<process_path>
  };

  // variable_pool
  //
  const variable& variable_pool::
  insert (string n,
          const build2::value_type* t,
          variable_visibility v,
          bool o)
  {
    auto p (variable_pool_base::insert (variable {move (n), t, nullptr, v}));
    const variable& r (*p.first);

    if (!p.second)
    {
      // Update type?
      //
      if (t != nullptr && r.type != t)
      {
        assert (r.type == nullptr);
        const_cast<variable&> (r).type = t; // Not changing the key.
      }

      // Change visibility? While this might at first seem like a bad idea,
      // it can happen that the variable lookup happens before any values
      // were set, in which case the variable will be entered with the
      // default visibility.
      //
      if (r.visibility != v)
      {
        assert (r.visibility == variable_visibility::normal); // Default.
        const_cast<variable&> (r).visibility = v; // Not changing the key.
      }

      // Check overridability (all overrides, if any, should already have
      // been enetered (see context.cxx:reset()).
      //
      if (r.override != nullptr && !o)
        fail << "variable " << r.name << " cannot be overridden";
    }

    return r;
  }

  variable_pool var_pool;

  // variable_map
  //
  const value* variable_map::
  find (const variable& var, bool typed) const
  {
    auto i (m_.find (var));
    const value* r (i != m_.end () ? &i->second : nullptr);

    // First access after being assigned a type?
    //
    if (r != nullptr && typed && var.type != nullptr && r->type != var.type)
      typify (const_cast<value&> (*r), *var.type, &var);

    return  r;
  }

  value* variable_map::
  find (const variable& var, bool typed)
  {
    auto i (m_.find (var));
    value* r (i != m_.end () ? &i->second : nullptr);

    // First access after being assigned a type?
    //
    if (r != nullptr && typed && var.type != nullptr && r->type != var.type)
      typify (*r, *var.type, &var);

    return  r;
  }

  pair<reference_wrapper<value>, bool> variable_map::
  insert (const variable& var, bool typed)
  {
    auto r (m_.emplace (var, value (typed ? var.type : nullptr)));
    value& v (r.first->second);

    // First access after being assigned a type?
    //
    if (typed && !r.second && var.type != nullptr && v.type != var.type)
      typify (v, *var.type, &var);

    return make_pair (reference_wrapper<value> (v), r.second);
  }

  // variable_type_map
  //
  lookup variable_type_map::
  find (const target_type& type,
        const string& name,
        const variable& var) const
  {
    // Search across target type hierarchy.
    //
    for (auto tt (&type); tt != nullptr; tt = tt->base)
    {
      auto i (variable_type_map_base::find (*tt));

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

        //@@ TODO: should we detect ambiguity? 'foo-*' '*-foo' and 'foo-foo'?
        //   Right now the last defined will be used.

        // Ok, this pattern matches. But is there a variable?
        //
        // Since we store append/prepend values untyped, instruct find() not
        // to automatically type it. And if it is assignment, then typify it
        // ourselves.
        //
        if (const value* v = j->second.find (var, false))
        {
          if (v->extra == 0 && var.type != nullptr && v->type != var.type)
            typify (const_cast<value&> (*v), *var.type, &var);

          return lookup (v, &j->second);
        }
      }
    }

    return lookup ();
  }

  // variable_override
  //
  map<pair<const variable_map*, const variable*>, variable_override_value>
  variable_override_cache;
}
