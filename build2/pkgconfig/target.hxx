// file      : build2/pkgconfig/target.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_PKGCONFIG_TARGET_HXX
#define BUILD2_PKGCONFIG_TARGET_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/target.hxx>

namespace build2
{
  namespace pkgconfig
  {
    class pc: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
    };

    class pca: public pc // .static.pc
    {
    public:
      using pc::pc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class pcs: public pc // .shared.pc
    {
    public:
      using pc::pc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };
  }
}

#endif // BUILD2_PKGCONFIG_TARGET_HXX
