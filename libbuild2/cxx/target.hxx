// file      : libbuild2/cxx/target.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CXX_TARGET_HXX
#define LIBBUILD2_CXX_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/cc/target.hxx>

#include <libbuild2/cxx/export.hxx>

namespace build2
{
  namespace cxx
  {
    using cc::h;
    using cc::c;
    using cc::m;

    class LIBBUILD2_CXX_SYMEXPORT hxx: public cc::cc
    {
    public:
      hxx (context& c, dir_path d, dir_path o, string n)
        : cc (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_CXX_SYMEXPORT ixx: public cc::cc
    {
    public:
      ixx (context& c, dir_path d, dir_path o, string n)
        : cc (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_CXX_SYMEXPORT txx: public cc::cc
    {
    public:
      txx (context& c, dir_path d, dir_path o, string n)
        : cc (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_CXX_SYMEXPORT cxx: public cc::cc
    {
    public:
      cxx (context& c, dir_path d, dir_path o, string n)
        : cc (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // The module interface unit is both like a header (e.g., we need to
    // install it) and like a source (we need to compile it). Plus, to
    // support dual use (modules/headers) it could actually be #include'd
    // (and even in both cases e.g., by different codebases).
    //
    class LIBBUILD2_CXX_SYMEXPORT mxx: public cc::cc
    {
    public:
      mxx (context& c, dir_path d, dir_path o, string n)
        : cc (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Objective-C++ source file.
    //
    class LIBBUILD2_CXX_SYMEXPORT mm: public cc::cc
    {
    public:
      mm (context& c, dir_path d, dir_path o, string n)
        : cc (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // This is an abstract base target for deriving additional targets (for
    // example, Qt moc{}) that can be #include'd in C++ translation units. In
    // particular, only such targets will be considered to reverse-lookup
    // extensions to target types (see dyndep_rule::map_extension() for
    // background).
    //
    class LIBBUILD2_CXX_SYMEXPORT cxx_inc: public cc::cc
    {
    public:
      cxx_inc (context& c, dir_path d, dir_path o, string n)
        : cc (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };
  }
}

#endif // LIBBUILD2_CXX_TARGET_HXX
