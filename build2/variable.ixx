// file      : build2/variable.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <type_traits> // is_same

namespace build2
{
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
    const value_type* b (v.type);
    for (;
         b != nullptr && b != &value_traits<T>::value_type;
         b = b->base_type) ;
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
  cast (const lookup& l)
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
  cast_null (const lookup& l)
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
  cast_empty (const lookup& l)
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
  cast_default (const lookup& l, const T& d)
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
  cast_false (const lookup& l)
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
  cast_true (const lookup& l)
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

  void
  typify (value&, const value_type&, const variable*, memory_order);

  inline void
  typify (value& v, const value_type& t, const variable* var)
  {
    typify (v, t, var, memory_order_relaxed);
  }

  inline vector_view<const name>
  reverse (const value& v, names& storage)
  {
    assert (v &&
            storage.empty () &&
            (v.type == nullptr || v.type->reverse != nullptr));
    return v.type == nullptr ? v.as<names> () : v.type->reverse (v, storage);
  }

  inline vector_view<name>
  reverse (value& v, names& storage)
  {
    names_view cv (reverse (static_cast<const value&> (v), storage));
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
  template <typename T>
  inline auto
  convert (names&& ns) -> decltype (value_traits<T>::convert (move (ns)))
  {
    return value_traits<T>::convert (move (ns));
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
    // Convert the value to its "self-sufficient" form.
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

  // map<K, V> value
  //
  template <typename K, typename V>
  inline void value_traits<std::map<K, V>>::
  assign (value& v, map<K, V>&& x)
  {
    if (v)
      v.as<map<K, V>> () = move (x);
    else
      new (&v.data_) map<K, V> (move (x));
  }

  template <typename K, typename V>
  inline void value_traits<std::map<K, V>>::
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
        m.insert (m.end (),
                  make_move_iterator (x.begin ()),
                  make_move_iterator (x.end ()));
    }
    else
      new (&v.data_) map<K, V> (move (x));
  }

  // variable_pool
  //
  inline const variable& variable_pool::
  operator[] (const string& n) const
  {
    const variable* r (find (n));
    assert (r != nullptr);
    return *r;
  }

  inline const variable* variable_pool::
  find (const string& n) const
  {
    auto i (map_.find (&n));
    return i != map_.end () ? &i->second : nullptr;
  }

  // variable_map
  //
  inline void variable_map::
  typify (const value_data& v, const variable& var) const
  {
    // We assume typification is not modification so no version increment.
    //
    if (phase == run_phase::load)
    {
      if (v.type != var.type)
        build2::typify (const_cast<value_data&> (v), *var.type, &var);
    }
    else
    {
      if (v.type.load (memory_order_acquire) != var.type)
        build2::typify_atomic (const_cast<value_data&> (v), *var.type, &var);
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
