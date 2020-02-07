// file      : libbuild2/bash/target.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BASH_TARGET_HXX
#define LIBBUILD2_BASH_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

#include <libbuild2/bash/export.hxx>

namespace build2
{
  namespace bash
  {
    // Bash module file to be sourced by a script. The default/standard
    // extension is .bash.
    //
    class LIBBUILD2_BASH_SYMEXPORT bash: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };
  }
}

#endif // LIBBUILD2_BASH_TARGET_HXX
