// file      : build2/test/target.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TEST_TARGET_HXX
#define BUILD2_TEST_TARGET_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/target.hxx>

namespace build2
{
  namespace test
  {
    class testscript: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };
  }
}

#endif // BUILD2_TEST_TARGET_HXX
