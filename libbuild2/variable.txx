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

  // This one will be SFINAE'd out unless T is a simple value.
  //
  template <typename T>
  auto
  convert (names&& ns) ->
    decltype (value_traits<T>::convert (move (ns[0]), nullptr))
  {
    size_t n (ns.size ());

    if (n == 0)
    {
      if (value_traits<T>::empty_value)
        return T ();
    }
    else if (n == 1)
    {
      return convert<T> (move (ns[0]));
    }
    else if (n == 2 && ns[0].pair != '\0')
    {
      return convert<T> (move (ns[0]), move (ns[1]));
    }

    throw invalid_argument (
      string ("invalid ") + value_traits<T>::type_name +
      (n == 0 ? " value: empty" : " value: multiple names"));
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
  static names_view
  vector_reverse (const value& v, names& s, bool)
  {
    auto& vv (v.as<vector<T>> ());
    s.reserve (vv.size ());

    for (const T& x: vv)
      s.push_back (value_traits<T>::reverse (x));

    return s;
  }

  template <typename T>
  static int
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
    &default_empty<vector<T>>
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
  static names_view
  pair_vector_reverse (const value& v, names& s, bool)
  {
    auto& vv (v.as<vector<pair<K, V>>> ());
    s.reserve (2 * vv.size ());

    for (const auto& p: vv)
      value_traits<pair<K, V>>::reverse (p.first, p.second, s);

    return s;
  }

  template <typename K, typename V>
  static int
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
      type_name  = value_traits<K>::type_name;
      type_name += '_';
      type_name += value_traits<V>::type_name;
      type_name += "_pair_vector";
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
      type_name  = value_traits<K>::type_name;
      type_name += "_optional_";
      type_name += value_traits<V>::type_name;
      type_name += "_pair_vector";
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
      type_name  = "optional_";
      type_name += value_traits<K>::type_name;
      type_name += '_';
      type_name += value_traits<V>::type_name;
      type_name += "_pair_vector";
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
    &default_empty<vector<pair<K, V>>>
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

      p.emplace (move (v.first), move (v.second));
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

      // Poor man's emplace_or_assign().
      //
      p.emplace (move (v.first), V ()).first->second = move (v.second);
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
  static names_view
  map_reverse (const value& v, names& s, bool)
  {
    auto& vm (v.as<map<K, V>> ());
    s.reserve (2 * vm.size ());

    for (const auto& p: vm)
      value_traits<pair<K, V>>::reverse (p.first, p.second, s);

    return s;
  }

  template <typename K, typename V>
  static int
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
      type_name  = value_traits<K>::type_name;
      type_name += '_';
      type_name += value_traits<V>::type_name;
      type_name += "_map";
      name = type_name.c_str ();
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
      type_name  = value_traits<K>::type_name;
      type_name += "_optional_";
      type_name += value_traits<V>::type_name;
      type_name += "_map";
      name = type_name.c_str ();
    }
  };

  template <typename K, typename V>
  struct map_value_type<optional<K>, V>: value_type
  {
    string type_name;

    map_value_type (value_type&& v)
        : value_type (move (v))
    {
      type_name  = "optional_";
      type_name += value_traits<K>::type_name;
      type_name += '_';
      type_name += value_traits<V>::type_name;
      type_name += "_map";
      name = type_name.c_str ();
    }
  };

  template <typename K, typename V>
  const map<K, V> value_traits<map<K, V>>::empty_instance;

  template <typename K, typename V>
  const map_value_type<K, V>
  value_traits<map<K, V>>::value_type = build2::value_type // VC14 wants =
  {
    nullptr,             // Patched above.
    sizeof (map<K, V>),
    nullptr,             // No base.
    true,                // Container.
    nullptr,             // No element (not named).
    &default_dtor<map<K, V>>,
    &default_copy_ctor<map<K, V>>,
    &default_copy_assign<map<K, V>>,
    &map_assign<K, V>,
    &map_append<K, V>,
    &map_prepend<K, V>,
    &map_reverse<K, V>,
    nullptr,             // No cast (cast data_ directly).
    &map_compare<K, V>,
    &default_empty<map<K, V>>
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
      ctx.mutexes.variable_cache[
        hash<variable_cache*> () (this) % ctx.mutexes.variable_cache_size]);

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
