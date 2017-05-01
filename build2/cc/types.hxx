// file      : build2/cc/types.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_TYPES_HXX
#define BUILD2_CC_TYPES_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

namespace build2
{
  namespace cc
  {
    // Compiler language.
    //
    enum class lang {c, cxx};

    inline ostream&
    operator<< (ostream& os, lang l)
    {
      return os << (l == lang::c ? "C" : "C++");
    }

    // Compile/link output type (executable, static, or shared).
    //
    enum class otype {e, a, s};

    // Library link order.
    //
    enum class lorder {a, s, a_s, s_a};
  }
}

#endif // BUILD2_CC_TYPES_HXX
