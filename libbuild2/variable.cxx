// file      : libbuild2/variable.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/variable.hxx>

#include <cstring> // memcmp()

#include <libbutl/path-pattern.mxx>

#include <libbuild2/target.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace std;

namespace build2
{
  // variable_visibility
  //
  string
  to_string (variable_visibility v)
  {
    string r;

    switch (v)
    {
    case variable_visibility::global:  r = "global";       break;
    case variable_visibility::project: r = "project";      break;
    case variable_visibility::scope:   r = "scope";        break;
    case variable_visibility::target:  r = "target";       break;
    case variable_visibility::prereq:  r = "prerequisite"; break;
    }

    return r;
  }

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
  typify (value& v, const value_type& t, const variable* var, memory_order mo)
  {
    if (v.type == nullptr)
    {
      if (v)
      {
        // Note: the order in which we do things here is important.
        //
        names ns (move (v).as<names> ());
        v = nullptr;

        // Use value_type::assign directly to delay v.type change.
        //
        t.assign (v, move (ns), var);
        v.null = false;
      }
      else
        v.type = &t;

      v.type.store (&t, mo);
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
  typify_atomic (context& ctx,
                 value& v,
                 const value_type& t,
                 const variable* var)
  {
    // Typification is kind of like caching so we reuse that mutex shard.
    //
    shared_mutex& m (
      ctx.mutexes.variable_cache[
        hash<value*> () (&v) % ctx.mutexes.variable_cache_size]);

    // Note: v.type is rechecked by typify() under lock.
    //
    ulock l (m);
    typify (v, t, var, memory_order_release);
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

  [[noreturn]] void
  convert_throw (const value_type* from, const value_type& to)
  {
    string m ("invalid ");
    m += to.name;
    m += " value: ";

    if (from != nullptr)
    {
      m += "conversion from ";
      m += from->name;
    }
    else
      m += "null";

    throw invalid_argument (move (m));
  }

  // Throw invalid_argument for an invalid simple value.
  //
  [[noreturn]] static void
  throw_invalid_argument (const name& n,
                          const name* r,
                          const char* type,
                          bool pair_ok = false)
  {
    string m;
    string t (type);

    // Note that the message should be suitable for appending "in variable X".
    //
    if (!pair_ok && r != nullptr)
      m = "pair in " + t + " value";
    else if (n.pattern || (r != nullptr && r->pattern))
      m = "pattern in " + t + " value";
    else
    {
      m = "invalid " + t + " value ";

      if (n.simple ())
        m += "'" + n.value + "'";
      else if (n.directory ())
        m += "'" + n.dir.representation () + "'";
      else
        m += "name '" + to_string (n) + "'";
    }

    throw invalid_argument (m);
  }

  // names
  //
  const names& value_traits<names>::empty_instance = empty_names;

  // bool value
  //
  bool value_traits<bool>::
  convert (const name& n, const name* r)
  {
    if (r == nullptr && !n.pattern && n.simple () )
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

  // int64_t value
  //
  int64_t value_traits<int64_t>::
  convert (const name& n, const name* r)
  {
    if (r == nullptr && !n.pattern && n.simple ())
    {
      try
      {
        // May throw invalid_argument or out_of_range.
        //
        size_t i;
        int64_t r (stoll (n.value, &i));

        if (i == n.value.size ())
          return r;

        // Fall through.
      }
      catch (const std::exception&)
      {
        // Fall through.
      }
    }

    throw_invalid_argument (n, r, "int64");
  }

  const char* const value_traits<int64_t>::type_name = "int64";

  const value_type value_traits<int64_t>::value_type
  {
    type_name,
    sizeof (int64_t),
    nullptr,                          // No base.
    nullptr,                          // No element.
    nullptr,                          // No dtor (POD).
    nullptr,                          // No copy_ctor (POD).
    nullptr,                          // No copy_assign (POD).
    &simple_assign<int64_t>,
    &simple_append<int64_t>,
    &simple_append<int64_t>,          // Prepend same as append.
    &simple_reverse<int64_t>,
    nullptr,                          // No cast (cast data_ directly).
    nullptr,                          // No compare (compare as POD).
    nullptr                           // Never empty.
  };

  // uint64_t value
  //
  uint64_t value_traits<uint64_t>::
  convert (const name& n, const name* r)
  {
    if (r == nullptr && !n.pattern && n.simple ())
    {
      try
      {
        // May throw invalid_argument or out_of_range.
        //
        size_t i;
        uint64_t r (stoull (n.value, &i));

        if (i == n.value.size ())
          return r;

        // Fall through.
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
    if (n.pattern ||
        !(n.simple (true) || n.directory (true)))
      throw_invalid_argument (n, nullptr, "string");

    if (r != nullptr)
    {
      if (r->pattern ||
          !(r->simple (true) || r->directory (true)))
        throw_invalid_argument (*r, nullptr, "string");
    }

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
      string p (move (*n.proj).string ());
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
        s += r->proj->string ();
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
    if (r == nullptr && !n.pattern)
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

      // Reassemble split dir/value.
      //
      if (n.untyped () && n.unqualified ())
      {
        try
        {
          return n.dir / n.value;
        }
        catch (const invalid_path&)
        {
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
    if (r == nullptr && !n.pattern)
    {
      if (n.directory ())
        return move (n.dir);

      if (n.simple ())
      {
        try
        {
          return dir_path (move (n.value));
        }
        catch (invalid_path& e)
        {
          n.value = move (e.path); // Restore the name object for diagnostics.
          // Fall through.
        }
      }

      // Reassemble split dir/value.
      //
      if (n.untyped () && n.unqualified ())
      {
        try
        {
          n.dir /= n.value;
          return move (n.dir);
        }
        catch (const invalid_path&)
        {
          // Fall through.
        }
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
    if (r == nullptr && !n.pattern && (n.simple () || n.directory ()))
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
    if (r == nullptr && !n.pattern)
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
    if (n.pattern || (r != nullptr && r->pattern))
      throw_invalid_argument (n, r, "name_pair", true /* pair_ok */);

    n.pair = '\0'; // Keep "unpaired" in case r is empty.
    return name_pair (move (n), r != nullptr ? move (*r) : name ());
  }

  static void
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
  template <typename T>
  static T
  process_path_convert (name&& n, name* r, const char* what)
  {
    if (                   !n.pattern &&  n.untyped () &&  n.unqualified () &&  !n.empty () &&
        (r == nullptr || (!r->pattern && r->untyped () && r->unqualified () && !r->empty ())))
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

      T pp (nullptr, move (rp), move (ep));
      pp.initial = pp.recall.string ().c_str ();
      return pp;
    }

    throw_invalid_argument (n, r, what, true /* pair_ok */);
  }

  process_path value_traits<process_path>::
  convert (name&& n, name* r)
  {
    return process_path_convert<process_path> (move (n), r, "process_path");
  }

  static void
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

  template <typename T>
  static void
  process_path_copy_ctor (value& l, const value& r, bool m)
  {
    const auto& rhs (r.as<T> ());

    if (m)
      new (&l.data_) T (move (const_cast<T&> (rhs)));
    else
    {
      auto& lhs (
        *new (&l.data_) T (
          nullptr, path (rhs.recall), path (rhs.effect)));
      lhs.initial = lhs.recall.string ().c_str ();
    }
  }

  static void
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

  static void
  process_path_reverse_impl (const process_path& x, names& s)
  {
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

  static names_view
  process_path_reverse (const value& v, names& s)
  {
    const auto& x (v.as<process_path> ());

    if (!x.empty ())
    {
      s.reserve (x.effect.empty () ? 1 : 2);
      process_path_reverse_impl (x, s);
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
    &process_path_copy_ctor<process_path>,
    &process_path_copy_assign,
    &process_path_assign,
    nullptr,                         // Append not supported.
    nullptr,                         // Prepend not supported.
    &process_path_reverse,
    nullptr,                         // No cast (cast data_ directly).
    &simple_compare<process_path>,
    &default_empty<process_path>
  };

  // process_path_ex value
  //
  process_path_ex value_traits<process_path_ex>::
  convert (names&& ns)
  {
    if (ns.empty ())
      return process_path_ex ();

    bool p (ns[0].pair);

    process_path_ex pp (
      process_path_convert<process_path_ex> (
        move (ns[0]), p ? &ns[1] : nullptr, "process_path_ex"));

    for (auto i (ns.begin () + (p ? 2 : 1)); i != ns.end (); ++i)
    {
      if (!i->pair)
        throw invalid_argument ("non-pair in process_path_ex value");

      if (i->pattern || !i->simple ())
        throw_invalid_argument (*i, nullptr, "process_path_ex");

      const string& k ((i++)->value);

      // NOTE: see also find_end() below.
      //
      if (k == "name")
      {
        if (i->pattern || !i->simple ())
          throw_invalid_argument (*i, nullptr, "process_path_ex name");

        pp.name = move (i->value);
      }
      else if (k == "checksum")
      {
        if (i->pattern || !i->simple ())
          throw_invalid_argument (
            *i, nullptr, "process_path_ex executable checksum");

        pp.checksum = move (i->value);
      }
      else if (k == "env-checksum")
      {
        if (i->pattern || !i->simple ())
          throw_invalid_argument (
            *i, nullptr, "process_path_ex environment checksum");

        pp.env_checksum = move (i->value);
      }
      else
        throw invalid_argument (
          "unknown key '" + k + "' in process_path_ex value");
    }

    return pp;
  }

  names::iterator value_traits<process_path_ex>::
  find_end (names& ns)
  {
    auto b (ns.begin ()), i (b), e (ns.end ());
    for (i += i->pair ? 2 : 1; i != e && i->pair; i += 2)
    {
      if (!i->simple () || (i->value != "name" &&
                            i->value != "checksum" &&
                            i->value != "env-checksum"))
        break;
    }

    return i;
  }

  static void
  process_path_ex_assign (value& v, names&& ns, const variable* var)
  {
    using traits = value_traits<process_path_ex>;

    try
    {
      traits::assign (v, traits::convert (move (ns)));
    }
    catch (const invalid_argument& e)
    {
      // Note: ns is not guaranteed to be valid.
      //
      diag_record dr (fail);
      dr << "invalid process_path_ex value";

      if (var != nullptr)
        dr << " in variable " << var->name;

      dr << ": " << e;
    }
  }

  static void
  process_path_ex_copy_ex (value& l, const value& r, bool m)
  {
    auto& lhs (l.as<process_path_ex> ());

    if (m)
    {
      const auto& rhs (const_cast<value&> (r).as<process_path_ex> ());

      lhs.name = move (rhs.name);
      lhs.checksum = move (rhs.checksum);
      lhs.env_checksum = move (rhs.env_checksum);
    }
    else
    {
      const auto& rhs (r.as<process_path_ex> ());

      lhs.name = rhs.name;
      lhs.checksum = rhs.checksum;
      lhs.env_checksum = rhs.env_checksum;
    }
  }

  static void
  process_path_ex_copy_ctor (value& l, const value& r, bool m)
  {
    process_path_copy_ctor<process_path_ex> (l, r, m);

    if (!m)
      process_path_ex_copy_ex (l, r, false);
  }

  static void
  process_path_ex_copy_assign (value& l, const value& r, bool m)
  {
    process_path_copy_assign (l, r, m);
    process_path_ex_copy_ex (l, r, m);
  }

  static names_view
  process_path_ex_reverse (const value& v, names& s)
  {
    const auto& x (v.as<process_path_ex> ());

    if (!x.empty ())
    {
      s.reserve ((x.effect.empty () ? 1 : 2) +
                 (x.name ? 2 : 0)            +
                 (x.checksum ? 2 : 0)        +
                 (x.env_checksum ? 2 : 0));

      process_path_reverse_impl (x, s);

      if (x.name)
      {
        s.push_back (name ("name"));
        s.back ().pair = '@';
        s.push_back (name (*x.name));
      }

      if (x.checksum)
      {
        s.push_back (name ("checksum"));
        s.back ().pair = '@';
        s.push_back (name (*x.checksum));
      }

      if (x.env_checksum)
      {
        s.push_back (name ("env-checksum"));
        s.back ().pair = '@';
        s.push_back (name (*x.env_checksum));
      }
    }

    return s;
  }

  const char* const value_traits<process_path_ex>::type_name =
    "process_path_ex";

  const value_type value_traits<process_path_ex>::value_type
  {
    type_name,
    sizeof (process_path_ex),
    &value_traits<                   // Base (assuming direct cast works
      process_path>::value_type,     // for both).
    nullptr,                         // No element.
    &default_dtor<process_path_ex>,
    &process_path_ex_copy_ctor,
    &process_path_ex_copy_assign,
    &process_path_ex_assign,
    nullptr,                         // Append not supported.
    nullptr,                         // Prepend not supported.
    &process_path_ex_reverse,
    nullptr,                         // No cast (cast data_ directly).
    &simple_compare<process_path>,   // For now compare as process_path.
    &default_empty<process_path_ex>
  };

  // target_triplet value
  //
  target_triplet value_traits<target_triplet>::
  convert (name&& n, name* r)
  {
    if (r == nullptr && !n.pattern && n.simple ())
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

  // project_name value
  //
  project_name value_traits<project_name>::
  convert (name&& n, name* r)
  {
    if (r == nullptr && !n.pattern && n.simple ())
    {
      try
      {
        return n.empty () ? project_name () : project_name (move (n.value));
      }
      catch (const invalid_argument& e)
      {
        throw invalid_argument (
          string ("invalid project_name value: ") + e.what ());
      }
    }

    throw_invalid_argument (n, r, "project_name");
  }

  const project_name&
  value_traits<project_name>::empty_instance = empty_project_name;

  const char* const value_traits<project_name>::type_name = "project_name";

  const value_type value_traits<project_name>::value_type
  {
    type_name,
    sizeof (project_name),
    nullptr,                         // No base.
    nullptr,                         // No element.
    &default_dtor<project_name>,
    &default_copy_ctor<project_name>,
    &default_copy_assign<project_name>,
    &simple_assign<project_name>,
    nullptr,                         // Append not supported.
    nullptr,                         // Prepend not supported.
    &simple_reverse<project_name>,
    nullptr,                         // No cast (cast data_ directly).
    &simple_compare<project_name>,
    &default_empty<project_name>
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
    // been entered; see context ctor for details).
    //
    if (o != nullptr && var.overrides != nullptr && !*o)
      fail << "variable " << var.name << " cannot be overridden";

    bool ut (t != nullptr && var.type != t);
    bool uv (v != nullptr && var.visibility != *v);

    // Variable should not be updated post-aliasing.
    //
    assert (var.aliases == &var || (!ut && !uv));

    // Update type?
    //
    if (ut)
    {
      assert (var.type == nullptr);
      var.type = t;
    }

    // Change visibility? While this might at first seem like a bad idea, it
    // can happen that the variable lookup happens before any values were set
    // in which case the variable will be entered with the default (project)
    // visibility.
    //
    // For example, a buildfile, for some reason, could reference a target-
    // specific variable (say, test) before loading a module (say, test) that
    // sets this visibility. While strictly speaking we could have potentially
    // already made a lookup using the wrong visibility, presumably this
    // should be harmless.
    //
    // @@ But consider a situation where this test is set on scope prior to
    //    loading the module: now this value will effectively be unreachable
    //    without any diagnostics. So maybe we should try to clean this up.
    //    But we will need proper diagnostics instead of assert (which means
    //    we would probably need to track the location where the variable
    //    was first entered).
    //
    // Note also that this won't work well for global visibility since we only
    // allow restrictions. The thinking is that global visibility is special
    // and requiring special arrangements (like variable patterns, similar to
    // how we've done it for config.*) is reasonable. In fact, it feels like
    // only the build system core should be allowed to use global visibility
    // (see the context ctor for details).
    //
    if (uv)
    {
      assert (*v > var.visibility);
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
      {
        // Allow the pattern to restrict but not relax.
        //
        if (*p.visibility > *v)
          v = &*p.visibility;
        else
          assert (*v == *p.visibility);
      }
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

  pair<variable&, bool> variable_pool::
  insert (string n,
          const build2::value_type* t,
          const variable_visibility* v,
          const bool* o,
          bool pat)
  {
    assert (!global_ || global_->phase == run_phase::load);

    // Apply pattern.
    //
    const pattern* pa (nullptr);
    auto pt (t); auto pv (v); auto po (o);

    if (pat)
    {
      if (n.find ('.') != string::npos)
      {
        // Reverse means from the "largest" (most specific).
        //
        for (const pattern& p: reverse_iterate (patterns_))
        {
          if (match_pattern (n, p.prefix, p.suffix, p.multi))
          {
            merge_pattern (p, pt, pv, po);
            pa = &p;
            break;
          }
        }
      }
    }

    auto r (
      insert (
        variable {
          move (n),
          nullptr,
          pt,
          nullptr,
          pv != nullptr ? *pv : variable_visibility::project}));

    variable& var (r.first->second);

    if (r.second)
      var.aliases = &var;
    else // Note: overridden variable will always exist.
    {
      // This is tricky: if the pattern does not require a match, then we
      // should re-merge it with values that came from the variable.
      //
      bool vo;
      if (pa != nullptr && !pa->match)
      {
        pt = t != nullptr ? t : var.type;
        pv = v != nullptr ? v : &var.visibility;
        po = o != nullptr ? o : &(vo = true);

        merge_pattern (*pa, pt, pv, po);
      }

      if (po == nullptr) // NULL overridable falls back to false.
        po = &(vo = false);

      update (var, pt, pv, po); // Not changing the key.
    }

    return pair<variable&, bool> (var, r.second);
  }

  const variable& variable_pool::
  insert_alias (const variable& var, string n)
  {
    assert (var.aliases != nullptr && var.overrides == nullptr);

    variable& a (insert (move (n),
                         var.type,
                         &var.visibility,
                         nullptr /* override */,
                         false   /* pattern  */).first);

    assert (a.overrides == nullptr);

    if (a.aliases == &a) // Not aliased yet.
    {
      a.aliases = var.aliases;
      const_cast<variable&> (var).aliases = &a;
    }
    else
      assert (a.alias (var)); // Make sure it is already an alias of var.

    return a;
  }

  void variable_pool::
  insert_pattern (const string& p,
                  optional<const value_type*> t,
                  optional<bool> o,
                  optional<variable_visibility> v,
                  bool retro,
                  bool match)
  {
    assert (!global_ || global_->phase == run_phase::load);

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

  // variable_map
  //
  const variable_map empty_variable_map (nullptr /* context */);

  auto variable_map::
  lookup (const variable& var, bool typed) const ->
    pair<const value_data*, const variable&>
  {
    const variable* v (&var);
    const value_data* r (nullptr);
    do
    {
      // @@ Should we verify that there are no distinct values for aliases?
      //    This can happen if the values were entered before the variables
      //    were aliased. Possible but probably highly unlikely.
      //
      auto i (m_.find (*v));
      if (i != m_.end ())
      {
        r = &i->second;
        break;
      }

      v = v->aliases;

    } while (v != &var && v != nullptr);

    // Check if this is the first access after being assigned a type.
    //
    if (r != nullptr && typed && v->type != nullptr)
      typify (*r, *v);

    return pair<const value_data*, const variable&> (
      r, r != nullptr ? *v : var);
  }

  auto variable_map::
  lookup_to_modify (const variable& var, bool typed) ->
    pair<value_data*, const variable&>
  {
    auto p (lookup (var, typed));
    auto* r (const_cast<value_data*> (p.first));

    if (r != nullptr)
      r->version++;

    return pair<value_data*, const variable&> (r, p.second);
  }

  pair<value&, bool> variable_map::
  insert (const variable& var, bool typed)
  {
    assert (!global_ || ctx->phase == run_phase::load);

    auto p (m_.emplace (var, value_data (typed ? var.type : nullptr)));
    value_data& r (p.first->second);

    if (!p.second)
    {
      // Check if this is the first access after being assigned a type.
      //
      // Note: we still need atomic in case this is not a global state.
      //
      if (typed && var.type != nullptr)
        typify (r, var);
    }

    r.version++;

    return pair<value&, bool> (r, p.second);
  }

  bool variable_map::
  erase (const variable& var)
  {
    assert (!global_ || ctx->phase == run_phase::load);

    return m_.erase (var) != 0;
  }

  // variable_pattern_map
  //
  variable_map& variable_pattern_map::
  insert (pattern_type type, string&& text)
  {
    auto r (map_.emplace (pattern {type, false, move (text), {}},
                          variable_map (ctx, global_)));

    // Compile the regex.
    //
    if (r.second && type == pattern_type::regex_pattern)
    {
      // On exception restore the text argument (so that it's available for
      // diagnostics) and remove the element from the map.
      //
      auto eg (make_exception_guard (
                 [&text, &r, this] ()
                 {
                   text = r.first->first.text;
                   map_.erase (r.first);
                 }));

      const string& t (r.first->first.text);
      size_t n (t.size ()), p (t.rfind (t[0]));

      // Convert flags.
      //
      regex::flag_type f (regex::ECMAScript);
      for (size_t i (p + 1); i != n; ++i)
      {
        switch (t[i])
        {
        case 'i': f |= regex::icase;               break;
        case 'e': r.first->first.match_ext = true; break;
        }
      }

      // Skip leading delimiter as well as trailing delimiter and flags.
      //
      r.first->first.regex = regex (t.c_str () + 1, p - 1, f);
    }

    return r.first->second;
  }

  // variable_type_map
  //
  lookup variable_type_map::
  find (const target_key& tk,
        const variable& var,
        optional<string>& oname) const
  {
    // Compute and cache "effective" name that we will be matching.
    //
    // See also the additional match_ext logic below.
    //
    auto name = [&tk, &oname] () -> const string&
    {
      if (!oname)
      {
        const target_type& tt (*tk.type);

        // Note that if the name is not empty, then we always use that, even
        // if the type is dir/fsdir.
        //
        if (tk.name->empty () && (tt.is_a<dir> () || tt.is_a<fsdir> ()))
        {
          oname = tk.dir->leaf ().string ();
        }
        // If we have the extension and the type expects the extension to be
        // always specified explicitly by the user, then add it to the name.
        //
        // Overall, we have the following cases:
        //
        // 1. Extension is fixed: man1{}.
        //
        // 2. Extension is always specified by the user: file{}.
        //
        // 3. Default extension that may be overridden by the user: hxx{}.
        //
        // 4. Extension assigned by the rule but may be overridden by the
        //    user: obje{}.
        //
        // By default we only match the extension for (2).
        //
        else if (tk.ext && !tk.ext->empty () &&
                 (tt.fixed_extension == &target_extension_none ||
                  tt.fixed_extension == &target_extension_must))
        {
          oname = *tk.name + '.' + *tk.ext;
        }
        else
          oname = string (); // Use tk.name as is.
      }

      return oname->empty () ? *tk.name : *oname;
    };

    // Search across target type hierarchy.
    //
    for (auto tt (tk.type); tt != nullptr; tt = tt->base)
    {
      auto i (map_.find (*tt));

      if (i == end ())
        continue;

      // Try to match the pattern, starting from the longest values.
      //
      const variable_pattern_map& m (i->second);
      for (auto j (m.rbegin ()); j != m.rend (); ++j)
      {
        using pattern = variable_pattern_map::pattern;
        using pattern_type = variable_pattern_map::pattern_type;

        const pattern& pat (j->first);

        bool r, e (false);
        if (pat.type == pattern_type::path)
        {
          r = pat.text == "*" || butl::path_match (name (), pat.text);
        }
        else
        {
          const string& n (name ());

          // Deal with match_ext: first see if the extension would be added by
          // default. If not, then temporarily add it in oname and then clean
          // it up if there is no match (to prevent another pattern from using
          // it). While we may keep adding it if there are multiple patterns
          // with such a flag, we will at least reuse the buffer in oname.
          //
          e = pat.match_ext && tk.ext && !tk.ext->empty () && oname->empty ();
          if (e)
          {
            *oname = *tk.name;
            *oname += '.';
            *oname += *tk.ext;
          }

          r = regex_match (e ? *oname : n, *pat.regex);
        }

        // Ok, this pattern matches. But is there a variable?
        //
        // Since we store append/prepend values untyped, instruct find() not
        // to automatically type it. And if it is assignment, then typify it
        // ourselves.
        //
        if (r)
        {
          const variable_map& vm (j->second);
          auto p (vm.lookup (var, false));
          if (const variable_map::value_data* v = p.first)
          {
            // Check if this is the first access after being assigned a type.
            //
            if (v->extra == 0 && var.type != nullptr)
              vm.typify (*v, var);

            // Make sure the effective name is computed if this is
            // append/prepend (it is used as a cache key).
            //
            if (v->extra != 0 && !oname)
              name ();

            return lookup (*v, p.second, vm);
          }
        }

        if (e)
          oname->clear ();
      }
    }

    return lookup ();
  }

  template struct LIBBUILD2_DEFEXPORT value_traits<strings>;
  template struct LIBBUILD2_DEFEXPORT value_traits<vector<name>>;
  template struct LIBBUILD2_DEFEXPORT value_traits<paths>;
  template struct LIBBUILD2_DEFEXPORT value_traits<dir_paths>;
  template struct LIBBUILD2_DEFEXPORT value_traits<int64s>;
  template struct LIBBUILD2_DEFEXPORT value_traits<uint64s>;

  template struct LIBBUILD2_DEFEXPORT
  value_traits<vector<pair<string, string>>>;

  template struct LIBBUILD2_DEFEXPORT
  value_traits<vector<pair<string, optional<string>>>>;

  template struct LIBBUILD2_DEFEXPORT
  value_traits<vector<pair<optional<string>, string>>>;

  template struct LIBBUILD2_DEFEXPORT
  value_traits<vector<pair<string, optional<bool>>>>;

  template struct LIBBUILD2_DEFEXPORT
  value_traits<map<string, string>>;

  template struct LIBBUILD2_DEFEXPORT
  value_traits<map<string, optional<string>>>;

  template struct LIBBUILD2_DEFEXPORT
  value_traits<map<optional<string>, string>>;

  template struct LIBBUILD2_DEFEXPORT
  value_traits<map<string, optional<bool>>>;

  template struct LIBBUILD2_DEFEXPORT
  value_traits<map<project_name, dir_path>>;
}
