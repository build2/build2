// file      : libbuild2/variable.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <type_traits> // is_same

#include <libbuild2/export.hxx>

namespace build2
{
  // value_type
  //
  template <typename T>
  inline const value_type* value_type::
  is_a () const
  {
    const value_type* b (this);
    for (;
         b != nullptr && b != &value_traits<T>::value_type;
         b = b->base_type) ;
    return b;
  }

  // value
  //
  inline bool value::
  empty () const
  {
    assert (!null);
    return type == nullptr
      ? as<names> ().empty ()
      : type->empty == nullptr ? false : type->empty (*this);
  }

  inline value::
  value (names ns)
      : type (nullptr), null (false), extra (0)
  {
    new (&data_) names (move (ns));
  }

  inline value::
  value (optional<names> ns)
      : type (nullptr), null (!ns), extra (0)
  {
    if (!null)
      new (&data_) names (move (*ns));
  }

  template <typename T>
  inline value::
  value (T v)
      : type (&value_traits<T>::value_type), null (true), extra (0)
  {
    value_traits<T>::assign (*this, move (v));
    null = false;
  }

  template <typename T>
  inline value::
  value (optional<T> v)
      : type (&value_traits<T>::value_type), null (true), extra (0)
  {
    if (v)
    {
      value_traits<T>::assign (*this, move (*v));
      null = false;
    }
  }

  inline value& value::
  operator= (reference_wrapper<value> v)
  {
    return *this = v.get ();
  }

  inline value& value::
  operator= (reference_wrapper<const value> v)
  {
    return *this = v.get ();
  }

  template <typename T>
  inline value& value::
  operator= (T v)
  {
    assert (type == &value_traits<T>::value_type || type == nullptr);

    // Prepare the receiving value.
    //
    if (type == nullptr)
    {
      *this = nullptr;
      type = &value_traits<T>::value_type;
    }

    value_traits<T>::assign (*this, move (v));
    null = false;
    return *this;
  }

  template <typename T>
  inline value& value::
  operator+= (T v)
  {
    assert (type == &value_traits<T>::value_type || (type == nullptr && null));

    // Prepare the receiving value.
    //
    if (type == nullptr)
      type = &value_traits<T>::value_type;

    value_traits<T>::append (*this, move (v));
    null = false;
    return *this;
  }

  template <typename T>
  inline value& value::
  prepend (T v)
  {
    assert (type == &value_traits<T>::value_type || (type == nullptr && null));

    // Prepare the receiving value.
    //
    if (type == nullptr)
      type = &value_traits<T>::value_type;

    value_traits<T>::prepend (*this, move (v));
    null = false;
    return *this;
  }

  inline value& value::
  operator= (names v)
  {
    assert (type == nullptr);
    assign (move (v), nullptr);
    return *this;
  }

  inline value& value::
  operator+= (names v)
  {
    assert (type == nullptr);
    append (move (v), nullptr);
    return *this;
  }

  inline void value::
  assign (name&& n, const variable* var)
  {
    names ns;
    ns.push_back (move (n));
    assign (move (ns), var);
  }

  inline bool
  operator!= (const value& x, const value& y)
  {
    return !(x == y);
  }

  inline bool
  operator<= (const value& x, const value& y)
  {
    return !(x > y);
  }

  inline bool
  operator>= (const value& x, const value& y)
  {
    return !(x < y);
  }

  template <>
  inline const names&
  cast (const value& v)
  {
    assert (v && v.type == nullptr);
    return v.as<names> ();
  }

  template <>
  inline names&
  cast (value& v)
  {
    assert (v && v.type == nullptr);
    return v.as<names> ();
  }

  template <typename T>
  inline const T&
  cast (const value& v)
  {
    assert (v);

    // Find base if any.
    //
    // Note that here we use the value type address as type identity.
    //
    const value_type* b (v.type->is_a<T> ());
    assert (b != nullptr);

    return *static_cast<const T*> (v.type->cast == nullptr
                                   ? static_cast<const void*> (&v.data_)
                                   : v.type->cast (v, b));
  }

  template <typename T>
  inline T&
  cast (value& v)
  {
    // Forward to const T&.
    //
    return const_cast<T&> (cast<T> (static_cast <const value&> (v)));
  }

  template <typename T>
  inline T&&
  cast (value&& v)
  {
    return move (cast<T> (v)); // Forward to T&.
  }

  template <typename T>
  inline const T&
  cast (lookup l)
  {
    return cast<T> (*l);
  }

  template <typename T>
  inline T*
  cast_null (value& v)
  {
    return v ? &cast<T> (v) : nullptr;
  }

  template <typename T>
  inline const T*
  cast_null (const value& v)
  {
    return v ? &cast<T> (v) : nullptr;
  }

  template <typename T>
  inline const T*
  cast_null (lookup l)
  {
    return l ? &cast<T> (*l) : nullptr;
  }

  template <typename T>
  inline const T&
  cast_empty (const value& v)
  {
    return v ? cast<T> (v) : value_traits<T>::empty_instance;
  }

  template <typename T>
  inline const T&
  cast_empty (lookup l)
  {
    return l ? cast<T> (l) : value_traits<T>::empty_instance;
  }

  template <typename T>
  inline T
  cast_default (const value& v, const T& d)
  {
    return v ? cast<T> (v) : d;
  }

  template <typename T>
  inline T
  cast_default (lookup l, const T& d)
  {
    return l ? cast<T> (l) : d;
  }

  template <typename T>
  inline T
  cast_false (const value& v)
  {
    return v && cast<T> (v);
  }

  template <typename T>
  inline T
  cast_false (lookup l)
  {
    return l && cast<T> (l);
  }

  template <typename T>
  inline T
  cast_true (const value& v)
  {
    return !v || cast<T> (v);
  }

  template <typename T>
  inline T
  cast_true (lookup l)
  {
    return !l || cast<T> (l);
  }

  template <typename T>
  inline void
  typify (value& v, const variable* var)
  {
    const value_type& t (value_traits<T>::value_type);

    if (v.type != &t)
      typify (v, t, var);
  }

  LIBBUILD2_SYMEXPORT void
  typify (value&, const value_type&, const variable*, memory_order);

  inline void
  typify (value& v, const value_type& t, const variable* var)
  {
    typify (v, t, var, memory_order_relaxed);
  }

  inline vector_view<const name>
  reverse (const value& v, names& storage, bool reduce)
  {
    assert (v &&
            storage.empty () &&
            (v.type == nullptr || v.type->reverse != nullptr));

    return v.type == nullptr
      ? v.as<names> ()
      : v.type->reverse (v, storage, reduce);
  }

  inline vector_view<name>
  reverse (value& v, names& storage, bool reduce)
  {
    names_view cv (reverse (static_cast<const value&> (v), storage, reduce));
    return vector_view<name> (const_cast<name*> (cv.data ()), cv.size ());
  }

  // value_traits
  //
  template <typename T>
  inline T
  convert (name&& n)
  {
    return value_traits<T>::convert (move (n), nullptr);
  }

  template <typename T>
  inline T
  convert (name&& l, name&& r)
  {
    return value_traits<T>::convert (move (l), &r);
  }

  // This one will be SFINAE'd out unless T is a container.
  //
  // If T is both (e.g., json_value), then make this version preferable.
  //
  template <typename T>
  inline auto
  convert_impl (names&& ns, int)
    -> decltype (value_traits<T>::convert (move (ns)))
  {
    return value_traits<T>::convert (move (ns));
  }

  // This one will be SFINAE'd out unless T is a simple value.
  //
  // If T is both (e.g., json_value), then make this version less preferable.
  //
  template <typename T>
  auto // NOTE: not inline!
  convert_impl (names&& ns, ...) ->
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

  template <typename T>
  inline T
  convert (names&& ns)
  {
    return convert_impl<T> (move (ns), 0);
  }

  // bool value
  //
  inline void value_traits<bool>::
  assign (value& v, bool x)
  {
    if (v)
      v.as<bool> () = x;
    else
      new (&v.data_) bool (x);
  }

  inline void value_traits<bool>::
  append (value& v, bool x)
  {
    // Logical OR.
    //
    if (v)
      v.as<bool> () = v.as<bool> () || x;
    else
      new (&v.data_) bool (x);
  }

  inline int value_traits<bool>::
  compare (bool l, bool r)
  {
    return l < r ? -1 : (l > r ? 1 : 0);
  }

  // int64_t value
  //
  inline void value_traits<int64_t>::
  assign (value& v, int64_t x)
  {
    if (v)
      v.as<int64_t> () = x;
    else
      new (&v.data_) int64_t (x);
  }

  inline void value_traits<int64_t>::
  append (value& v, int64_t x)
  {
    // ADD.
    //
    if (v)
      v.as<int64_t> () += x;
    else
      new (&v.data_) int64_t (x);
  }

  inline int value_traits<int64_t>::
  compare (int64_t l, int64_t r)
  {
    return l < r ? -1 : (l > r ? 1 : 0);
  }

  // uint64_t value
  //
  inline void value_traits<uint64_t>::
  assign (value& v, uint64_t x)
  {
    if (v)
      v.as<uint64_t> () = x;
    else
      new (&v.data_) uint64_t (x);
  }

  inline void value_traits<uint64_t>::
  append (value& v, uint64_t x)
  {
    // ADD.
    //
    if (v)
      v.as<uint64_t> () += x;
    else
      new (&v.data_) uint64_t (x);
  }

  inline int value_traits<uint64_t>::
  compare (uint64_t l, uint64_t r)
  {
    return l < r ? -1 : (l > r ? 1 : 0);
  }

  // string value
  //
  inline void value_traits<string>::
  assign (value& v, string&& x)
  {
    if (v)
      v.as<string> () = move (x);
    else
      new (&v.data_) string (move (x));
  }

  inline void value_traits<string>::
  append (value& v, string&& x)
  {
    if (v)
    {
      string& s (v.as<string> ());

      if (s.empty ())
        s.swap (x);
      else
        s += x;
    }
    else
      new (&v.data_) string (move (x));
  }

  inline void value_traits<string>::
  prepend (value& v, string&& x)
  {
    if (v)
    {
      string& s (v.as<string> ());

      if (!s.empty ())
        x += s;

      s.swap (x);
    }
    else
      new (&v.data_) string (move (x));
  }

  inline int value_traits<string>::
  compare (const string& l, const string& r)
  {
    return l.compare (r);
  }

  // path value
  //
  inline void value_traits<path>::
  assign (value& v, path&& x)
  {
    if (v)
      v.as<path> () = move (x);
    else
      new (&v.data_) path (move (x));
  }

  inline void value_traits<path>::
  append (value& v, path&& x)
  {
    if (v)
    {
      path& p (v.as<path> ());

      if (p.empty ())
        p.swap (x);
      else
        p /= x;
    }
    else
      new (&v.data_) path (move (x));
  }

  inline void value_traits<path>::
  prepend (value& v, path&& x)
  {
    if (v)
    {
      path& p (v.as<path> ());

      if (!p.empty ())
        x /= p;

      p.swap (x);
    }
    else
      new (&v.data_) path (move (x));
  }

  inline int value_traits<path>::
  compare (const path& l, const path& r)
  {
    return l.compare (r);
  }

  // dir_path value
  //
  inline void value_traits<dir_path>::
  assign (value& v, dir_path&& x)
  {
    if (v)
      v.as<dir_path> () = move (x);
    else
      new (&v.data_) dir_path (move (x));
  }

  inline void value_traits<dir_path>::
  append (value& v, dir_path&& x)
  {
    if (v)
    {
      dir_path& p (v.as<dir_path> ());

      if (p.empty ())
        p.swap (x);
      else
        p /= x;
    }
    else
      new (&v.data_) dir_path (move (x));
  }

  inline void value_traits<dir_path>::
  prepend (value& v, dir_path&& x)
  {
    if (v)
    {
      dir_path& p (v.as<dir_path> ());

      if (!p.empty ())
        x /= p;

      p.swap (x);
    }
    else
      new (&v.data_) dir_path (move (x));
  }

  inline int value_traits<dir_path>::
  compare (const dir_path& l, const dir_path& r)
  {
    return l.compare (r);
  }

  // abs_dir_path value
  //
  inline void value_traits<abs_dir_path>::
  assign (value& v, abs_dir_path&& x)
  {
    if (v)
      v.as<abs_dir_path> () = move (x);
    else
      new (&v.data_) abs_dir_path (move (x));
  }

  inline void value_traits<abs_dir_path>::
  append (value& v, abs_dir_path&& x)
  {
    if (v)
    {
      abs_dir_path& p (v.as<abs_dir_path> ());

      if (p.empty ())
        p.swap (x);
      else
        p /= x;
    }
    else
      new (&v.data_) abs_dir_path (move (x));
  }

  inline int value_traits<abs_dir_path>::
  compare (const abs_dir_path& l, const abs_dir_path& r)
  {
    return l.compare (static_cast<const dir_path&> (r));
  }

  // name value
  //
  inline void value_traits<name>::
  assign (value& v, name&& x)
  {
    if (v)
      v.as<name> () = move (x);
    else
      new (&v.data_) name (move (x));
  }

  // name_pair value
  //
  inline void value_traits<name_pair>::
  assign (value& v, name_pair&& x)
  {
    if (v)
      v.as<name_pair> () = move (x);
    else
      new (&v.data_) name_pair (move (x));
  }

  inline int value_traits<name_pair>::
  compare (const name_pair& x, const name_pair& y)
  {
    int r (x.first.compare (y.first));

    if (r == 0)
      r = x.second.compare (y.second);

    return r;
  }

  // process_path value
  //
  inline void value_traits<process_path>::
  assign (value& v, process_path&& x)
  {
    // Convert the value to its "self-sufficient" form (see also below).
    //
    if (x.recall.empty ())
      x.recall = path (x.initial);

    x.initial = x.recall.string ().c_str ();

    if (v)
      v.as<process_path> () = move (x);
    else
      new (&v.data_) process_path (move (x));
  }

  inline int value_traits<process_path>::
  compare (const process_path& x, const process_path& y)
  {
    int r (x.recall.compare (y.recall));

    if (r == 0)
      r = x.effect.compare (y.effect);

    return r;
  }

  // process_path_ex value
  //
  inline void value_traits<process_path_ex>::
  assign (value& v, process_path_ex&& x)
  {
    // Convert the value to its "self-sufficient" form (see also above).
    //
    if (x.recall.empty ())
      x.recall = path (x.initial);

    x.initial = x.recall.string ().c_str ();

    if (v)
      v.as<process_path_ex> () = move (x);
    else
      new (&v.data_) process_path_ex (move (x));
  }

  // target_triplet value
  //
  inline void value_traits<target_triplet>::
  assign (value& v, target_triplet&& x)
  {
    if (v)
      v.as<target_triplet> () = move (x);
    else
      new (&v.data_) target_triplet (move (x));
  }

  // project_name value
  //
  inline void value_traits<project_name>::
  assign (value& v, project_name&& x)
  {
    if (v)
      v.as<project_name> () = move (x);
    else
      new (&v.data_) project_name (move (x));
  }

  inline name value_traits<project_name>::
  reverse (const project_name& x)
  {
    // Make work for the special unnamed subproject representation (see
    // find_subprojects() in file.cxx for details).
    //
    const string& s (x.string ());
    return name (s.empty () || path::traits_type::is_separator (s.back ())
                 ? empty_string
                 : s);
  }

  // optional<T>
  //
  template <typename T>
  inline int value_traits<optional<T>>::
  compare (const optional<T>& l, const optional<T>& r)
  {
    return l
      ? (r ? value_traits<T>::compare (*l, *r) : 1)
      : (r ? -1 : 0);
  }

  // pair<F, S> value
  //
  template <typename F, typename S>
  inline int value_traits<pair<F, S>>::
  compare (const pair<F, S>& l, const pair<F, S>& r)
  {
    int i (value_traits<F>::compare (l.first, r.first));

    if (i == 0)
      i = value_traits<S>::compare (l.second, r.second);

    return i;
  }

  // vector<T> value
  //
  template <typename T>
  inline void value_traits<vector<T>>::
  assign (value& v, vector<T>&& x)
  {
    if (v)
      v.as<vector<T>> () = move (x);
    else
      new (&v.data_) vector<T> (move (x));
  }

  template <typename T>
  inline void value_traits<vector<T>>::
  append (value& v, vector<T>&& x)
  {
    if (v)
    {
      vector<T>& p (v.as<vector<T>> ());

      if (p.empty ())
        p.swap (x);
      else
        p.insert (p.end (),
                  make_move_iterator (x.begin ()),
                  make_move_iterator (x.end ()));
    }
    else
      new (&v.data_) vector<T> (move (x));
  }

  template <typename T>
  inline void value_traits<vector<T>>::
  prepend (value& v, vector<T>&& x)
  {
    if (v)
    {
      vector<T>& p (v.as<vector<T>> ());

      if (!p.empty ())
        x.insert (x.end (),
                  make_move_iterator (p.begin ()),
                  make_move_iterator (p.end ()));

      p.swap (x);
    }
    else
      new (&v.data_) vector<T> (move (x));
  }

  // vector<pair<K, V>> value
  //
  template <typename K, typename V>
  inline void value_traits<vector<pair<K, V>>>::
  assign (value& v, vector<pair<K, V>>&& x)
  {
    if (v)
      v.as<vector<pair<K, V>>> () = move (x);
    else
      new (&v.data_) vector<pair<K, V>> (move (x));
  }

  template <typename K, typename V>
  inline void value_traits<vector<pair<K, V>>>::
  append (value& v, vector<pair<K, V>>&& x)
  {
    if (v)
    {
      vector<pair<K, V>>& y (v.as<vector<pair<K, V>>> ());

      if (y.empty ())
        y.swap (x);
      else
        y.insert (y.end (),
                  make_move_iterator (x.begin ()),
                  make_move_iterator (x.end ()));
    }
    else
      new (&v.data_) vector<pair<K, V>> (move (x));
  }

  // set<T> value
  //
  template <typename T>
  inline void value_traits<set<T>>::
  assign (value& v, set<T>&& x)
  {
    if (v)
      v.as<set<T>> () = move (x);
    else
      new (&v.data_) set<T> (move (x));
  }

  template <typename T>
  inline void value_traits<set<T>>::
  append (value& v, set<T>&& x)
  {
    if (v)
    {
      set<T>& p (v.as<set<T>> ());

      if (p.empty ())
        p.swap (x);
      else
        // Keys (being const) can only be copied.
        //
        p.insert (x.begin (), x.end ());
    }
    else
      new (&v.data_) set<T> (move (x));
  }

  template <typename T>
  inline void value_traits<set<T>>::
  prepend (value& v, set<T>&& x)
  {
    append (v, move (x));
  }

  // map<K, V> value
  //
  template <typename K, typename V>
  inline void value_traits<map<K, V>>::
  assign (value& v, map<K, V>&& x)
  {
    if (v)
      v.as<map<K, V>> () = move (x);
    else
      new (&v.data_) map<K, V> (move (x));
  }

  template <typename K, typename V>
  inline void value_traits<map<K, V>>::
  append (value& v, map<K, V>&& x)
  {
    if (v)
    {
      map<K, V>& m (v.as<map<K, V>> ());

      if (m.empty ())
        m.swap (x);
      else
        // Note that this will only move values. Keys (being const) are still
        // copied.
        //
        m.insert (make_move_iterator (x.begin ()),
                  make_move_iterator (x.end ()));
    }
    else
      new (&v.data_) map<K, V> (move (x));
  }

  template <typename K, typename V>
  inline void value_traits<map<K, V>>::
  prepend (value& v, map<K, V>&& x)
  {
    if (v)
    {
      map<K, V>& m (v.as<map<K, V>> ());

      m.swap (x);

      // Note that this will only move values. Keys (being const) are still
      // copied.
      //
      m.insert (make_move_iterator (x.begin ()),
                make_move_iterator (x.end ()));
    }
    else
      new (&v.data_) map<K, V> (move (x));
  }

  // json
  //
  inline bool value_traits<json_value>::
  empty (const json_value& v)
  {
    // Note: should be consistent with $json.size().
    //
    switch (v.type)
    {
    case json_type::null:               return true;
    case json_type::boolean:
    case json_type::signed_number:
    case json_type::unsigned_number:
    case json_type::hexadecimal_number:
    case json_type::string:             break;
    case json_type::array:              return v.array.empty ();
    case json_type::object:             return v.object.empty ();
    }

    return false;
  }

  inline void value_traits<json_value>::
  assign (value& v, json_value&& x)
  {
    if (v)
      v.as<json_value> () = move (x);
    else
      new (&v.data_) json_value (move (x));
  }

  inline void value_traits<json_value>::
  append (value& v, json_value&& x)
  {
    if (v)
      v.as<json_value> ().append (move (x));
    else
      new (&v.data_) json_value (move (x));
  }

  inline void value_traits<json_value>::
  prepend (value& v, json_value&& x)
  {
    if (v)
      v.as<json_value> ().prepend (move (x));
    else
      new (&v.data_) json_value (move (x));
  }

  // json_array
  //
  inline void value_traits<json_array>::
  assign (value& v, json_array&& x)
  {
    if (v)
      v.as<json_array> () = move (x);
    else
      new (&v.data_) json_array (move (x));
  }

  inline void value_traits<json_array>::
  append (value& v, json_value&& x)
  {
    if (!v)
      new (&v.data_) json_array ();

    v.as<json_array> ().append (move (x));
  }

  inline void value_traits<json_array>::
  prepend (value& v, json_value&& x)
  {
    if (!v)
      new (&v.data_) json_array ();

    v.as<json_array> ().prepend (move (x));
  }

  // json_object
  //
  inline void value_traits<json_object>::
  assign (value& v, json_object&& x)
  {
    if (v)
      v.as<json_object> () = move (x);
    else
      new (&v.data_) json_object (move (x));
  }

  inline void value_traits<json_object>::
  append (value& v, json_value&& x)
  {
    if (!v)
      new (&v.data_) json_object ();

    v.as<json_object> ().append (move (x));
  }

  inline void value_traits<json_object>::
  prepend (value& v, json_value&& x)
  {
    if (!v)
      new (&v.data_) json_object ();

    v.as<json_object> ().prepend (move (x));
  }

  // variable_pool
  //
  inline const variable* variable_pool::
  find (const string& n) const
  {
    // The pool chaining semantics for lookup: first check own pool then, if
    // not found, check the outer pool.
    //
    auto i (map_.find (&n));
    if (i != map_.end ())
      return &i->second;

    if (outer_ != nullptr)
    {
      i = outer_->map_.find (&n);
      if (i != outer_->map_.end ())
        return &i->second;
    }

    return nullptr;
  }

  inline const variable& variable_pool::
  operator[] (const string& n) const
  {
    const variable* r (find (n));
    assert (r != nullptr);
    return *r;
  }

  // variable_map
  //
  inline void variable_map::
  typify (const value_data& v, const variable& var) const
  {
    // We assume typification is not modification so no version increment.
    //
    if (ctx->phase == run_phase::load)
    {
      if (v.type != var.type)
        build2::typify (const_cast<value_data&> (v), *var.type, &var);
    }
    else
    {
      if (v.type.load (memory_order_acquire) != var.type)
        typify_atomic (*ctx, const_cast<value_data&> (v), *var.type, &var);
    }
  }

  // variable_map::iterator_adapter
  //
  template <typename I>
  inline typename I::reference variable_map::iterator_adapter<I>::
  operator* () const
  {
    auto& r (I::operator* ());
    const variable& var (r.first);
    const value_data& val (r.second);

    // Check if this is the first access after being assigned a type.
    //
    if (var.type != nullptr)
      m_->typify (val, var);

    return r;
  }

  template <typename I>
  inline typename I::pointer variable_map::iterator_adapter<I>::
  operator-> () const
  {
    auto p (I::operator-> ());
    const variable& var (p->first);
    const value_data& val (p->second);

    // Check if this is the first access after being assigned a type.
    //
    if (var.type != nullptr)
      m_->typify (val, var);

    return p;
  }
}
