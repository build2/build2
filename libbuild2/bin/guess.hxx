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
    // llvm         LLVM llvm-ar
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
    // The environment is an optional list of environment variables that
    // affect ar/ranlib result.
    //
    // Watch out for the environment not to affect any of the extracted
    // information since we cache it.
    //
    struct ar_info
    {
      process_path ar_path;
      string ar_id;
      string ar_signature;
      string ar_checksum;
      semantic_version ar_version;
      const char* const* ar_environment;

      process_path ranlib_path;
      string ranlib_id;
      string ranlib_signature;
      string ranlib_checksum;
      const char* const* ranlib_environment;
    };

    // The ranlib path can be NULL, in which case no ranlib guessing will be
    // attemplated and the returned ranlib_* members will be left empty.
    //
    const ar_info&
    guess_ar (context&, const path& ar, const path* ranlib, const char* paths);

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
    // The environment is an optional list of environment variables that
    // affect the linker result.
    //
    // Watch out for the environment not to affect any of the extracted
    // information since we cache it.
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
      const char* const* environment;
    };

    const ld_info&
    guess_ld (context&, const path& ld, const char* paths);

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
    // The environment is an optional list of environment variables that
    // affect the resource compiler result.
    //
    // Watch out for the environment not to affect any of the extracted
    // information since we cache it.
    //
    struct rc_info
    {
      process_path path;
      string id;
      string signature;
      string checksum;
      const char* const* environment;
    };

    const rc_info&
    guess_rc (context&, const path& rc, const char* paths);

    // nm information.
    //
    // Currently recognized nm and nm-like utilities and their ids:
    //
    // gnu           GNU binutils nm
    // msvc          Microsoft's dumpbin.exe
    // llvm          LLVM llvm-nm
    // elftoolchain  ELF Toolchain (used by FreeBSD)
    // generic       Other/generic/unrecognized (including Mac OS X)
    //
    // The signature is normally the --version line.
    //
    // The checksum is used to detect nm changes. It is calculated in a
    // toolchain-specific manner (usually the output of --version) and is not
    // bulletproof.
    //
    // The environment is an optional list of environment variables that
    // affect the resource compiler result.
    //
    // Watch out for the environment not to affect any of the extracted
    // information since we cache it.
    //
    struct nm_info
    {
      process_path path;
      string id;
      string signature;
      string checksum;
      const char* const* environment;
    };

    const nm_info&
    guess_nm (context&, const path& nm, const char* paths);
  }
}

#endif // LIBBUILD2_BIN_GUESS_HXX
