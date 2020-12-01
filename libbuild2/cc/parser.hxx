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
      parse (ifdstream& is, const path_name& n)
      {
        unit r;
        parse (is, n, r);
        return r;
      }

      void
      parse (ifdstream&, const path_name&, unit&);

    private:
      void
      parse_module (token&, bool, location_value);

      void
      parse_import (token&, bool);

      pair<string, bool>
      parse_module_name (token&, bool);

      string
      parse_module_part (token& t)
      {
        string n;
        parse_module_part (t, n);
        return n;
      }

      void
      parse_module_part (token&, string&);

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
