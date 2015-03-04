// file      : build/variable.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/variable>

#include <build/utility>

using namespace std;

namespace build
{
  variable_set variable_pool;

  // value_proxy
  //
  template <>
  list_value& value_proxy::
  as<list_value&> () const
  {
    list_value* lv (dynamic_cast<list_value*> (p->get ()));
    assert (lv != nullptr);
    return *lv;
  }

  template <>
  const string& value_proxy::
  as<const string&> () const
  {
    const list_value& lv (as<list_value&> ());
    assert (lv.data.size () < 2);

    if (lv.data.empty ())
      return empty_string;

    const name& n (lv.data.front ());

    assert (n.type.empty () && n.dir.empty ());
    return n.value;
  }

  // Note: get the name's directory.
  //
  template <>
  const path& value_proxy::
  as<const path&> () const
  {
    const list_value& lv (as<list_value&> ());
    assert (lv.data.size () < 2);

    if (lv.data.empty ())
      return empty_path;

    const name& n (lv.data.front ());

    assert (n.type.empty () && n.value.empty ());
    return n.dir;
  }
}
