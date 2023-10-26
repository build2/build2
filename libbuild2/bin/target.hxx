// file      : libbuild2/bin/target.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BIN_TARGET_HXX
#define LIBBUILD2_BIN_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

#include <libbuild2/bin/export.hxx>

namespace build2
{
  namespace bin
  {
    // The obj{} target group.
    //
    // Common base of all objX{} object files.
    //
    class LIBBUILD2_BIN_SYMEXPORT objx: public file
    {
    public:
      objx (context& c, dir_path d, dir_path o, string n)
        : file (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT obje: public objx
    {
    public:
      obje (context& c, dir_path d, dir_path o, string n)
        : objx (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT obja: public objx
    {
    public:
      obja (context& c, dir_path d, dir_path o, string n)
        : objx (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT objs: public objx
    {
    public:
      objs (context& c, dir_path d, dir_path o, string n)
        : objx (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Note: this is a "choice" target group.
    //
    class LIBBUILD2_BIN_SYMEXPORT obj: public target
    {
    public:
      obj (context& c, dir_path d, dir_path o, string n)
        : target (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Binary module interface (BMI).
    //
    // While currently there are only C++ modules, if things pan out, chances
    // are we will have C (or Obj-C) modules. And in that case it is plausible
    // we will also have some binutils to examine BMIs, similar to objdump,
    // etc. So that's why this target type is in bin and not cxx.
    //
    // bmi*{} is similar to obj*{} though the semantics is a bit different:
    // the idea is that we should try hard to re-use a single bmiX{} file for
    // an entire "build" but if that's not possible (because the compilation
    // options are too different), then compile a private version for
    // ourselves (the definition of "too different" is, of course, compiler-
    // specific).
    //
    // When we compile a module interface unit, we end up with bmi*{} and
    // obj*{}. How that obj*{} is produced is compiler-dependent. While it
    // makes sense to decouple the production of the two in order to increase
    // parallelism, doing so will further complicate the already hairy
    // organization. So, at least for now, we produce the two at the same time
    // and make obj*{} an ad hoc member of bmi*{}.
    //
    // There are also header units for which we define a parallel hbmi*{}
    // hierarchy. Note that hbmix{} is-a bmix{} (we think of header BMIs as a
    // more specialized kind of BMI) so where you need to distinguish between
    // header and module BMIs, you should check for headers first. Note also
    // that in case of a header unit there may be no obj*{}.
    //
    // Common base of all bmiX{} interface files.
    //
    class LIBBUILD2_BIN_SYMEXPORT bmix: public file
    {
    public:
      bmix (context& c, dir_path d, dir_path o, string n)
        : file (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Common base of all hbmiX{} interface files.
    //
    class LIBBUILD2_BIN_SYMEXPORT hbmix: public bmix
    {
    public:
      hbmix (context& c, dir_path d, dir_path o, string n)
        : bmix (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT bmie: public bmix
    {
    public:
      bmie (context& c, dir_path d, dir_path o, string n)
        : bmix (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT hbmie: public hbmix
    {
    public:
      hbmie (context& c, dir_path d, dir_path o, string n)
        : hbmix (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT bmia: public bmix
    {
    public:
      bmia (context& c, dir_path d, dir_path o, string n)
        : bmix (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT hbmia: public hbmix
    {
    public:
      hbmia (context& c, dir_path d, dir_path o, string n)
        : hbmix (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT bmis: public bmix
    {
    public:
      bmis (context& c, dir_path d, dir_path o, string n)
        : bmix (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT hbmis: public hbmix
    {
    public:
      hbmis (context& c, dir_path d, dir_path o, string n)
        : hbmix (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Note: this is a "choice" target group (similar to obj{}).
    //
    class LIBBUILD2_BIN_SYMEXPORT bmi: public target
    {
    public:
      bmi (context& c, dir_path d, dir_path o, string n)
        : target (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Note: this is a "choice" target group (similar to bmi{} and obj{}).
    //
    class LIBBUILD2_BIN_SYMEXPORT hbmi: public target
    {
    public:
      hbmi (context& c, dir_path d, dir_path o, string n)
        : target (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Common base for lib{} and libul{} groups.
    //
    // Use mtime_target as a base for the "trust me it exists" functionality
    // which we use, for example, to have installed lib{} prerequisites that
    // are matched by the fallback file rule.
    //
    class LIBBUILD2_BIN_SYMEXPORT libx: public mtime_target
    {
    public:
      libx (context& c, dir_path d, dir_path o, string n)
        : mtime_target (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // The libue{} target, libul{} group and libua{} and libus{} members
    // (utility library).
    //
    // Utility libraries are static libraries that differ based on the kind of
    // object files they contains. Note that the libul{} group is more like
    // obj{} rather than lib{} in that one does not build the group directly
    // rather picking a suitable member.
    //
    // libul{} is a "library utility library" in that the choice of members is
    // libua{} or libus{}, even when linking an executable (normally a unit
    // test).
    //
    // Note that there is no "general utility library" with all three types of
    // members (that would cause member uplink ambiguity). If you need to
    // build both a library from libua{}/libus{} and an executable from
    // libue{} then you will need to arrange this explicitly, for example:
    //
    // exe{foo}: libue{foo}
    // lib{foo}: libul{foo}
    //
    // {libue libul}{foo}: cxx{*}
    //
    // Common base of all libuX{} static libraries.
    //
    class LIBBUILD2_BIN_SYMEXPORT libux: public file
    {
    public:
      libux (context& c, dir_path d, dir_path o, string n)
        : file (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT libue: public libux
    {
    public:
      libue (context& c, dir_path d, dir_path o, string n)
        : libux (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT libua: public libux
    {
    public:
      libua (context& c, dir_path d, dir_path o, string n)
        : libux (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT libus: public libux
    {
    public:
      libus (context& c, dir_path d, dir_path o, string n)
        : libux (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Note: this is a "choice" target group.
    //
    // @@ Ideally this shouldn't derive from mtime_target (via libx). Maybe
    //    get rid of libx?
    //
    class LIBBUILD2_BIN_SYMEXPORT libul: public libx
    {
    public:
      libul (context& c, dir_path d, dir_path o, string n)
        : libx (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // The lib{} target group.
    //
    class LIBBUILD2_BIN_SYMEXPORT liba: public file
    {
    public:
      liba (context& c, dir_path d, dir_path o, string n)
        : file (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    class LIBBUILD2_BIN_SYMEXPORT libs: public file
    {
    public:
      libs (context& c, dir_path d, dir_path o, string n)
        : file (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Standard layout type compatible with group_view's const target*[2].
    //
    struct lib_members
    {
      const liba* a = nullptr;
      const libs* s = nullptr;
    };

    class LIBBUILD2_BIN_SYMEXPORT lib: public libx, public lib_members
    {
    public:
      lib (context& c, dir_path d, dir_path o, string n)
        : libx (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

      virtual group_view
      group_members (action) const override;

      // Match options for the install operation on the liba{}/libs{} and
      // libua{}/libus{} target types (note: not lib{}/libul{} nor libue{}).
      //
      // If only install_runtime option is specified, then only install the
      // runtime files omitting everything buildtime (headers, pkg-config
      // files, shared library version-related symlinks, etc).
      //
      // Note that it's either runtime-only or runtime and buildtime (i.e.,
      // everything), so match with install_all instead of install_buildtime
      // (the latter is only useful in the rule implementations).
      //
      static constexpr uint64_t option_install_runtime   = 0x01;
      static constexpr uint64_t option_install_buildtime = 0x02;
      static constexpr uint64_t option_install_all = match_extra::all_options;

    public:
      static const target_type static_type;
    };

    // Windows import library.
    //
    class LIBBUILD2_BIN_SYMEXPORT libi: public file
    {
    public:
      libi (context& c, dir_path d, dir_path o, string n)
        : file (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };

    // Windows module definition (.def).
    //
    class LIBBUILD2_BIN_SYMEXPORT def: public file
    {
    public:
      def (context& c, dir_path d, dir_path o, string n)
        : file (c, move (d), move (o), move (n))
      {
        dynamic_type = &static_type;
      }

    public:
      static const target_type static_type;
    };
  }
}

#endif // LIBBUILD2_BIN_TARGET_HXX
