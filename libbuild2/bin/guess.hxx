// file      : libbuild2/bin/guess.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BIN_GUESS_HXX
#define LIBBUILD2_BIN_GUESS_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

namespace build2
{
  namespace bin
  {
    // ar/ranlib information.
    //
    // Currently recognized ar/ranlib and their ids:
    //
    // gnu          GNU binutils
    // llvm         LLVM ar
    // bsd          FreeBSD (and maybe other BSDs)
    // msvc         Microsoft's lib.exe
    // msvc-llvm    LLVM llvm-lib.exe
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
      semantic_version ar_version;

      process_path ranlib_path;
      string ranlib_id;
      string ranlib_signature;
      string ranlib_checksum;
    };

    // The ranlib path can be NULL, in which case no ranlib guessing will be
    // attemplated and the returned ranlib_* members will be left empty.
    //
    ar_info
    guess_ar (const path& ar, const path* ranlib, const char* paths);

    // ld information.
    //
    // Currently recognized linkers and their ids (following cc's type-variant
    // theme):
    //
    // gnu          GNU binutils ld.bfd
    // gnu-gold     GNU binutils ld.gold
    // gnu-lld      LLVM ld.lld (and older lld)
    // ld64         Apple's new linker
    // ld64-lld     LLVM ld64.lld
    // cctools      Apple's old/classic linker
    // msvc         Microsoft's link.exe
    // msvc-lld     LLVM lld-link.exe
    // wasm-lld     LLVM wasm-ld
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
    // Note that for now the version is extracted only for some linkers. Once
    // it's done for all of them, we should drop optional.
    //
    struct ld_info
    {
      process_path path;
      string id;
      string signature;
      string checksum;

      optional<semantic_version> version;
    };

    ld_info
    guess_ld (const path& ld, const char* paths);

    // rc information.
    //
    // Currently recognized resource compilers and their ids:
    //
    // gnu          GNU binutils windres
    // msvc         Microsoft's rc.exe
    // msvc-llvm    LLVM llvm-rc.exe
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
    guess_rc (const path& rc, const char* paths);
  }
}

#endif // LIBBUILD2_BIN_GUESS_HXX
