// file      : build2/cc/utility.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace cc
  {
    inline otype
    compile_type (target& t)
    {
      return
        t.is_a<bin::obje> () ? otype::e :
        t.is_a<bin::obja> () ? otype::a :
        otype::s;
    }

    inline otype
    link_type (target& t)
    {
      return
        t.is_a<exe> ()       ? otype::e :
        t.is_a<bin::liba> () ? otype::a :
        otype::s;
    }
  }
}
