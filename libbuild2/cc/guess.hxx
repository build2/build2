// file      : libbuild2/cc/guess.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_GUESS_HXX
#define LIBBUILD2_CC_GUESS_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/cc/types.hxx>

namespace build2
{
  namespace cc
  {
    // Compiler id consisting of a type and optional variant. If the variant
    // is not empty, then the id is spelled out as 'type-variant', similar to
    // target triplets (this also means that the type cannot contain '-').
    //
    // Currently recognized compilers and their ids:
    //
    // gcc               GCC gcc/g++
    // clang             Vanilla Clang clang/clang++
    // clang-apple       Apple Clang clang/clang++ and the gcc/g++ "alias"
    // clang-emscripten  Emscripten emcc/em++.
    // msvc              Microsoft cl.exe
    // msvc-clang        Clang in the cl compatibility mode (clang-cl)
    // icc               Intel icc/icpc
    //
    // Note that the user can provide a custom id with one of the predefined
    // types and a custom variant (say 'gcc-tasking').
    //
    enum class compiler_type
    {
      gcc = 1, // 0 value represents invalid type.
      clang,
      msvc,
      icc
      // Update compiler_id(string) and to_string() if adding a new type.
    };

    const compiler_type invalid_compiler_type = static_cast<compiler_type> (0);

    string
    to_string (compiler_type);

    inline ostream&
    operator<< (ostream& o, const compiler_type& t)
    {
      return o << to_string (t);
    }

    struct compiler_id
    {
      compiler_type type = invalid_compiler_type;
      std::string   variant;

      bool
      empty () const {return type == invalid_compiler_type;}

      std::string
      string () const;

      compiler_id ()
          : type (invalid_compiler_type) {}

      compiler_id (compiler_type t, std::string v)
          : type (t), variant (move (v)) {}

      explicit
      compiler_id (const std::string&);
    };

    inline ostream&
    operator<< (ostream& o, const compiler_id& id)
    {
      return o << id.string ();
    }

    // Compiler class describes a set of compilers that follow more or less
    // the same command line interface. Compilers that don't belong to any of
    // the existing classes are in classes of their own (say, Sun CC would be
    // on its own if we were to support it).
    //
    // Currently defined compiler classes:
    //
    // gcc          gcc, clang, clang-{apple,emscripten}, icc (on non-Windows)
    // msvc         msvc, clang-cl, icc (Windows)
    //
    enum class compiler_class
    {
      gcc,
      msvc
    };

    string
    to_string (compiler_class);

    inline ostream&
    operator<< (ostream& o, compiler_class c)
    {
      return o << to_string (c);
    }

    // Compiler version. Here we map the various compiler version formats to
    // something that resembles the MAJOR.MINOR.PATCH-BUILD form of the
    // Semantic Versioning. While the MAJOR.MINOR part is relatively
    // straightforward, PATCH may be empty and BUILD can contain pretty much
    // anything (including spaces).
    //
    // All compilers of the same type follow the same versioning scheme
    // (normally of their variant-less primary member):
    //
    // gcc           A.B.C[ ...]         {A, B, C, ...}
    // clang         A.B.C[( |-)...]     {A, B, C, ...}
    // icc           A.B[.C.D] ...       {A, B, C, D ...}
    // msvc          A.B.C[.D]           {A, B, C, D}
    //
    // A compiler variant may also have a variant version:
    //
    // clang-apple       A.B[.C] ...     {A, B, C, ...}
    // clang-emscripten  A.B.C ...        {A, B, C, ...}
    // msvc-clang        A.B.C[( |-)...] {A, B, C, ...} (native Clang version)
    //
    // Note that the clang-apple variant version is a custom Apple version
    // that doesn't correspond to the vanilla Clang version nor is the mapping
    // documented by Apple. We try to map it conservatively to the best of our
    // abilities.
    //
    struct compiler_version
    {
      std::string string;

      // Currently all the compilers that we support have numeric MAJOR,
      // MINOR, and PATCH components and it makes sense to represent them as
      // integers for easy comparison. If we meet a compiler for which this
      // doesn't hold, then we will probably just set these to 0 and let the
      // user deal with the string representation.
      //
      uint64_t major;
      uint64_t minor;
      uint64_t patch;
      std::string build;
    };

    // Compiler information.
    //
    // The signature is normally the -v/--version line that was used to guess
    // the compiler id and its version.
    //
    // The checksum is used to detect compiler changes. It is calculated in a
    // compiler-specific manner (usually the output of -v/--version) and is
    // not bulletproof (e.g., it most likely won't detect that the underlying
    // assembler or linker has changed). However, it should detect most
    // common cases, such as an upgrade to a new version or a configuration
    // change.
    //
    // Note that we assume the checksum incorporates the (default) target so
    // that if the compiler changes but only in what it targets, then the
    // checksum will still change. This is currently the case for all the
    // compilers that we support.
    //
    // The target is the compiler's traget architecture triplet. Note that
    // unlike all the preceding fields, this one takes into account the
    // compile options (e.g., -m32).
    //
    // The pattern is the toolchain program pattern that could sometimes be
    // derived for some toolchains. For example, i686-w64-mingw32-*-4.9.
    //
    // The bin_pattern is the binutils program pattern that could sometimes be
    // derived for some toolchains. For example, i686-w64-mingw32-*. If the
    // pattern could not be derived, then it could alternatively contain
    // search paths (similar to the PATH environment variable), in which case
    // it will end with a directory separator but will not contain '*'.
    //
    // Watch out for the environment variables affecting any of the extracted
    // information (like sys_*_dirs) since we cache it.
    //
    struct compiler_info
    {
      process_path path;
      compiler_id id;
      compiler_class class_;
      compiler_version version;
      optional<compiler_version> variant_version;
      string signature;
      string checksum;
      string target;
      string original_target; // As reported by the compiler.
      string pattern;
      string bin_pattern;

      // Compiler runtime, C standard library, and language (e.g., C++)
      // standard library.
      //
      // The runtime is the low-level compiler runtime library and its name is
      // the library/project name. Current values are (but can also be some
      // custom name specified with Clang's --rtlib):
      //
      // libgcc
      // compiler-rt  (clang)
      // msvc
      //
      // The C standard library is normally the library/project name (e.g,
      // glibc, klibc, newlib, etc) but if there is none, then we fallback to
      // the vendor name (e.g., freebsd, apple). Current values are:
      //
      // glibc
      // msvc         (msvcrt.lib/msvcrNNN.dll)
      // freebsd
      // netbsd
      // apple
      // newlib       (also used by Cygwin)
      // klibc
      // bionic
      // uclibc
      // musl
      // dietlibc
      // emscripten
      // other
      // none
      //
      // The C++ standard library is normally the library/project name.
      // Current values are:
      //
      // libstdc++
      // libc++
      // msvcp        (msvcprt.lib/msvcpNNN.dll)
      // other
      // none
      //
      string runtime;
      string c_stdlib;
      string x_stdlib;

      // System library/header/module search paths and number of leading mode
      // entries, if extracted at the guess stage.
      //
      optional<pair<dir_paths, size_t>> sys_lib_dirs;
      optional<pair<dir_paths, size_t>> sys_hdr_dirs;
      optional<pair<dir_paths, size_t>> sys_mod_dirs;

      // Optional list of environment variables that affect the compiler and
      // its target platform.
      //
      const char* const* compiler_environment;
      const char* const* platform_environment;
    };

    // In a sense this is analagous to the language standard which we handle
    // via a virtual function in common. However, duplicating this hairy ball
    // of fur in multiple places doesn't seem wise, especially considering
    // that most of it will be the same, at least for C and C++.
    //
    const compiler_info&
    guess (context&,
           const char* xm,        // Module (for var names in diagnostics).
           lang xl,               // Language.
           const string& ec,      // Environment checksum.
           const path& xc,        // Compiler path.
           const string* xi,      // Compiler id (optional).
           const string* xv,      // Compiler version (optional).
           const string* xt,      // Compiler target (optional).
           const strings& x_mode, // Compiler mode options.
           const strings* c_poptions, const strings* x_poptions,
           const strings* c_coptions, const strings* x_coptions,
           const strings* c_loptions, const strings* x_loptions);

    // Given a language, compiler id, optional (empty) pattern, and mode
    // return an appropriate default config.x value (compiler path and mode)
    //
    // For example, for (lang::cxx, gcc, *-4.9) we will get g++-4.9.
    //
    strings
    guess_default (lang,
                   const string& cid,
                   const string& pattern,
                   const strings& mode);

    // Insert importable/non-importable C++ standard library headers
    // ([headers]/4).
    //
    // Note that the importable_headers instance should be unique-locked.
    //
    void
    guess_std_importable_headers (const compiler_info&,
                                  const dir_paths& sys_hdr_dirs,
                                  importable_headers&);
  }
}

#endif // LIBBUILD2_CC_GUESS_HXX
