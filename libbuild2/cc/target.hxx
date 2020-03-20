// file      : libbuild2/cc/target.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_TARGET_HXX
#define LIBBUILD2_CC_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  namespace cc
  {
    // This is an abstract base target for all c-common header/source files.
    // We use this arrangement during rule matching to detect "unknown" (to
    // this rule) source/header files that it cannot handle but should not
    // ignore either. For example, a C link rule that sees a C++ source file.
    //
    class LIBBUILD2_CC_SYMEXPORT cc: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const = 0;
    };

    // There is hardly a c-family compilation without a C header inclusion.
    // As a result, this target type is registered for any c-family module.
    //
    class LIBBUILD2_CC_SYMEXPORT h: public cc
    {
    public:
      using cc::cc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    // This one we define in cc but the target type is only registered by the
    // c module. This way we can implement rule chaining without jumping
    // through too many hoops (like resolving target type dynamically) but
    // also without relaxing things too much (i.e., the user still won't be
    // able to refer to c{} without loading the c module).
    //
    class LIBBUILD2_CC_SYMEXPORT c: public cc
    {
    public:
      using cc::cc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    // pkg-config file targets.
    //
    class LIBBUILD2_CC_SYMEXPORT pc: public file // .pc (common)
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class LIBBUILD2_CC_SYMEXPORT pca: public pc // .static.pc
    {
    public:
      using pc::pc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class LIBBUILD2_CC_SYMEXPORT pcs: public pc // .shared.pc
    {
    public:
      using pc::pc;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };
  }
}

#endif // LIBBUILD2_CC_TARGET_HXX
