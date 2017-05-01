// file      : build2/bin/guess.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_BIN_GUESS_HXX
#define BUILD2_BIN_GUESS_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

namespace build2
{
  namespace bin
  {
    // ar/ranlib information.
    //
    // Currently recognized ar/ranlib and their ids:
    //
    // gnu          GNU binutils
    // llvm         LLVM
    // bsd          FreeBSD (and maybe other BSDs)
    // msvc         Microsoft's lib.exe
    // generic      Generic/unrecognized
    //
    // The signature is normally the --version/-V line.
    //
    // The checksum is used to detect ar/ranlib changes. It is calculated in
    // a toolchain-specific manner (usually the output of --version/-V) and
    // is not bulletproof.
    //
    struct ar_info
    {
      process_path ar_path;
      string ar_id;
      string ar_signature;
      string ar_checksum;

      process_path ranlib_path;
      string ranlib_id;
      string ranlib_signature;
      string ranlib_checksum;
    };

    // The ranlib path can be NULL, in which case no ranlib guessing will be
    // attemplated and the returned ranlib_* members will be left empty.
    //
    ar_info
    guess_ar (const path& ar, const path* ranlib, const dir_path& fallback);

    // ld information.
    //
    // Currently recognized linkers and their ids:
    //
    // gnu          GNU binutils ld.bfd
    // gold         GNU binutils ld.gold
    // llvm         LLVM lld (note: not llvm-ld or llvm-link)
    // ld64         Apple's new linker
    // cctools      Apple's old/classic linker
    // msvc         Microsoft's link.exe
    //
    // Note that BSDs are currently using GNU ld but some of them (e.g.,
    // FreeBSD) are hoping to migrate to lld.
    //
    // The signature is normally the --version/-version/-v line.
    //
    // The checksum is used to detect ld changes. It is calculated in a
    // toolchain-specific manner (usually the output of --version/-version/-v)
    // and is not bulletproof.
    //
    struct ld_info
    {
      process_path path;
      string id;
      string signature;
      string checksum;
    };

    ld_info
    guess_ld (const path& ld, const dir_path& fallback);

    // rc information.
    //
    // Currently recognized resource compilers and their ids:
    //
    // gnu          GNU binutils windres
    // msvc         Microsoft's rc.exe
    //
    // The signature is normally the --version line.
    //
    // The checksum is used to detect rc changes. It is calculated in a
    // toolchain-specific manner (usually the output of --version) and is not
    // bulletproof.
    //
    struct rc_info
    {
      process_path path;
      string id;
      string signature;
      string checksum;
    };

    rc_info
    guess_rc (const path& rc, const dir_path& fallback);
  }
}

#endif // BUILD2_BIN_GUESS_HXX
