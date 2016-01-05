// file      : build2/variable.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <iterator> // make_move_iterator()

#include <build2/diagnostics>

namespace build2
{
  // vector<T> value
  //
  template <typename T>
  bool
  vector_assign (names& v, const variable& var)
  {
    // Verify each element has valid value of T.
    //
    for (name& n: v)
    {
      if (!assign<T> (n))
        fail << "invalid " << value_traits<T>::value_type.name << " element "
             << "'" << n << "' in variable '" << var.name << "'";
    }

    return !v.empty ();
  }

  template <typename T>
  bool
  vector_append (names& v, names a, const variable& var)
  {
    // Verify that what we are appending is valid.
    //
    vector_assign<T> (a, var);

    if (v.empty ())
      v = move (a);
    else
      v.insert (v.end (),
                std::make_move_iterator (a.begin ()),
                std::make_move_iterator (a.end ()));

    return !v.empty ();
  }

  template <typename T>
  const std::string value_traits<std::vector<T>>::type_name = std::string (
    value_traits<T>::value_type.name) + 's';

  template <typename T>
  const value_type value_traits<std::vector<T>>::value_type
  {
    value_traits<std::vector<T>>::type_name.c_str (),
    &vector_assign<T>,
    &vector_append<T>
  };

  // map<K, V> value
  //
  template <typename K, typename V, typename D>
  map_value<K, V, D>& map_value<K, V, D>::
  assign (std::map<K, V> m)
  {
    d->clear ();
    for (auto& p: m)
    {
      d->emplace_back (p.first); // Const, can't move.
      d->back ().pair = '=';
      d->emplace_back (std::move (p.second));
    }

    return *this;
  }

  template <typename K, typename V, typename D>
  auto map_value<K, V, D>::
  find (const K& k) -> iterator
  {
    // @@ Scan backwards to handle duplicates.
    //
    for (auto i (d->rbegin ()); i != d->rend (); ++i)
      if (as<K> (*++i) == k)
        return iterator (--(i.base ()));

    return end ();
  }

  template <typename K, typename V, typename D>
  auto map_value<K, V, D>::
  find (const K& k) const -> const_iterator
  {
    // @@ Scan backwards to handle duplicates.
    //
    for (auto i (d->rbegin ()); i != d->rend (); ++i)
      if (as<K> (*++i) == k)
        return const_iterator (--(i.base ()));

    return end ();
  }

  template <typename K, typename V>
  bool
  map_assign (names& v, const variable& var)
  {
    // Verify we have a sequence of pairs and each lhs/rhs is a valid
    // value of K/V.
    //
    for (auto i (v.begin ()); i != v.end (); ++i)
    {
      if (i->pair == '\0')
        fail << value_traits<std::map<K, V>>::value_type.name << " key-value "
             << "pair expected instead of '" << *i << "' "
             << "in variable '" << var.name << "'";

      if (!assign<K> (*i))
        fail << "invalid " << value_traits<K>::value_type.name << " key "
             << "'" << *i << "' in variable '" << var.name << "'";

      ++i; // Got to have the second half of the pair.

      if (!assign<V> (*i))
        fail << "invalid " << value_traits<V>::value_type.name << " value "
             << "'" << *i << "' in variable '" << var.name << "'";
    }

    //@@ When doing sorting, note that assign() can convert the
    //   value.

    //@@ Is sorting really the right trade-off (i.e., insertion
    //   vs search)? Perhaps linear search is ok?

    return !v.empty ();
  }

  template <typename K, typename V>
  bool
  map_append (names& v, names a, const variable& var)
  {
    //@@ Not weeding out duplicates.

    // Verify that what we are appending is valid.
    //
    map_assign<K, V> (a, var);

    if (v.empty ())
      v = move (a);
    else
      v.insert (v.end (),
                std::make_move_iterator (a.begin ()),
                std::make_move_iterator (a.end ()));

    return !v.empty ();
  }

  template <typename K, typename V>
  const std::string value_traits<std::map<K, V>>::type_name = std::string (
    value_traits<K>::value_type.name) + '_' +
    value_traits<V>::value_type.name + "_map";

  template <typename K, typename V>
  const value_type value_traits<std::map<K, V>>::value_type
  {
    value_traits<std::map<K, V>>::type_name.c_str (),
    &map_assign<K, V>,
    &map_append<K, V>
  };
}
