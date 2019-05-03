// file      : build2/cc/types.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
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
    // Translation unit information.
    //
    // We use absolute and normalized header path as the header unit module
    // name.
    //
    // Note that our terminology doesn't exactly align with the (current)
    // standard where a header unit is not a module (that is, you either
    // import a "module [interface translation unit]" or a "[synthesized]
    // header [translation] unit"). On the other hand, lots of the underlying
    // mechanics suggest that a header unit is module-like; they end up having
    // BMIs (which stand for "binary module interface"), etc. In a sense, a
    // header unit is an "interface unit" for (a part of) the global module
    // (maybe a partition).
    //
    enum class unit_type
    {
      non_modular,
      module_iface,
      module_impl,
      module_header
    };

    struct module_import
    {
      unit_type  type;      // Either module_iface or module_header.
      string     name;
      bool       exported;  // True if re-exported (export import M;).
      size_t     score;     // Match score (see compile::search_modules()).
    };

    using module_imports = vector<module_import>;

    struct module_info
    {
      string         name;     // Empty if non-modular.
      module_imports imports;  // Imported modules.
    };

    struct unit
    {
      unit_type               type = unit_type::non_modular;
      build2::cc::module_info module_info;
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

    struct ltype
    {
      otype type;
      bool  utility; // True for utility libraries.

      bool executable ()     const {return type == otype::e && !utility;}
      bool library ()        const {return type != otype::e ||  utility;}
      bool static_library () const {return type == otype::a ||  utility;}
      bool shared_library () const {return type == otype::s && !utility;}
    };

    // Compile target types.
    //
    struct compile_target_types
    {
      const target_type& obj;
      const target_type& bmi;
      const target_type& hbmi;
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

    // Prerequisite link flags.
    //
    using lflags = uintptr_t; // To match prerequisite_target::data.

    const lflags lflag_whole = 0x00000001U; // Link whole liba{}/libu*}.
  }
}

#endif // BUILD2_CC_TYPES_HXX
