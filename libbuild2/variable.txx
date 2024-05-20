// file      : libbuild2/variable.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/diagnostics.hxx>

namespace build2
{
  template <typename T>
  bool lookup::
  belongs (const T& x, bool t) const
  {
    if (vars == &x.vars)
      return true;

    if (t)
    {
      for (const auto& p1: x.target_vars) // variable_type_map
      {
        for (const auto& p2: p1.second) // variable_pattern_map
        {
          if (vars == &p2.second)
            return true;
        }
      }
    }

    return false;
  }

  [[noreturn]] LIBBUILD2_SYMEXPORT void
  convert_throw (const value_type* from, const value_type& to);

  template <typename T>
  T
  convert (value&& v)
  {
    if (v)
    {
      if (v.type == nullptr)
        return convert<T> (move (v).as<names> ());
      //
      // Note that while it may be tempting to use is_a() here like in cast(),
      // the implications are unclear (i.e., we may end up relaxing things we
      // don't want to). So we have the convert_to_base() variants instead.
      //
      else if (v.type == &value_traits<T>::value_type)
        return move (v).as<T> ();
    }

    convert_throw (v ? v.type : nullptr, value_traits<T>::value_type);
  }

  template <typename T>
  T
  convert (const value& v)
  {
    if (v)
    {
      if (v.type == nullptr)
        return convert<T> (names (v.as<names> ()));
      else if (v.type == &value_traits<T>::value_type)
        return v.as<T> ();
    }

    convert_throw (v ? v.type : nullptr, value_traits<T>::value_type);
  }

  template <typename T>
  T
  convert_to_base (value&& v)
  {
    if (v)
    {
      if (v.type == nullptr)
        return convert<T> (move (v).as<names> ());
      else if (v.type->is_a<T> ())
        return move (v).as<T> ();
    }

    convert_throw (v ? v.type : nullptr, value_traits<T>::value_type);
  }

  template <typename T>
  T
  convert_to_base (const value& v)
  {
    if (v)
    {
      if (v.type == nullptr)
        return convert<T> (names (v.as<names> ()));
      else if (v.type->is_a<T> ())
        return v.as<T> ();
    }

    convert_throw (v ? v.type : nullptr, value_traits<T>::value_type);
  }

  template <typename T>
  void
  default_dtor (value& v)
  {
    v.as<T> ().~T ();
  }

  template <typename T>
  void
  default_copy_ctor (value& l, const value& r, bool m)
  {
    if (m)
      new (&l.data_) T (move (const_cast<value&> (r).as<T> ()));
    else
      new (&l.data_) T (r.as<T> ());
  }

  template <typename T>
  void
  default_copy_assign (value& l, const value& r, bool m)
  {
    if (m)
      l.as<T> () = move (const_cast<value&> (r).as<T> ());
    else
      l.as<T> () = r.as<T> ();
  }

  template <typename T>
  bool
  default_empty (const value& v)
  {
    return value_traits<T>::empty (v.as<T> ());
  }

  template <typename T>
  void
  simple_assign (value& v, names&& ns, const variable* var)
  {
    size_t n (ns.size ());

    diag_record dr;
    if (value_traits<T>::empty_value ? n <= 1 : n == 1)
    {
      try
      {
        value_traits<T>::assign (
          v,
          (n == 0
           ? T ()
           : value_traits<T>::convert (move (ns.front ()), nullptr)));
      }
      catch (const invalid_argument& e)
      {
        dr << fail << e;
      }
    }
    else
      dr << fail << "invalid " << value_traits<T>::value_type.name
         << " value: " << (n == 0 ? "empty" : "multiple names");

    if (!dr.empty ())
    {
      if (var != nullptr)
        dr << " in variable " << var->name;

      dr << info << "while converting '" << ns << "'";
    }
  }

  template <typename T>
  void
  simple_append (value& v, names&& ns, const variable* var)
  {
    size_t n (ns.size ());

    diag_record dr;
    if (value_traits<T>::empty_value ? n <= 1 : n == 1)
    {
      try
      {
        value_traits<T>::append (
          v,
          (n == 0
           ? T ()
           : value_traits<T>::convert (move (ns.front ()), nullptr)));
      }
      catch (const invalid_argument& e)
      {
        dr << fail << e;
      }
    }
    else
      dr << fail << "invalid " << value_traits<T>::value_type.name
         << " value: " << (n == 0 ? "empty" : "multiple names");

    if (!dr.empty ())
    {
      if (var != nullptr)
        dr << " in variable " << var->name;

      dr << info << "while converting '" << ns << "'";
    }
  }

  template <typename T>
  void
  simple_prepend (value& v, names&& ns, const variable* var)
  {
    size_t n (ns.size ());

    diag_record dr;
    if (value_traits<T>::empty_value ? n <= 1 : n == 1)
    {
      try
      {
        value_traits<T>::prepend (
          v,
          (n == 0
           ? T ()
           : value_traits<T>::convert (move (ns.front ()), nullptr)));
      }
      catch (const invalid_argument& e)
      {
        dr << fail << e;
      }
    }
    else
      dr << fail << "invalid " << value_traits<T>::value_type.name
         << " value: " << (n == 0 ? "empty" : "multiple names");

    if (!dr.empty ())
    {
      if (var != nullptr)
        dr << " in variable " << var->name;

      dr << info << "while converting '" << ns << "'";
    }
  }

  template <typename T>
  names_view
  simple_reverse (const value& v, names& s, bool reduce)
  {
    const T& x (v.as<T> ());

    // Unless requested otherwise, represent an empty simple value as empty
    // name sequence rather than a single empty name. This way, for example,
    // during serialization we end up with a much saner looking:
    //
    // config.import.foo =
    //
    // Rather than:
    //
    // config.import.foo = {}
    //
    if (!value_traits<T>::empty (x))
      s.emplace_back (value_traits<T>::reverse (x));
    else if (!reduce)
      s.push_back (name ());

    return s;
  }

  template <typename T>
  int
  simple_compare (const value& l, const value& r)
  {
    return value_traits<T>::compare (l.as<T> (), r.as<T> ());
  }

  // pair<F, S> value
  //
  template <typename F, typename S>
  pair<F, S> pair_value_traits<F, S>::
  convert (name&& l, name* r,
           const char* type, const char* what, const variable* var)
  {
    if (!l.pair)
    {
      diag_record dr (fail);

      dr << type << ' ' << what << (*what != '\0' ? " " : "")
         << "pair expected instead of '" << l << "'";

      if (var != nullptr)
        dr << " in variable " << var->name;
    }

    if (l.pair != '@')
    {
      diag_record dr (fail);

      dr << "unexpected pair style for "
         << type << ' ' << what << (*what != '\0' ? " " : "")
         << "key-value pair '"
         << l << "'" << l.pair << "'" << *r << "'";

      if (var != nullptr)
        dr << " in variable " << var->name;
    }

    try
    {
      F f (value_traits<F>::convert (move (l), nullptr));

      try
      {
        S s (value_traits<S>::convert (move (*r), nullptr));

        return pair<F, S>  (move (f), move (s));
      }
      catch (const invalid_argument& e)
      {
        diag_record dr (fail);

        dr << e;
        if (var != nullptr)
          dr << " in variable " << var->name;

        dr << info << "while converting second have of pair '" << *r << "'"
           << endf;
      }
    }
    catch (const invalid_argument& e)
    {
      diag_record dr (fail);

      dr << e;
      if (var != nullptr)
        dr << " in variable " << var->name;

      dr << info << "while converting first have of pair '" << l << "'"
         << endf;
    }
  }

  template <typename F, typename S>
  pair<F, optional<S>> pair_value_traits<F, optional<S>>::
  convert (name&& l, name* r,
           const char* type, const char* what, const variable* var)
  {
    if (l.pair && l.pair != '@')
    {
      diag_record dr (fail);

      dr << "unexpected pair style for "
         << type << ' ' << what << (*what != '\0' ? " " : "")
         << "key-value pair '"
         << l << "'" << l.pair << "'" << *r << "'";

      if (var != nullptr)
        dr << " in variable " << var->name;
    }

    try
    {
      F f (value_traits<F>::convert (move (l), nullptr));

      try
      {
        optional<S> s;

        if (l.pair)
          s = value_traits<S>::convert (move (*r), nullptr);

        return pair<F, optional<S>>  (move (f), move (s));
      }
      catch (const invalid_argument& e)
      {
        diag_record dr (fail);

        dr << e;
        if (var != nullptr)
          dr << " in variable " << var->name;

        dr << info << "while converting second have of pair '" << *r << "'"
           << endf;
      }
    }
    catch (const invalid_argument& e)
    {
      diag_record dr (fail);

      dr << e;
      if (var != nullptr)
        dr << " in variable " << var->name;

      dr << info << "while converting first have of pair '" << l << "'"
         << endf;
    }
  }

  template <typename F, typename S>
  pair<optional<F>, S> pair_value_traits<optional<F>, S>::
  convert (name&& l, name* r,
           const char* type, const char* what, const variable* var)
  {
    if (l.pair && l.pair != '@')
    {
      diag_record dr (fail);

      dr << "unexpected pair style for "
         << type << ' ' << what << (*what != '\0' ? " " : "")
         << "key-value pair '"
         << l << "'" << l.pair << "'" << *r << "'";

      if (var != nullptr)
        dr << " in variable " << var->name;
    }

    try
    {
      optional<F> f;

      if (l.pair)
      {
        f = value_traits<F>::convert (move (l), nullptr);
        l = move (*r); // Shift.
      }

      try
      {
        S s (value_traits<S>::convert (move (l), nullptr));

        return pair<optional<F>, S>  (move (f), move (s));
      }
      catch (const invalid_argument& e)
      {
        diag_record dr (fail);

        dr << e;
        if (var != nullptr)
          dr << " in variable " << var->name;

        dr << info << "while converting second have of pair '" << *r << "'"
           << endf;
      }
    }
    catch (const invalid_argument& e)
    {
      diag_record dr (fail);

      dr << e;
      if (var != nullptr)
        dr << " in variable " << var->name;

      dr << info << "while converting first have of pair '" << l << "'"
         << endf;
    }
  }

  template <typename F, typename S>
  void pair_value_traits<F, S>::
  reverse (const F& f, const S& s, names& ns)
  {
    ns.push_back (value_traits<F>::reverse (f));
    ns.back ().pair = '@';
    ns.push_back (value_traits<S>::reverse (s));
  }

  template <typename F, typename S>
  void pair_value_traits<F, optional<S>>::
  reverse (const F& f, const optional<S>& s, names& ns)
  {
    ns.push_back (value_traits<F>::reverse (f));
    if (s)
    {
      ns.back ().pair = '@';
      ns.push_back (value_traits<S>::reverse (*s));
    }
  }

  template <typename F, typename S>
  void pair_value_traits<optional<F>, S>::
  reverse (const optional<F>& f, const S& s, names& ns)
  {
    if (f)
    {
      ns.push_back (value_traits<F>::reverse (*f));
      ns.back ().pair = '@';
    }
    ns.push_back (value_traits<S>::reverse (s));
  }

  // vector<T> value
  //
  template <typename T>
  vector<T> value_traits<vector<T>>::
  convert (names&& ns)
  {
    vector<T> v;
    v.reserve (ns.size ()); // Normally there won't be any pairs.

    // Similar to vector_append() below except we throw instead of issuing
    // diagnostics.
    //
    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      name& n (*i);
      name* r (nullptr);

      if (n.pair)
      {
        r = &*++i;

        if (n.pair != '@')
          throw invalid_argument (
            string ("invalid pair character: '") + n.pair + '\'');
      }

      v.push_back (value_traits<T>::convert (move (n), r));
    }

    return v;
  }

  template <typename T>
  void
  vector_append (value& v, names&& ns, const variable* var)
  {
    vector<T>& p (v
                  ? v.as<vector<T>> ()
                  : *new (&v.data_) vector<T> ());

    p.reserve (p.size () + ns.size ()); // Normally there won't be any pairs.

    // Convert each element to T while merging pairs.
    //
    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      name& n (*i);
      name* r (nullptr);

      if (n.pair)
      {
        r = &*++i;

        if (n.pair != '@')
        {
          diag_record dr (fail);

          dr << "unexpected pair style for "
             << value_traits<T>::value_type.name << " value "
             << "'" << n << "'" << n.pair << "'" << *r << "'";

          if (var != nullptr)
            dr << " in variable " << var->name;
        }
      }

      try
      {
        p.push_back (value_traits<T>::convert (move (n), r));
      }
      catch (const invalid_argument& e)
      {
        diag_record dr (fail);

        dr << e;
        if (var != nullptr)
          dr << " in variable " << var->name;

        dr << info << "while converting ";
        if (n.pair)
          dr << " element pair '" << n << "'@'" << *r << "'";
        else
          dr << " element '" << n << "'";
      }
    }
  }

  template <typename T>
  void
  vector_assign (value& v, names&& ns, const variable* var)
  {
    if (v)
      v.as<vector<T>> ().clear ();

    vector_append<T> (v, move (ns), var);
  }

  template <typename T>
  void
  vector_prepend (value& v, names&& ns, const variable* var)
  {
    // Reduce to append.
    //
    vector<T> t;
    vector<T>* p;

    if (v)
    {
      p = &v.as<vector<T>> ();
      p->swap (t);
    }
    else
      p = new (&v.data_) vector<T> ();

    vector_append<T> (v, move (ns), var);

    p->insert (p->end (),
               make_move_iterator (t.begin ()),
               make_move_iterator (t.end ()));
  }

  template <typename T>
  names_view
  vector_reverse (const value& v, names& s, bool)
  {
    auto& vv (v.as<vector<T>> ());
    s.reserve (vv.size ());

    for (const T& x: vv)
      s.push_back (value_traits<T>::reverse (x));

    return s;
  }

  template <typename T>
  int
  vector_compare (const value& l, const value& r)
  {
    auto& lv (l.as<vector<T>> ());
    auto& rv (r.as<vector<T>> ());

    auto li (lv.begin ()), le (lv.end ());
    auto ri (rv.begin ()), re (rv.end ());

    for (; li != le && ri != re; ++li, ++ri)
      if (int r = value_traits<T>::compare (*li, *ri))
        return r;

    if (li == le && ri != re) // l shorter than r.
      return -1;

    if (ri == re && li != le) // r shorter than l.
      return 1;

    return 0;
  }

  // Provide subscript for vector<T> for efficiency.
  //
  template <typename T>
  value
  vector_subscript (const value& val, value* val_data,
                    value&& sub,
                    const location& sloc,
                    const location& bloc)
  {
    // Process subscript even if the value is null to make sure it is valid.
    //
    size_t i;
    try
    {
      i = static_cast<size_t> (convert<uint64_t> (move (sub)));
    }
    catch (const invalid_argument& e)
    {
      fail (sloc) << "invalid " << value_traits<vector<T>>::value_type.name
                  << " value subscript: " << e <<
        info (bloc) << "use the '\\[' escape sequence if this is a "
                    << "wildcard pattern";
    }

    value r;
    if (!val.null)
    {
      const auto& v (val.as<vector<T>> ());
      if (i < v.size ())
      {
        const T& e (v[i]);

        // Steal the value if possible.
        //
        r = &val == val_data ? T (move (const_cast<T&> (e))) : T (e);
      }
    }

    // Typify null values so that type-specific subscript (e.g., for
    // json_value) gets called for chained subscripts.
    //
    if (r.null)
      r.type = &value_traits<T>::value_type;

    return r;
  }

  // Provide iterate for vector<T> for efficiency.
  //
  template <typename T>
  void
  vector_iterate (const value& val,
                  const function<void (value&&, bool first)>& f)
  {
    const auto& v (val.as<vector<T>> ()); // Never NULL.

    for (auto b (v.begin ()), i (b), e (v.end ()); i != e; ++i)
    {
      f (value (*i), i == b);
    }
  }

  // Make sure these are static-initialized together. Failed that VC will make
  // sure it's done in the wrong order.
  //
  template <typename T>
  struct vector_value_type: value_type
  {
    string type_name;

    vector_value_type (value_type&& v)
        : value_type (move (v))
    {
      // Note: vector<T> always has a convenience alias.
      //
      type_name  = value_traits<T>::type_name;
      type_name += 's';
      name = type_name.c_str ();
    }
  };

  template <typename T>
  const vector<T> value_traits<vector<T>>::empty_instance;

  template <typename T>
  const vector_value_type<T>
  value_traits<vector<T>>::value_type = build2::value_type // VC14 wants =.
  {
    nullptr,                          // Patched above.
    sizeof (vector<T>),
    nullptr,                          // No base.
    true,                             // Container.
    &value_traits<T>::value_type,     // Element type.
    &default_dtor<vector<T>>,
    &default_copy_ctor<vector<T>>,
    &default_copy_assign<vector<T>>,
    &vector_assign<T>,
    &vector_append<T>,
    &vector_prepend<T>,
    &vector_reverse<T>,
    nullptr,                          // No cast (cast data_ directly).
    &vector_compare<T>,
    &default_empty<vector<T>>,
    &vector_subscript<T>,
    &vector_iterate<T>
  };

  // vector<pair<K, V>> value
  //
  template <typename K, typename V>
  void
  pair_vector_append (value& v, names&& ns, const variable* var)
  {
    vector<pair<K, V>>& p (v
                           ? v.as<vector<pair<K, V>>> ()
                           : *new (&v.data_) vector<pair<K, V>> ());

    // Verify we have a sequence of pairs and convert each lhs/rhs to K/V.
    //
    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      name& l (*i);
      name* r (l.pair ? &*++i : nullptr);

      p.push_back (value_traits<pair<K, V>>::convert (
                     move (l), r,
                     value_traits<vector<pair<K, V>>>::value_type.name,
                     "element",
                     var));

    }
  }

  template <typename K, typename V>
  void
  pair_vector_assign (value& v, names&& ns, const variable* var)
  {
    if (v)
      v.as<vector<pair<K, V>>> ().clear ();

    pair_vector_append<K, V> (v, move (ns), var);
  }

  template <typename K, typename V>
  names_view
  pair_vector_reverse (const value& v, names& s, bool)
  {
    auto& vv (v.as<vector<pair<K, V>>> ());
    s.reserve (2 * vv.size ());

    for (const auto& p: vv)
      value_traits<pair<K, V>>::reverse (p.first, p.second, s);

    return s;
  }

  template <typename K, typename V>
  int
  pair_vector_compare (const value& l, const value& r)
  {
    auto& lv (l.as<vector<pair<K, V>>> ());
    auto& rv (r.as<vector<pair<K, V>>> ());

    auto li (lv.begin ()), le (lv.end ());
    auto ri (rv.begin ()), re (rv.end ());

    for (; li != le && ri != re; ++li, ++ri)
    {
      if (int r = value_traits<pair<K, V>>::compare (*li, *ri))
        return r;
    }

    if (li == le && ri != re) // l shorter than r.
      return -1;

    if (ri == re && li != le) // r shorter than l.
      return 1;

    return 0;
  }

  // Make sure these are static-initialized together. Failed that VC will make
  // sure it's done in the wrong order.
  //
  template <typename K, typename V>
  struct pair_vector_value_type: value_type
  {
    string type_name;

    pair_vector_value_type (value_type&& v)
        : value_type (move (v))
    {
      // vector<pair<K,V>>
      //
      type_name  = "vector<pair<";
      type_name += value_traits<K>::type_name;
      type_name += ',';
      type_name += value_traits<V>::type_name;
      type_name += ">>";
      name = type_name.c_str ();
    }
  };

  // This is beyond our static initialization order control skills, so we hack
  // it up for now.
  //
  template <typename K, typename V>
  struct pair_vector_value_type<K, optional<V>>: value_type
  {
    string type_name;

    pair_vector_value_type (value_type&& v)
        : value_type (move (v))
    {
      // vector<pair<K,optional<V>>>
      //
      type_name  = "vector<pair<";
      type_name += value_traits<K>::type_name;
      type_name += ",optional<";
      type_name += value_traits<V>::type_name;
      type_name += ">>>";
      name = type_name.c_str ();
    }
  };

  template <typename K, typename V>
  struct pair_vector_value_type<optional<K>, V>: value_type
  {
    string type_name;

    pair_vector_value_type (value_type&& v)
        : value_type (move (v))
    {
      // vector<pair<optional<K>,V>>
      //
      type_name  = "vector<pair<optional<";
      type_name += value_traits<K>::type_name;
      type_name += ">,";
      type_name += value_traits<V>::type_name;
      type_name += ">>";
      name = type_name.c_str ();
    }
  };

  template <typename K, typename V>
  const vector<pair<K, V>> value_traits<vector<pair<K, V>>>::empty_instance;

  template <typename K, typename V>
  const pair_vector_value_type<K, V>
  value_traits<vector<pair<K, V>>>::value_type = build2::value_type // VC14 wants =
  {
    nullptr,                     // Patched above.
    sizeof (vector<pair<K, V>>),
    nullptr,                     // No base.
    true,                        // Container.
    nullptr,                     // No element (not named).
    &default_dtor<vector<pair<K, V>>>,
    &default_copy_ctor<vector<pair<K, V>>>,
    &default_copy_assign<vector<pair<K, V>>>,
    &pair_vector_assign<K, V>,
    &pair_vector_append<K, V>,
    &pair_vector_append<K, V>,   // Prepend is the same as append.
    &pair_vector_reverse<K, V>,
    nullptr,                     // No cast (cast data_ directly).
    &pair_vector_compare<K, V>,
    &default_empty<vector<pair<K, V>>>,
    nullptr,                     // Subscript.
    nullptr                      // Iterate.
  };

  // set<T> value
  //
  template <typename T>
  set<T> value_traits<set<T>>::
  convert (names&& ns)
  {
    set<T> s;

    // Similar to set_append() below except we throw instead of issuing
    // diagnostics.
    //
    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      name& n (*i);
      name* r (nullptr);

      if (n.pair)
      {
        r = &*++i;

        if (n.pair != '@')
          throw invalid_argument (
            string ("invalid pair character: '") + n.pair + '\'');
      }

      s.insert (value_traits<T>::convert (move (n), r));
    }

    return s;
  }

  template <typename T>
  void
  set_append (value& v, names&& ns, const variable* var)
  {
    set<T>& s (v ? v.as<set<T>> () : *new (&v.data_) set<T> ());

    // Convert each element to T while merging pairs.
    //
    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      name& n (*i);
      name* r (nullptr);

      if (n.pair)
      {
        r = &*++i;

        if (n.pair != '@')
        {
          diag_record dr (fail);

          dr << "unexpected pair style for "
             << value_traits<T>::value_type.name << " value "
             << "'" << n << "'" << n.pair << "'" << *r << "'";

          if (var != nullptr)
            dr << " in variable " << var->name;
        }
      }

      try
      {
        s.insert (value_traits<T>::convert (move (n), r));
      }
      catch (const invalid_argument& e)
      {
        diag_record dr (fail);

        dr << e;
        if (var != nullptr)
          dr << " in variable " << var->name;

        dr << info << "while converting ";
        if (n.pair)
          dr << " element pair '" << n << "'@'" << *r << "'";
        else
          dr << " element '" << n << "'";
      }
    }
  }

  template <typename T>
  void
  set_assign (value& v, names&& ns, const variable* var)
  {
    if (v)
      v.as<set<T>> ().clear ();

    set_append<T> (v, move (ns), var);
  }

  template <typename T>
  names_view
  set_reverse (const value& v, names& s, bool)
  {
    auto& sv (v.as<set<T>> ());
    s.reserve (sv.size ());

    for (const T& x: sv)
      s.push_back (value_traits<T>::reverse (x));

    return s;
  }

  template <typename T>
  int
  set_compare (const value& l, const value& r)
  {
    auto& ls (l.as<set<T>> ());
    auto& rs (r.as<set<T>> ());

    auto li (ls.begin ()), le (ls.end ());
    auto ri (rs.begin ()), re (rs.end ());

    for (; li != le && ri != re; ++li, ++ri)
      if (int r = value_traits<T>::compare (*li, *ri))
        return r;

    if (li == le && ri != re) // l shorter than r.
      return -1;

    if (ri == re && li != le) // r shorter than l.
      return 1;

    return 0;
  }

  // Map subscript to set::contains().
  //
  template <typename T>
  value
  set_subscript (const value& val, value*,
                 value&& sub,
                 const location& sloc,
                 const location& bloc)
  {
    // Process subscript even if the value is null to make sure it is valid.
    //
    T k;
    try
    {
      k = convert<T> (move (sub));
    }
    catch (const invalid_argument& e)
    {
      fail (sloc) << "invalid " << value_traits<set<T>>::value_type.name
                  << " value subscript: " << e <<
        info (bloc) << "use the '\\[' escape sequence if this is a "
                    << "wildcard pattern";
    }

    bool r (false);
    if (!val.null)
    {
      const auto& s (val.as<set<T>> ());
      r = s.find (k) != s.end ();
    }

    return value (r);
  }

  // Provide iterate for set<T> for efficiency.
  //
  template <typename T>
  void
  set_iterate (const value& val,
               const function<void (value&&, bool first)>& f)
  {
    const auto& v (val.as<set<T>> ()); // Never NULL.

    for (auto b (v.begin ()), i (b), e (v.end ()); i != e; ++i)
    {
      f (value (*i), i == b);
    }
  }

  // Make sure these are static-initialized together. Failed that VC will make
  // sure it's done in the wrong order.
  //
  template <typename T>
  struct set_value_type: value_type
  {
    string type_name;

    set_value_type (value_type&& v)
        : value_type (move (v))
    {
      // set<T>
      //
      type_name  = "set<";
      type_name += value_traits<T>::type_name;
      type_name += '>';
      name = type_name.c_str ();
    }
  };

  // Convenience aliases for certain set<T> cases.
  //
  template <>
  struct set_value_type<string>: value_type
  {
    set_value_type (value_type&& v)
        : value_type (move (v))
    {
      name = "string_set";
    }
  };

  template <typename T>
  const set<T> value_traits<set<T>>::empty_instance;

  template <typename T>
  const set_value_type<T>
  value_traits<set<T>>::value_type = build2::value_type // VC14 wants =.
  {
    nullptr,                          // Patched above.
    sizeof (set<T>),
    nullptr,                          // No base.
    true,                             // Container.
    &value_traits<T>::value_type,     // Element type.
    &default_dtor<set<T>>,
    &default_copy_ctor<set<T>>,
    &default_copy_assign<set<T>>,
    &set_assign<T>,
    &set_append<T>,
    &set_append<T>,                   // Prepend the same as append.
    &set_reverse<T>,
    nullptr,                          // No cast (cast data_ directly).
    &set_compare<T>,
    &default_empty<set<T>>,
    &set_subscript<T>,
    &set_iterate<T>
  };

  // map<K, V> value
  //
  template <typename K, typename V>
  void
  map_append (value& v, names&& ns, const variable* var)
  {
    map<K, V>& p (v
                  ? v.as<map<K, V>> ()
                  : *new (&v.data_) map<K, V> ());

    // Verify we have a sequence of pairs and convert each lhs/rhs to K/V.
    //
    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      name& l (*i);
      name* r (l.pair ? &*++i : nullptr);

      pair<K, V> v (value_traits<pair<K, V>>::convert (
                      move (l), r,
                      value_traits<map<K, V>>::value_type.name,
                      "element",
                      var));

      // Poor man's emplace_or_assign().
      //
      p.emplace (move (v.first), V ()).first->second = move (v.second);
    }
  }

  template <typename K, typename V>
  void
  map_prepend (value& v, names&& ns, const variable* var)
  {
    map<K, V>& p (v
                  ? v.as<map<K, V>> ()
                  : *new (&v.data_) map<K, V> ());

    // Verify we have a sequence of pairs and convert each lhs/rhs to K/V.
    //
    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      name& l (*i);
      name* r (l.pair ? &*++i : nullptr);

      pair<K, V> v (value_traits<pair<K, V>>::convert (
                      move (l), r,
                      value_traits<map<K, V>>::value_type.name,
                      "element",
                      var));

      p.emplace (move (v.first), move (v.second));
    }
  }

  template <typename K, typename V>
  void
  map_assign (value& v, names&& ns, const variable* var)
  {
    if (v)
      v.as<map<K, V>> ().clear ();

    map_append<K, V> (v, move (ns), var);
  }

  template <typename K, typename V>
  names_view
  map_reverse (const value& v, names& s, bool)
  {
    auto& vm (v.as<map<K, V>> ());
    s.reserve (2 * vm.size ());

    for (const auto& p: vm)
      value_traits<pair<K, V>>::reverse (p.first, p.second, s);

    return s;
  }

  template <typename K, typename V>
  int
  map_compare (const value& l, const value& r)
  {
    auto& lm (l.as<map<K, V>> ());
    auto& rm (r.as<map<K, V>> ());

    auto li (lm.begin ()), le (lm.end ());
    auto ri (rm.begin ()), re (rm.end ());

    for (; li != le && ri != re; ++li, ++ri)
    {
      if (int r = value_traits<pair<const K, V>>::compare (*li, *ri))
        return r;
    }

    if (li == le && ri != re) // l shorter than r.
      return -1;

    if (ri == re && li != le) // r shorter than l.
      return 1;

    return 0;
  }

  // Note that unlike json_value, we don't provide index support for maps.
  // There are two reasons for this: Firstly, consider map<uint64_t,...>.
  // Secondly, even something like map<string,...> may contain integers as
  // keys (in JSON, there is a strong convention for object member names not
  // to be integers). Instead, we provide the $keys() function which allows
  // one to implement an index-based access with a bit of overhead, if needed.
  //
  template <typename K, typename V>
  value
  map_subscript (const value& val, value* val_data,
                 value&& sub,
                 const location& sloc,
                 const location& bloc)
  {
    // Process subscript even if the value is null to make sure it is valid.
    //
    K k;
    try
    {
      k = convert<K> (move (sub));
    }
    catch (const invalid_argument& e)
    {
      fail (sloc) << "invalid " << value_traits<map<K, V>>::value_type.name
                  << " value subscript: " << e <<
        info (bloc) << "use the '\\[' escape sequence if this is a "
                    << "wildcard pattern";
    }

    value r;
    if (!val.null)
    {
      const auto& m (val.as<map<K, V>> ());
      auto i (m.find (k));
      if (i != m.end ())
      {
        // Steal the value if possible.
        //
        r = (&val == val_data
             ? V (move (const_cast<V&> (i->second)))
             : V (i->second));
      }
    }

    // Typify null values so that type-specific subscript (e.g., for
    // json_value) gets called for chained subscripts.
    //
    if (r.null)
      r.type = &value_traits<V>::value_type;

    return r;
  }

  // Make sure these are static-initialized together. Failed that VC will make
  // sure it's done in the wrong order.
  //
  template <typename K, typename V>
  struct map_value_type: value_type
  {
    string type_name;

    map_value_type (value_type&& v)
        : value_type (move (v))
    {
      // map<K,V>
      //
      type_name  = "map<";
      type_name += value_traits<K>::type_name;
      type_name += ',';
      type_name += value_traits<V>::type_name;
      type_name += '>';
      name = type_name.c_str ();
      subscript = &map_subscript<K, V>;
    }
  };

  // This is beyond our static initialization order control skills, so we hack
  // it up for now.
  //
  template <typename K, typename V>
  struct map_value_type<K, optional<V>>: value_type
  {
    string type_name;

    map_value_type (value_type&& v)
        : value_type (move (v))
    {
      // map<K,optional<V>>
      //
      type_name  = "map<";
      type_name += value_traits<K>::type_name;
      type_name += ",optional<";
      type_name += value_traits<V>::type_name;
      type_name += ">>";
      name = type_name.c_str ();
      // @@ TODO: subscript
    }
  };

  template <typename K, typename V>
  struct map_value_type<optional<K>, V>: value_type
  {
    string type_name;

    map_value_type (value_type&& v)
        : value_type (move (v))
    {
      // map<optional<K>,V>
      //
      type_name  = "map<optional<";
      type_name += value_traits<K>::type_name;
      type_name += ">,";
      type_name += value_traits<V>::type_name;
      type_name += '>';
      name = type_name.c_str ();
      // @@ TODO: subscript
    }
  };

  // Convenience aliases for certain map<T,T> cases.
  //
  template <>
  struct map_value_type<string, string>: value_type
  {
    map_value_type (value_type&& v)
        : value_type (move (v))
    {
      name = "string_map";
      subscript = &map_subscript<string, string>;
    }
  };

  template <typename K, typename V>
  const map<K, V> value_traits<map<K, V>>::empty_instance;

  // Note that custom iteration would be better (more efficient, return typed
  // value), but we don't yet have pair<> as value type so we let the generic
  // implementation return an untyped pair.
  //
  // BTW, one negative consequence of returning untyped pair is that
  // $first()/$second() don't return types values either, which is quite
  // unfortunate for something like json_map.
  //
  template <typename K, typename V>
  const map_value_type<K, V>
  value_traits<map<K, V>>::value_type = build2::value_type // VC14 wants =
  {
    nullptr,             // Patched above.
    sizeof (map<K, V>),
    nullptr,             // No base.
    true,                // Container.
    nullptr,             // No element (pair<> not a value type yet).
    &default_dtor<map<K, V>>,
    &default_copy_ctor<map<K, V>>,
    &default_copy_assign<map<K, V>>,
    &map_assign<K, V>,
    &map_append<K, V>,
    &map_prepend<K, V>,
    &map_reverse<K, V>,
    nullptr,             // No cast (cast data_ directly).
    &map_compare<K, V>,
    &default_empty<map<K, V>>,
    nullptr,             // Subscript (patched in by map_value_type above).
    nullptr              // Iterate.
  };

  // variable_cache
  //
  template <typename K>
  pair<value&, ulock> variable_cache<K>::
  insert (context& ctx,
          K k,
          const lookup& stem,
          size_t bver,
          const variable& var)
  {
    using value_data = variable_map::value_data;

    const variable_map* svars (stem.vars); // NULL if undefined.
    size_t sver (stem.defined ()
                 ? static_cast<const value_data*> (stem.value)->version
                 : 0);

    shared_mutex& m (
      ctx.mutexes->variable_cache[
        hash<variable_cache*> () (this) % ctx.mutexes->variable_cache_size]);

    slock sl (m);
    ulock ul (m, defer_lock);

    auto i (m_.find (k));

    // Cache hit.
    //
    if (i != m_.end ()                 &&
        i->second.base_version == bver &&
        i->second.stem_vars == svars   &&
        i->second.stem_version == sver &&
        (var.type == nullptr || i->second.value.type == var.type))
      return pair<value&, ulock> (i->second.value, move (ul));

    // Relock for exclusive access. Note that it is entirely possible
    // that between unlock and lock someone else has updated the entry.
    //
    sl.unlock ();
    ul.lock ();

    // Note that the cache entries are never removed so we can reuse the
    // iterator.
    //
    pair<typename map_type::iterator, bool> p (i, i == m_.end ());

    if (p.second)
      p = m_.emplace (move (k),
                      entry_type {value_data (nullptr), bver, svars, sver});

    entry_type& e (p.first->second);

    if (p.second)
    {
      // Cache miss.
      //
      e.value.version++; // New value.
    }
    else if (e.base_version != bver  ||
             e.stem_vars    != svars ||
             e.stem_version != sver)
    {
      // Cache invalidation.
      //
      assert (e.base_version <= bver);
      e.base_version = bver;

      if (e.stem_vars != svars)
        e.stem_vars = svars;
      else
        assert (e.stem_version <= sver);

      e.stem_version = sver;

      e.value.extra = 0; // For consistency (we don't really use it).
      e.value.version++; // Value changed.
    }
    else
    {
      // Cache hit.
      //
      if (var.type != nullptr && e.value.type != var.type)
        typify (e.value, *var.type, &var);

      ul.unlock ();
    }

    return pair<value&, ulock> (e.value, move (ul));
  }
}
