// file      : build/variable.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build
{
  // value
  //
  template <typename T>
  inline void
  assign (value& v, const variable& var)
  {
    auto t (&value_traits<T>::value_type);

    if (v.type != t)
      assign (v, t, var);
  }

  template <typename T>
  inline typename value_traits<T>::type
  as (value& v)
  {
    return value_traits<T>::as (v);
  }

  template <typename T>
  inline typename value_traits<T>::const_type
  as (const value& v)
  {
    return value_traits<T>::as (v);
  }

  template <typename T>
  inline bool
  assign (name& n)
  {
    return value_traits<T>::assign (n);
  }

  template <typename T>
  inline typename value_traits<T>::type
  as (name& n)
  {
    return value_traits<T>::as (n);
  }

  template <typename T>
  inline typename value_traits<T>::const_type
  as (const name& n)
  {
    return value_traits<T>::as (n);
  }

  template <typename T>
  inline value& value::
  operator= (T v)
  {
    value_traits<T>::assign (*this, std::move (v));
    return *this;
  }

  template <typename T>
  inline value& value::
  operator+= (T v)
  {
    value_traits<T>::append (*this, std::move (v));
    return *this;
  }

  inline void value::
  assign (names v, const variable& var)
  {
    data_ = std::move (v);
    state_ = (type != nullptr && type->assign != nullptr
              ? type->assign (data_, var)
              : !data_.empty ())
      ? state_type::filled
      : state_type::empty;
  }

  // bool value
  //
  inline bool_value<name> value_traits<bool>::
  as (value& v)
  {
    assert (v.type == bool_type);
    return bool_value<name> (v.data_.front ());
  }

  inline bool_value<const name> value_traits<bool>::
  as (const value& v)
  {
    assert (v.type == bool_type);
    return bool_value<const name> (v.data_.front ());
  }

  inline void value_traits<bool>::
  assign (value& v, bool x)
  {
    if (v.null ())
    {
      if (v.type == nullptr)
        v.type = bool_type;
      v.data_.emplace_back (name ());
      v.state_ = value::state_type::empty;
    }

    as (v) = x;
    v.state_ = value::state_type::filled;
  }

  inline void value_traits<bool>::
  append (value& v, bool x)
  {
    if (v.null ())
      assign (v, x);
    else
      as (v) += x; // Cannot be empty.
  }

  // string value
  //
  inline std::string& value_traits<std::string>::
  as (value& v)
  {
    assert (v.type == string_type);
    return v.data_.front ().value;
  }

  inline const std::string& value_traits<std::string>::
  as (const value& v)
  {
    assert (v.type == string_type);
    return v.data_.front ().value;
  }

  inline void value_traits<std::string>::
  assign (value& v, std::string x)
  {
    if (v.null ())
    {
      if (v.type == nullptr)
        v.type = string_type;
      v.data_.emplace_back (name ());
      v.state_ = value::state_type::empty;
    }

    v.state_ = (as (v) = std::move (x)).empty ()
      ? value::state_type::empty
      : value::state_type::filled;
  }

  inline void value_traits<std::string>::
  append (value& v, std::string x)
  {
    if (v.null ())
      assign (v, std::move (x));
    else
      v.state_ = (as (v) += std::move (x)).empty ()
        ? value::state_type::empty
        : value::state_type::filled;
  }

  // dir_path value
  //
  inline dir_path& value_traits<dir_path>::
  as (value& v)
  {
    assert (v.type == dir_path_type);
    return v.data_.front ().dir;
  }

  inline const dir_path& value_traits<dir_path>::
  as (const value& v)
  {
    assert (v.type == dir_path_type);
    return v.data_.front ().dir;
  }

  inline void value_traits<dir_path>::
  assign (value& v, dir_path x)
  {
    if (v.null ())
    {
      if (v.type == nullptr)
        v.type = dir_path_type;
      v.data_.emplace_back (name ());
      v.state_ = value::state_type::empty;
    }

    v.state_ = (as (v) = std::move (x)).empty ()
      ? value::state_type::empty
      : value::state_type::filled;
  }

  inline void value_traits<dir_path>::
  append (value& v, dir_path x)
  {
    if (v.null ())
      assign (v, std::move (x));
    else
      v.state_ = (as (v) /= std::move (x)).empty ()
        ? value::state_type::empty
        : value::state_type::filled;
  }

  // name value
  //
  inline name& value_traits<name>::
  as (value& v)
  {
    assert (v.type == name_type);
    return v.data_.front ();
  }

  inline const name& value_traits<name>::
  as (const value& v)
  {
    assert (v.type == name_type);
    return v.data_.front ();
  }

  inline void value_traits<name>::
  assign (value& v, name x)
  {
    if (v.null ())
    {
      if (v.type == nullptr)
        v.type = name_type;
      v.data_.emplace_back (name ());
      v.state_ = value::state_type::empty;
    }

    v.state_ = (as (v) = std::move (x)).empty ()
      ? value::state_type::empty
      : value::state_type::filled;
  }

  // vector<T> value
  //
  template <typename T, typename D>
  inline vector_value<T, D>& vector_value<T, D>::
  assign (std::vector<T> v)
  {
    d->clear ();
    d->insert (d->end (),
               std::make_move_iterator (v.begin ()),
               std::make_move_iterator (v.end ()));
    return *this;
  }

  template <typename T, typename D>
  template <typename D1>
  inline vector_value<T, D>& vector_value<T, D>::
  assign (const vector_value<T, D1>& v)
  {
    d->clear ();
    d->insert (d->end (), v.begin (), v.end ());
    return *this;
  }

  template <typename T, typename D>
  template <typename D1>
  inline vector_value<T, D>& vector_value<T, D>::
  append (const vector_value<T, D1>& v)
  {
    d->insert (d->end (), v.begin (), v.end ());
    return *this;
  }

  template <typename T>
  inline vector_value<T, names> value_traits<std::vector<T>>::
  as (value& v)
  {
    assert (v.type == &value_traits<std::vector<T>>::value_type);
    return vector_value<T, names> (v.data_);
  }

  template <typename T>
  inline vector_value<T, const names> value_traits<std::vector<T>>::
  as (const value& v)
  {
    assert (v.type == &value_traits<std::vector<T>>::value_type);
    return vector_value<T, const names> (v.data_);
  }

  template <typename T>
  template <typename V>
  inline void value_traits<std::vector<T>>::
  assign (value& v, V x)
  {
    if (v.null ())
    {
      if (v.type == nullptr)
        v.type = &value_traits<std::vector<T>>::value_type;
      v.state_ = value::state_type::empty;
    }

    v.state_ = (as (v).assign (std::move (x))).empty ()
      ? value::state_type::empty
      : value::state_type::filled;
  }

  template <typename T>
  template <typename V>
  inline void value_traits<std::vector<T>>::
  append (value& v, V x)
  {
    if (v.null ())
      assign (v, std::move (x));
    else
      v.state_ = (as (v).append (std::move (x))).empty ()
        ? value::state_type::empty
        : value::state_type::filled;
  }

  // map<K, V> value
  //
  template <typename K, typename V>
  inline map_value<K, V, names> value_traits<std::map<K, V>>::
  as (value& v)
  {
    assert ((v.type == &value_traits<std::map<K, V>>::value_type));
    return map_value<K, V, names> (v.data_);
  }

  template <typename K, typename V>
  inline map_value<K, V, const names> value_traits<std::map<K, V>>::
  as (const value& v)
  {
    assert ((v.type == &value_traits<std::map<K, V>>::value_type));
    return map_value<K, V, const names> (v.data_);
  }

  template <typename K, typename V>
  template <typename M>
  inline void value_traits<std::map<K, V>>::
  assign (value& v, M x)
  {
    if (v.null ())
    {
      if (v.type == nullptr)
        v.type = &value_traits<std::map<K, V>>::value_type;
      v.state_ = value::state_type::empty;
    }

    v.state_ = (as (v).assign (std::move (x))).empty ()
      ? value::state_type::empty
      : value::state_type::filled;
  }

  template <typename K, typename V>
  template <typename M>
  inline void value_traits<std::map<K, V>>::
  append (value& v, M x)
  {
    if (v.null ())
      assign (v, std::move (x));
    else
      v.state_ = (as (v).append (std::move (x))).empty ()
        ? value::state_type::empty
        : value::state_type::filled;
  }

  // variable_map::iterator_adapter
  //
  template <typename I>
  inline typename I::reference variable_map::iterator_adapter<I>::
  operator* () const
  {
    auto& r (I::operator* ());
    const variable& var (r.first);
    auto& val (r.second);

    // First access after being assigned a type?
    //
    if (var.type != nullptr && val.type != var.type)
      build::assign (const_cast<value&> (val), var.type, var);

    return r;
  }

  template <typename I>
  inline typename I::pointer variable_map::iterator_adapter<I>::
  operator-> () const
  {
    auto p (I::operator-> ());
    const variable& var (p->first);
    auto& val (p->second);

    // First access after being assigned a type?
    //
    if (var.type != nullptr && val.type != var.type)
      build::assign (const_cast<value&> (val), var.type, var);

    return p;
  }
}
