// file      : libbuild2/cc/types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_TYPES_HXX
#define LIBBUILD2_CC_TYPES_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target-type.hxx>

#include <libbuild2/bin/types.hxx>

namespace build2
{
  namespace cc
  {
    using bin::otype;
    using bin::ltype;
    using bin::lorder;
    using bin::linfo;
    using bin::lflags;
    using bin::lflag_whole;

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
    // Note also that implementation paritions produce BMIs and are, in a
    // sense, module-private interfaces.
    //
    enum class unit_type
    {
      non_modular,
      module_intf,
      module_impl,
      module_intf_part,
      module_impl_part,
      module_header
    };

    // Note that an interface partition can be imported both as an interface
    // (with export) and as implementation (without export).
    //
    enum class import_type
    {
      module_intf,
      module_part,
      module_header
    };

    struct module_import
    {
      import_type type;
      string      name;
      bool        exported;  // True if re-exported (export import M;).
      size_t      score;     // Match score (see compile::search_modules()).
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

    // Compile target types.
    //
    struct compile_target_types
    {
      const target_type& obj;
      const target_type& bmi;
      const target_type& hbmi;
    };
  }
}

#endif // LIBBUILD2_CC_TYPES_HXX
