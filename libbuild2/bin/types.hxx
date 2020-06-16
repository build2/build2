// file      : libbuild2/bin/types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BIN_TYPES_HXX
#define LIBBUILD2_BIN_TYPES_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

namespace build2
{
  namespace bin
  {
    // Compiler/linker output type (executable, static, or shared).
    //
    enum class otype {e, a, s};

    struct ltype
    {
      otype type;
      bool  utility; // True for utility libraries.

      bool executable ()     const {return type == otype::e && !utility;}
      bool library ()        const {return type != otype::e ||  utility;}
      bool static_library () const {return type == otype::a ||  utility;}
      bool shared_library () const {return type == otype::s && !utility;}
      bool member_library () const {return type != otype::e;}
    };

    // Library group (lib{}) members to build.
    //
    struct lmembers
    {
      bool a;
      bool s;
    };

    // Library link order.
    //
    enum class lorder {a, s, a_s, s_a};

    // Link information: output type and link order.
    //
    struct linfo
    {
      otype  type;
      lorder order;
    };

    // Prerequisite target link flags (saved in prerequisite_target::data).
    //
    using lflags = uintptr_t;

    const lflags lflag_whole = 0x00000001U; // Link whole liba{}/libu*{}.
  }
}

#endif // LIBBUILD2_BIN_TYPES_HXX
