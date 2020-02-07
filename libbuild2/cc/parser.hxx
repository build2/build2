// file      : libbuild2/cc/parser.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_PARSER_HXX
#define LIBBUILD2_CC_PARSER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/diagnostics.hxx>

#include <libbuild2/cc/types.hxx>

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
      unit
      parse (ifdstream&, const path_name&);

    private:
      void
      parse_import (token&, bool);

      void
      parse_module (token&, bool);

      string
      parse_module_name (token&);

      string
      parse_header_name (token&);

    public:
      string checksum; // Translation unit checksum.

    private:
      lexer* l_;
      unit* u_;

      optional<location_value> module_marker_;
    };
  }
}

#endif // LIBBUILD2_CC_PARSER_HXX
