// file      : libbuild2/in/target.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_IN_TARGET_HXX
#define LIBBUILD2_IN_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

#include <libbuild2/in/export.hxx>

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
    class LIBBUILD2_IN_SYMEXPORT in: public file
    {
    public:
      in (context& c, dir_path d, dir_path o, string n)
        : file (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };
  }
}

#endif // LIBBUILD2_IN_TARGET_HXX
