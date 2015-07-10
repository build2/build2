// file      : build/variable.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
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
  string& value_proxy::
  as<string&> () const
  {
    list_value& lv (as<list_value&> ());
    assert (lv.size () == 1);
    name& n (lv.front ());
    assert (n.simple ());
    return n.value;
  }

  template <>
  const string& value_proxy::
  as<const string&> () const
  {
    const list_value& lv (as<list_value&> ());
    assert (lv.size () < 2);

    if (lv.empty ())
      return empty_string;

    const name& n (lv.front ());

    assert (n.simple ());
    return n.value;
  }

  template <>
  dir_path& value_proxy::
  as<dir_path&> () const
  {
    list_value& lv (as<list_value&> ());
    assert (lv.size () == 1);
    name& n (lv.front ());
    assert (n.directory ());
    return n.dir;
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

    if (n.empty ())
      return empty_dir_path;

    assert (n.directory ());
    return n.dir;
  }
}
