// file      : build2/cxx/target.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CXX_TARGET_HXX
#define BUILD2_CXX_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>
#include <libbuild2/cc/target.hxx>

namespace build2
{
  namespace cxx
  {
    using cc::h;
    using cc::c;

    class hxx: public cc::cc
    {
    public:
      using cc::cc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class ixx: public cc::cc
    {
    public:
      using cc::cc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class txx: public cc::cc
    {
    public:
      using cc::cc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class cxx: public cc::cc
    {
    public:
      using cc::cc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    // The module interface unit is both like a header (e.g., we need to
    // install it) and like a source (we need to compile it). Plus, to
    // support dual use (modules/headers) it could actually be #include'd
    // (and even in both cases e.g., by different codebases).
    //
    class mxx: public cc::cc
    {
    public:
      using cc::cc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };
  }
}

#endif // BUILD2_CXX_TARGET_HXX
