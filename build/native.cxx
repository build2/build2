// file      : build/native.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/native>

using namespace std;

namespace build
{
  const target_type obja::static_type
  {
    typeid (obja),
    "obja",
    &file::static_type,
    &member_target_factory<obja, obj>,
    file::static_type.search
  };

  const target_type objso::static_type
  {
    typeid (objso),
    "objso",
    &file::static_type,
    &member_target_factory<objso, obj>,
    file::static_type.search
  };

  static target*
  obj_factory (dir_path d, string n, const string* e)
  {
    target* a (targets.find (obja::static_type, d, n));
    target* so (targets.find (objso::static_type, d, n));

    obj* t (new obj (move (d), move (n), e));

    if ((t->a = static_cast<obja*> (a)))
      a->group = t;

    if ((t->so = static_cast<objso*> (so)))
      so->group = t;

    return t;
  }

  const target_type obj::static_type
  {
    typeid (obj),
    "obj",
    &target::static_type,
    &obj_factory,
    target::static_type.search
  };

  const target_type exe::static_type
  {
    typeid (exe),
    "exe",
    &file::static_type,
    &target_factory<exe>,
    file::static_type.search
  };

  const target_type lib::static_type
  {
    typeid (lib),
    "lib",
    &file::static_type,
    &target_factory<lib>,
    file::static_type.search
  };
}
