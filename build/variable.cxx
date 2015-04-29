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
  const string& value_proxy::
  as<const string&> () const
  {
    const list_value& lv (as<list_value&> ());
    assert (lv.size () < 2);

    if (lv.empty ())
      return empty_string;

    const name& n (lv.front ());

    assert (n.type.empty () && n.dir.empty ());
    return n.value;
  }

  template <>
  const dir_path& value_proxy::
  as<const dir_path&> () const
  {
    const list_value& lv (as<list_value&> ());
    assert (lv.size () < 2);

    if (lv.empty ())
      return empty_dir_path;

    const name& n (lv.front ());

    assert (n.type.empty () && n.value.empty ());
    return n.dir;
  }
}
