// file      : build2/bin/target.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_BIN_TARGET_HXX
#define BUILD2_BIN_TARGET_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/target.hxx>

namespace build2
{
  namespace bin
  {
    // The obj{} target group.
    //
    class objx: public file // Common base of all objX{} object files.
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
    };

    class obje: public objx
    {
    public:
      using objx::objx;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class obja: public objx
    {
    public:
      using objx::objx;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class objs: public objx
    {
    public:
      using objx::objx;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class obj: public target
    {
    public:
      using target::target;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    // Binary module interface.
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
    class bmix: public file // Common base of all bmiX{} interface files.
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
    };

    class bmie: public bmix
    {
    public:
      using bmix::bmix;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class bmia: public bmix
    {
    public:
      using bmix::bmix;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class bmis: public bmix
    {
    public:
      using bmix::bmix;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class bmi: public target
    {
    public:
      using target::target;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    // Common base for lib{} and libul{}/libu{} groups.
    //
    // We use mtime_target as a base for the "trust me it exists" functionality
    // which we use, for example, to have installed lib{} prerequisites that
    // are matched by the fallback file rule.
    //
    class libx: public mtime_target
    {
    public:
      using mtime_target::mtime_target;

    public:
      static const target_type static_type;
    };

    // The libul{}/libu{} target groups (utility library).
    //
    // All the members are static libraries that differ based on the kind of
    // object files they contains. Note that the group is more like obj{}
    // rather than lib{} in that one does not build the group directly rather
    // picking a suitable member.
    //
    // libul{} is a "library utility library" in that the choice of members is
    // libua{} or libus{}, even when linking an executable (normally a unit
    // test).
    //
    // libu{} is a general utility library with all three types of members. It
    // would normally be used when you want to build both a library from
    // libua{}/libus{} and an executable from libue{}.
    //
    class libux: public file // Common base of all libuX{} static libraries.
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
    };

    class libue: public libux
    {
    public:
      using libux::libux;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class libua: public libux
    {
    public:
      using libux::libux;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class libus: public libux
    {
    public:
      using libux::libux;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class libul: public libx
    {
    public:
      using libx::libx;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class libu: public libx
    {
    public:
      using libx::libx;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    // The lib{} target group.
    //
    class liba: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    class libs: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;

      virtual const target_type&
      dynamic_type () const override {return static_type;}
    };

    // Standard layout type compatible with group_view's const target*[2].
    //
    struct lib_members
    {
      const liba* a = nullptr;
      const libs* s = nullptr;
    };

    class lib: public libx, public lib_members
    {
    public:
      using libx::libx;

      virtual group_view
      group_members (action) const override;

    public:
      static const target_type static_type;

      virtual const target_type&
      dynamic_type () const override {return static_type;}
    };

    // Windows import library.
    //
    class libi: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };

    // Windows module definition (.def).
    //
    class def: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };
  }
}

#endif // BUILD2_BIN_TARGET_HXX
