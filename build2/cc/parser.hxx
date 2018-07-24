// file      : build2/cc/parser.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_PARSER_HXX
#define BUILD2_CC_PARSER_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/diagnostics.hxx>

#include <build2/cc/types.hxx>

namespace build2
{
  namespace cc
  {
    // Extract translation unit information from a preprocessed C/C++ source.
    //
    struct token;
    class lexer;

    class parser
    {
    public:
      translation_unit
      parse (ifdstream&, const path& name);

    private:
      void
      parse_import (token&, bool);

      void
      parse_module (token&, bool);

      string
      parse_module_name (token&);

    public:
      string checksum; // Translation unit checksum.

    private:
      lexer* l_;
      translation_unit* u_;

      optional<location> module_marker_;
    };
  }
}

#endif // BUILD2_CC_PARSER_HXX
