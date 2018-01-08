// file      : build2/cc/guess.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_GUESS_HXX
#define BUILD2_CC_GUESS_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/cc/types.hxx>

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
    // gcc          GCC gcc/g++
    // clang        Vanilla Clang clang/clang++
    // clang-apple  Apple Clang clang/clang++ and the gcc/g++ "alias"
    // msvc         Microsoft cl.exe
    // icc          Intel icc/icpc
    //
    struct compiler_id
    {
      std::string type;
      std::string variant;

      bool
      empty () const {return type.empty ();}

      std::string
      string () const {return variant.empty () ? type : type + "-" + variant;}

      enum value_type
      {
        gcc,
        clang,
        clang_apple,
        msvc,
        icc
      };

      value_type
      value () const;
    };

    inline ostream&
    operator<< (ostream& os, const compiler_id& id)
    {
      return os << id.string ();
    }

    // Compiler class describes a set of compilers that follow more or less
    // the same command line interface. Compilers that don't belong to any of
    // the existing classes are in classes of their own (say, Sun CC would be
    // on its own if we were to support it).
    //
    // Currently defined compiler classes:
    //
    // gcc          gcc, clang, clang-apple, icc (on non-Windows)
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
    operator<< (ostream& os, compiler_class cl)
    {
      return os << to_string (cl);
    }

    // Compiler version. Here we map the various compiler version formats to
    // something that resembles the MAJOR.MINOR.PATCH-BUILD form of the
    // Semantic Versioning. While the MAJOR.MINOR part is relatively
    // straightforward, PATCH may be empty and BUILD can contain pretty much
    // anything (including spaces).
    //
    // gcc           A.B.C[ ...]         {A, B, C, ...}
    // clang         A.B.C[( |-)...]     {A, B, C, ...}
    // clang-apple   A.B[.C] ...         {A, B, C, ...}
    // icc           A.B[.C.D] ...       {A, B, C, D ...}
    // msvc          A.B.C[.D]           {A, B, C, D}
    //
    // Note that the clang-apple version is a custom Apple version and does
    // not correspond to the vanilla clang version.
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
    // pattern could not be derived, then it could contain a fallback search
    // directory, in which case it will end with a directory separator but
    // will not contain '*'.
    //
    struct compiler_info
    {
      process_path path;
      compiler_id id;
      compiler_class class_;
      compiler_version version;
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
      // apple
      // newlib       (also used by Cygwin)
      // klibc
      // bionic
      // uclibc
      // musl
      // dietlibc
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
    };

    // In a sense this is analagous to the language standard which we handle
    // via a virtual function in common. However, duplicating this hairy ball
    // of fur in multiple places doesn't seem wise, especially considering
    // that most of it will be the same, at least for C and C++.
    //
    compiler_info
    guess (lang,
           const path& xc,
           const strings* c_poptions, const strings* x_poptions,
           const strings* c_coptions, const strings* x_coptions,
           const strings* c_loptions, const strings* x_loptions);

    // Given a language, toolchain id, and optionally (empty) a pattern,
    // return an appropriate default compiler path.
    //
    // For example, for (lang::cxx, gcc, *-4.9) we will get g++-4.9.
    //
    path
    guess_default (lang, const string& cid, const string& pattern);
  }
}

#endif // BUILD2_CC_GUESS_HXX
