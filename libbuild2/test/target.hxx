// file      : libbuild2/test/target.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_TARGET_HXX
#define LIBBUILD2_TEST_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  namespace test
  {
    class LIBBUILD2_SYMEXPORT testscript: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };
  }
}

#endif // LIBBUILD2_TEST_TARGET_HXX
