// file      : build2/in/target.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_IN_TARGET_HXX
#define BUILD2_IN_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

namespace build2
{
  namespace in
  {
    // This is the venerable .in ("input") file that needs some kind of
    // preprocessing.
    //
    // One interesting aspect of this target type is that the prerequisite
    // search is target-dependent. Consider:
    //
    // hxx{version}: in{version.hxx} // version.hxx.in -> version.hxx
    //
    // Having to specify the header extension explicitly is inelegant. Instead
    // what we really want to write is this:
    //
    // hxx{version}: in{version}
    //
    // But how do we know that in{version} means version.hxx.in? That's where
    // the target-dependent search comes in: we take into account the target
    // we are a prerequisite of.
    //
    class in: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };
  }
}

#endif // BUILD2_IN_TARGET_HXX
