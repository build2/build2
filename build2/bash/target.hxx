// file      : build2/bash/target.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_BASH_TARGET_HXX
#define BUILD2_BASH_TARGET_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>

namespace build2
{
  namespace bash
  {
    // Bash module file to be sourced by a script. The default/standard
    // extension is .bash.
    //
    class bash: public file
    {
    public:
      using file::file;

    public:
      static const target_type static_type;
      virtual const target_type& dynamic_type () const {return static_type;}
    };
  }
}

#endif // BUILD2_BASH_TARGET_HXX
