// file      : libbuild2/cc/parser.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_PARSER_HXX
#define LIBBUILD2_CC_PARSER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/diagnostics.hxx>

#include <libbuild2/cc/types.hxx>
#include <libbuild2/cc/guess.hxx> // compiler_id

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
      // The compiler_id argument should identify the compiler that has done
      // the preprocessing.
      //
      unit
      parse (ifdstream& is, const path_name& n, const compiler_id& cid)
      {
        unit r;
        parse (is, n, r, cid);
        return r;
      }

      void
      parse (ifdstream&, const path_name&, unit&, const compiler_id&);

    private:
      void
      parse_module (token&, bool, location_value);

      void
      parse_import (token&, bool);

      pair<string, bool>
      parse_module_name (token&, bool);

      void
      parse_module_part (token&, string&);

      string
      parse_header_name (token&);

    public:
      string checksum; // Translation unit checksum.

    private:
      const compiler_id* cid_;
      lexer* l_;
      unit* u_;

      optional<location_value> module_marker_;
    };
  }
}

#endif // LIBBUILD2_CC_PARSER_HXX
