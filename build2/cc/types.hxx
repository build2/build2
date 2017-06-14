// file      : build2/cc/types.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_TYPES_HXX
#define BUILD2_CC_TYPES_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/target-type.hxx>

namespace build2
{
  namespace cc
  {
    // Translation unit information (currently modules).
    //
    struct module_import
    {
      string name;
      bool   exported; // True if re-exported (export import M;).
      size_t score;    // See compile::search_modules().
    };

    using module_imports = vector<module_import>;

    struct translation_unit
    {
      string             module_name;      // Not empty if a module unit.
      bool               module_interface; // True if a module interface unit.
      cc::module_imports module_imports;   // Imported modules.
    };

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

    // Compile target types.
    //
    struct compile_target_types
    {
      const target_type& obj;
      const target_type& bmi;
    };

    // Library link order.
    //
    enum class lorder {a, s, a_s, s_a};
  }
}

#endif // BUILD2_CC_TYPES_HXX
