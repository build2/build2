// file      : build2/variable.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/variable.hxx>

#include <cstring> // memcmp()

#include <build2/context.hxx>
#include <build2/diagnostics.hxx>

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
    if (this != &v)
    {
      // Prepare the receiving value.
      //
      if (type != v.type)
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
    if (this != &v)
    {
      // Prepare the receiving value.
      //
      if (type != v.type)
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
          p = move (ns);
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

  void
  untypify (value& v)
  {
    if (v.type == nullptr)
      return;

    if (v.null)
    {
      v.type = nullptr;
      return;
    }

    names ns;
    names_view nv (v.type->reverse (v, ns));

    if (nv.empty () || nv.data () == ns.data ())
    {
      // If the data is in storage, then we are all set.
      //
      ns.resize (nv.size ()); // Just to be sure.
    }
    else
    {
      // If the data is somewhere in the value itself, then steal it.
      //
      auto b (const_cast<name*> (nv.data ()));
      ns.assign (make_move_iterator (b),
                 make_move_iterator (b + nv.size ()));
    }

    v = nullptr;                   // Free old data.
    v.type = nullptr;              // Change type.
    v.assign (move (ns), nullptr); // Assign new data.
  }

  // Throw invalid_argument for an invalid simple value.
  //
  [[noreturn]] static void
  throw_invalid_argument (const name& n, const name* r, const char* type)
  {
    string m;
    string t (type);

    if (r != nullptr)
      m = "pair in " + t + " value";
    else
    {
      m = "invalid " + t + " value: ";

      if (n.simple ())
        m += "'" + n.value + "'";
      else if (n.directory ())
        m += "'" + n.dir.representation () + "'";
      else
        m += "complex name";
    }

    throw invalid_argument (m);
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

    throw_invalid_argument (n, r, "bool");
  }

  const char* const value_traits<bool>::type_name = "bool";

  const value_type value_traits<bool>::value_type
  {
    type_name,
    sizeof (bool),
    nullptr,                      // No base.
    nullptr,                      // No element.
    nullptr,                      // No dtor (POD).
    nullptr,                      // No copy_ctor (POD).
    nullptr,                      // No copy_assign (POD).
    &simple_assign<bool>,
    &simple_append<bool>,
    &simple_append<bool>,         // Prepend same as append.
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
      catch (const std::exception&)
      {
        // Fall through.
      }
    }

    throw_invalid_argument (n, r, "uint64");
  }

  const char* const value_traits<uint64_t>::type_name = "uint64";

  const value_type value_traits<uint64_t>::value_type
  {
    type_name,
    sizeof (uint64_t),
    nullptr,                          // No base.
    nullptr,                          // No element.
    nullptr,                          // No dtor (POD).
    nullptr,                          // No copy_ctor (POD).
    nullptr,                          // No copy_assign (POD).
    &simple_assign<uint64_t>,
    &simple_append<uint64_t>,
    &simple_append<uint64_t>,         // Prepend same as append.
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
      throw_invalid_argument (n, r, "string");

    string s;

    if (n.directory (true))
      // Note that here we cannot assume what's in dir is really a
      // path (think s/foo/bar/) so we have to reverse it exactly.
      //
      s = move (n.dir).representation (); // Move out of path.
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
        s += move (r->dir).representation ();
      else
        s += r->value;
    }

    return s;
  }

  const string& value_traits<string>::empty_instance = empty_string;

  const char* const value_traits<string>::type_name = "string";

  const value_type value_traits<string>::value_type
  {
    type_name,
    sizeof (string),
    nullptr,                      // No base.
    nullptr,                      // No element.
    &default_dtor<string>,
    &default_copy_ctor<string>,
    &default_copy_assign<string>,
    &simple_assign<string>,
    &simple_append<string>,
    &simple_prepend<string>,
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
        catch (invalid_path& e)
        {
          n.value = move (e.path); // Restore the name object for diagnostics.

          // Fall through.
        }
      }

      // Fall through.
    }

    throw_invalid_argument (n, r, "path");
  }

  const path& value_traits<path>::empty_instance = empty_path;

  const char* const value_traits<path>::type_name = "path";

  const value_type value_traits<path>::value_type
  {
    type_name,
    sizeof (path),
    nullptr,                    // No base.
    nullptr,                    // No element.
    &default_dtor<path>,
    &default_copy_ctor<path>,
    &default_copy_assign<path>,
    &simple_assign<path>,
    &simple_append<path>,
    &simple_prepend<path>,
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

    throw_invalid_argument (n, r, "dir_path");
  }

  const dir_path& value_traits<dir_path>::empty_instance = empty_dir_path;

  const char* const value_traits<dir_path>::type_name = "dir_path";

  const value_type value_traits<dir_path>::value_type
  {
    type_name,
    sizeof (dir_path),
    &value_traits<path>::value_type, // Base (assuming direct cast works for
                                     // both).
    nullptr,                         // No element.
    &default_dtor<dir_path>,
    &default_copy_ctor<dir_path>,
    &default_copy_assign<dir_path>,
    &simple_assign<dir_path>,
    &simple_append<dir_path>,
    &simple_prepend<dir_path>,
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
    if (r == nullptr && (n.simple () || n.directory ()))
    {
      try
      {
        dir_path d (n.simple () ? dir_path (move (n.value)) : move (n.dir));

        if (!d.empty ())
        {
          if (d.relative ())
            d.complete ();

          d.normalize (true); // Actualize.
        }

        return abs_dir_path (move (d));
      }
      catch (const invalid_path&) {} // Fall through.
    }

    throw_invalid_argument (n, r, "abs_dir_path");
  }

  const char* const value_traits<abs_dir_path>::type_name = "abs_dir_path";

  const value_type value_traits<abs_dir_path>::value_type
  {
    type_name,
    sizeof (abs_dir_path),
    &value_traits<dir_path>::value_type, // Base (assuming direct cast works
                                         // for both).
    nullptr,                             // No element.
    &default_dtor<abs_dir_path>,
    &default_copy_ctor<abs_dir_path>,
    &default_copy_assign<abs_dir_path>,
    &simple_assign<abs_dir_path>,
    &simple_append<abs_dir_path>,
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
    if (r == nullptr)
      return move (n);

    throw_invalid_argument (n, r, "name");
  }

  static names_view
  name_reverse (const value& v, names&)
  {
    const name& n (v.as<name> ());
    return n.empty () ? names_view (nullptr, 0) : names_view (&n, 1);
  }

  const char* const value_traits<name>::type_name = "name";

  const value_type value_traits<name>::value_type
  {
    type_name,
    sizeof (name),
    nullptr,                    // No base.
    nullptr,                    // No element.
    &default_dtor<name>,
    &default_copy_ctor<name>,
    &default_copy_assign<name>,
    &simple_assign<name>,
    nullptr,                    // Append not supported.
    nullptr,                    // Prepend not supported.
    &name_reverse,
    nullptr,                    // No cast (cast data_ directly).
    &simple_compare<name>,
    &default_empty<name>
  };

  // name_pair
  //
  name_pair value_traits<name_pair>::
  convert (name&& n, name* r)
  {
    n.pair = '\0'; // Keep "unpaired" in case r is empty.
    return name_pair (move (n), r != nullptr ? move (*r) : name ());
  }

  void
  name_pair_assign (value& v, names&& ns, const variable* var)
  {
    using traits = value_traits<name_pair>;

    size_t n (ns.size ());

    if (n <= 2)
    {
      try
      {
        traits::assign (
          v,
          (n == 0
           ? name_pair ()
           : traits::convert (move (ns[0]), n == 2 ? &ns[1] : nullptr)));
        return;
      }
      catch (const invalid_argument&) {} // Fall through.
    }

    diag_record dr (fail);
    dr << "invalid name_pair value '" << ns << "'";

    if (var != nullptr)
      dr << " in variable " << var->name;
  }

  static names_view
  name_pair_reverse (const value& v, names& ns)
  {
    const name_pair& p (v.as<name_pair> ());
    const name& f (p.first);
    const name& s (p.second);

    if (f.empty () && s.empty ())
      return names_view (nullptr, 0);

    if (f.empty ())
      return names_view (&s, 1);

    if (s.empty ())
      return names_view (&f, 1);

    ns.push_back (f);
    ns.back ().pair = '@';
    ns.push_back (s);
    return ns;
  }

  const char* const value_traits<name_pair>::type_name = "name_pair";

  const value_type value_traits<name_pair>::value_type
  {
    type_name,
    sizeof (name_pair),
    nullptr,                         // No base.
    nullptr,                         // No element.
    &default_dtor<name_pair>,
    &default_copy_ctor<name_pair>,
    &default_copy_assign<name_pair>,
    &name_pair_assign,
    nullptr,                         // Append not supported.
    nullptr,                         // Prepend not supported.
    &name_pair_reverse,
    nullptr,                         // No cast (cast data_ directly).
    &simple_compare<name_pair>,
    &default_empty<name_pair>
  };

  // process_path value
  //
  process_path value_traits<process_path>::
  convert (name&& n, name* r)
  {
    if (                   n.untyped () &&  n.unqualified () &&  !n.empty () &&
        (r == nullptr || (r->untyped () && r->unqualified () && !r->empty ())))
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

    throw_invalid_argument (n, r, "process_path");
  }

  void
  process_path_assign (value& v, names&& ns, const variable* var)
  {
    using traits = value_traits<process_path>;

    size_t n (ns.size ());

    if (n <= 2)
    {
      try
      {
        traits::assign (
          v,
          (n == 0
           ? process_path ()
           : traits::convert (move (ns[0]), n == 2 ? &ns[1] : nullptr)));
        return;
      }
      catch (const invalid_argument&) {} // Fall through.
    }

    diag_record dr (fail);
    dr << "invalid process_path value '" << ns << "'";

    if (var != nullptr)
      dr << " in variable " << var->name;
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
    const process_path& x (v.as<process_path> ());

    if (!x.empty ())
    {
      s.reserve (x.effect.empty () ? 1 : 2);

      s.push_back (name (x.recall.directory (),
                         string (),
                         x.recall.leaf ().string ()));

      if (!x.effect.empty ())
      {
        s.back ().pair = '@';
        s.push_back (name (x.effect.directory (),
                           string (),
                           x.effect.leaf ().string ()));
      }
    }

    return s;
  }

  const char* const value_traits<process_path>::type_name = "process_path";

  const value_type value_traits<process_path>::value_type
  {
    type_name,
    sizeof (process_path),
    nullptr,                         // No base.
    nullptr,                         // No element.
    &default_dtor<process_path>,
    &process_path_copy_ctor,
    &process_path_copy_assign,
    &process_path_assign,
    nullptr,                         // Append not supported.
    nullptr,                         // Prepend not supported.
    &process_path_reverse,
    nullptr,                         // No cast (cast data_ directly).
    &simple_compare<process_path>,
    &default_empty<process_path>
  };

  // target_triplet value
  //
  target_triplet value_traits<target_triplet>::
  convert (name&& n, name* r)
  {
    if (r == nullptr)
    {
      if (n.simple ())
      {
        try
        {
          return n.empty () ? target_triplet () : target_triplet (n.value);
        }
        catch (const invalid_argument& e)
        {
          throw invalid_argument (
            string ("invalid target_triplet value: ") + e.what ());
        }
      }

      // Fall through.
    }

    throw_invalid_argument (n, r, "target_triplet");
  }

  const char* const value_traits<target_triplet>::type_name = "target_triplet";

  const value_type value_traits<target_triplet>::value_type
  {
    type_name,
    sizeof (target_triplet),
    nullptr,                         // No base.
    nullptr,                         // No element.
    &default_dtor<target_triplet>,
    &default_copy_ctor<target_triplet>,
    &default_copy_assign<target_triplet>,
    &simple_assign<target_triplet>,
    nullptr,                         // Append not supported.
    nullptr,                         // Prepend not supported.
    &simple_reverse<target_triplet>,
    nullptr,                         // No cast (cast data_ directly).
    &simple_compare<target_triplet>,
    &default_empty<target_triplet>
  };

  // variable_pool
  //
  void variable_pool::
  update (variable& var,
          const build2::value_type* t,
          const variable_visibility* v,
          const bool* o) const
  {
    // Check overridability (all overrides, if any, should already have
    // been entered (see context.cxx:reset()).
    //
    if (var.override != nullptr && (o == nullptr || !*o))
      fail << "variable " << var.name << " cannot be overridden";

    bool ut (t != nullptr && var.type != t);
    bool uv (v != nullptr && var.visibility != *v);

    // Update type?
    //
    if (ut)
    {
      assert (var.type == nullptr);
      var.type = t;
    }

    // Change visibility? While this might at first seem like a bad idea,
    // it can happen that the variable lookup happens before any values
    // were set, in which case the variable will be entered with the
    // default visibility.
    //
    if (uv)
    {
      assert (var.visibility == variable_visibility::normal); // Default.
      var.visibility = *v;
    }
  }

  static bool
  match_pattern (const string& n, const string& p, const string& s, bool multi)
  {
    size_t nn (n.size ()), pn (p.size ()), sn (s.size ());

    if (nn < pn + sn + 1)
      return false;

    if (pn != 0)
    {
      if (n.compare (0, pn, p) != 0)
        return false;
    }

    if (sn != 0)
    {
      if (n.compare (nn - sn, sn, s) != 0)
        return false;
    }

    // Make sure the stem is a single name unless instructed otherwise.
    //
    return multi || string::traits_type::find (n.c_str () + pn,
                                               nn - pn - sn,
                                               '.') == nullptr;
  }

  static inline void
  merge_pattern (const variable_pool::pattern& p,
                 const build2::value_type*& t,
                 const variable_visibility*& v,
                 const bool*& o)
  {
    if (p.type)
    {
      if (t == nullptr)
        t = *p.type;
      else if (p.match)
        assert (t == *p.type);
    }

    if (p.visibility)
    {
      if (v == nullptr)
        v = &*p.visibility;
      else if (p.match)
        assert (*v == *p.visibility);
    }

    if (p.overridable)
    {
      if (o == nullptr)
        o = &*p.overridable;
      else if (p.match)
      {
        // Allow the pattern to restrict but not relax.
        //
        if (*o)
          o = &*p.overridable;
        else
          assert (*o == *p.overridable);
      }
    }
  }

  const variable& variable_pool::
  insert (string n,
          const build2::value_type* t,
          const variable_visibility* v,
          const bool* o)
  {
    assert (!global_ || phase == run_phase::load);

    // Apply pattern.
    //
    if (n.find ('.') != string::npos)
    {
      // Reverse means from the "largest" (most specific).
      //
      for (const pattern& p: reverse_iterate (patterns_))
      {
        if (match_pattern (n, p.prefix, p.suffix, p.multi))
        {
          merge_pattern (p, t, v, o);
          break;
        }
      }
    }

    auto p (
      insert (
        variable {
          move (n),
          t,
          nullptr,
          v != nullptr ? *v : variable_visibility::normal}));

    variable& r (p.first->second);

    if (!p.second) // Note: overridden variable will always exist.
    {
      if (t != nullptr || v != nullptr || o != nullptr)
        update (r, t, v, o); // Not changing the key.
      else if (r.override != nullptr)
        fail << "variable " << r.name << " cannot be overridden";
    }

    return r;
  }

  void variable_pool::
  insert_pattern (const string& p,
                  optional<const value_type*> t,
                  optional<bool> o,
                  optional<variable_visibility> v,
                  bool retro,
                  bool match)
  {
    assert (!global_ || phase == run_phase::load);

    size_t pn (p.size ());

    size_t w (p.find ('*'));
    assert (w != string::npos);

    bool multi (w + 1 != pn && p[w + 1] == '*');

    // Extract prefix and suffix.
    //
    string pfx, sfx;

    if (w != 0)
    {
      assert (p[w - 1] == '.' && w != 1);
      pfx.assign (p, 0, w);
    }

    w += multi ? 2 : 1; // First suffix character.
    size_t sn (pn - w); // Suffix length.

    if (sn != 0)
    {
      assert (p[w] == '.' && sn != 1);
      sfx.assign (p, w, sn);
    }

    auto i (
      patterns_.insert (
        pattern {move (pfx), move (sfx), multi, match, t, v, o}));

    // Apply retrospectively to existing variables.
    //
    if (retro)
    {
      for (auto& p: map_)
      {
        variable& var (p.second);

        if (match_pattern (var.name, i->prefix, i->suffix, i->multi))
        {
          // Make sure that none of the existing more specific patterns
          // match.
          //
          auto j (i), e (patterns_.end ());
          for (++j; j != e; ++j)
          {
            if (match_pattern (var.name, j->prefix, j->suffix, j->multi))
              break;
          }

          if (j == e)
            update (var,
                    t ?  *t : nullptr,
                    v ? &*v : nullptr,
                    o ? &*o : nullptr); // Not changing the key.
        }
      }
    }
  }

  variable_pool variable_pool::instance (true);
  const variable_pool& variable_pool::cinstance = variable_pool::instance;
  const variable_pool& var_pool = variable_pool::cinstance;

  // variable_map
  //
  void variable_map::
  typify (value_data& v, const variable& var) const
  {
    // We assume typification is not modification so no version increment.
    //
    build2::typify (v, *var.type, &var);
  }

  auto variable_map::
  find (const variable& var, bool typed) const -> const value_data*
  {
    auto i (m_.find (var));
    const value_data* r (i != m_.end () ? &i->second : nullptr);

    // First access after being assigned a type?
    //
    if (r != nullptr && typed && var.type != nullptr && r->type != var.type)
    {
      // All values shall be typed during load.
      //
      assert (!global_ || phase == run_phase::load);
      typify (const_cast<value_data&> (*r), var);
    }

    return  r;
  }

  auto variable_map::
  find_to_modify (const variable& var, bool typed) -> value_data*
  {
    auto* r (const_cast<value_data*> (find (var, typed)));

    if (r != nullptr)
      r->version++;

    return r;
  }

  pair<reference_wrapper<value>, bool> variable_map::
  insert (const variable& var, bool typed)
  {
    assert (!global_ || phase == run_phase::load);

    auto p (m_.emplace (var, value_data (typed ? var.type : nullptr)));
    value_data& r (p.first->second);

    if (!p.second)
    {
      // First access after being assigned a type?
      //
      if (typed && var.type != nullptr && r.type != var.type)
        typify (r, var);
    }

    r.version++;

    return make_pair (reference_wrapper<value> (r), p.second);
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
      auto i (map_.find (*tt));

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
        const variable_map& vm (j->second);

        if (const variable_map::value_data* v = vm.find (var, false))
        {
          if (v->extra == 0 && var.type != nullptr && v->type != var.type)
          {
            // All values shall be typed during load.
            //
            assert (!global_ || phase == run_phase::load);
            vm.typify (const_cast<variable_map::value_data&> (*v), var);
          }

          return lookup (*v, vm);
        }
      }
    }

    return lookup ();
  }

  size_t variable_cache_mutex_shard_size;
  unique_ptr<shared_mutex[]> variable_cache_mutex_shard;
}
