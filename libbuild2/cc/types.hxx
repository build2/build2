// file      : libbuild2/cc/types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_TYPES_HXX
#define LIBBUILD2_CC_TYPES_HXX

#include <unordered_map>

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

    // Ad hoc (as opposed to marked with x.importable) importable headers.
    //
    // Note that these are only searched for in the system header search
    // directories (sys_hdr_dirs).
    //
    struct importable_headers
    {
      mutable shared_mutex mutex;

      using groups = small_vector<string, 3>;

      // Map of groups (e.g., std, <vector>, <boost/*.hpp>) that have already
      // been inserted.
      //
      // For angle-bracket file groups (e.g., <vector>), the value is a
      // pointer to the corresponding header_map element. For angle-bracket
      // file pattern groups (e.g., <boost/**.hpp>), the value is the number
      // of files in the group.
      //
      // Note that while the case-sensitivity of header names in #include
      // directives is implementation-defined, our group names are case-
      // sensitive (playing loose with the case will lead to portability
      // issues sooner or later so we don't bother with any more elborate
      // solutions).
      //
      std::unordered_map<string, uintptr_t> group_map;

      // Map of absolute and normalized header paths to groups (e.g., std,
      // <vector>, <boost/**.hpp>) to which they belong. The groups are
      // ordered from the most to least specific (e.g., <vector> then std).
      //
      std::unordered_map<path, groups> header_map;

      // Note that all these functions assume the instance is unique-locked.
      //

      // Search for and insert an angle-bracket file, for example <vector>,
      // making it belong to the angle-bracket file group itself. Return the
      // pointer to the corresponding header_map element if the file has been
      // found and NULL otherwise (so can be used as bool).
      //
      pair<const path, groups>*
      insert_angle (const dir_paths& sys_hdr_dirs, const string& file);

      // As above but for a manually-searched absolute and normalized path.
      //
      pair<const path, groups>&
      insert_angle (path, const string& file);

      // Search for and insert an angle-bracket file pattern, for example
      // <boost/**.hpp>, making each header belong to the angle-bracket file
      // group (e.g., <boost/any.hpp>) and the angle-bracket file pattern
      // group itself. Return the number of files found that match the
      // pattern.
      //
      size_t
      insert_angle_pattern (const dir_paths& sys_hdr_dirs, const string& pat);
    };

    // Headers and header groups whose inclusion should or should not be
    // translated to the corresponding header unit imports.
    //
    // The key is either an absolute and normalized header path or a reference
    // to an importable_headers group (e.g., <vector>, std).
    //
    using translatable_headers = map<string, optional<bool>>;

    // Special translatable header groups.
    //
    extern const string header_group_all;
    extern const string header_group_all_importable;
    extern const string header_group_std;
    extern const string header_group_std_importable;

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

    // "Unhide" operator<< from the build2 namespace.
    //
    using build2::operator<<;
  }
}

#endif // LIBBUILD2_CC_TYPES_HXX
