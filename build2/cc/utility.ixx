// file      : build2/cc/utility.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace cc
  {
    inline otype
    compile_type (const target& t, bool mod)
    {
      using namespace bin;

      return
        t.is_a (mod ? bmie::static_type : obje::static_type) ? otype::e :
        t.is_a (mod ? bmia::static_type : obja::static_type) ? otype::a :
        otype::s;
    }

    inline otype
    link_type (const target& t)
    {
      using namespace bin;

      return
        t.is_a<exe> ()  ? otype::e :
        t.is_a<liba> () ? otype::a :
        otype::s;
    }
  }
}
