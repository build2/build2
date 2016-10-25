// file      : build2/test/script/token.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/token>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      void
      token_printer (ostream& os, const token& t, bool d)
      {
        // Only quote non-name tokens for diagnostics.
        //
        const char* q (d ? "'" : "");

        switch (t.type)
        {
        case token_type::semi:         os << q << ';'    << q; break;

        case token_type::plus:         os << q << '+'    << q; break;
        case token_type::minus:        os << q << '-'    << q; break;

        case token_type::pipe:         os << q << '|'    << q; break;
        case token_type::clean:        os << q << '&'    << q; break;
        case token_type::log_and:      os << q << "&&"   << q; break;
        case token_type::log_or:       os << q << "||"   << q; break;

        case token_type::in_pass:      os << q << "<+"   << q; break;
        case token_type::in_null:      os << q << "<-"   << q; break;
        case token_type::in_str:       os << q << '<'    << q; break;
        case token_type::in_str_nn:    os << q << "<:"   << q; break;
        case token_type::in_doc:       os << q << "<<"   << q; break;
        case token_type::in_doc_nn:    os << q << "<<:"  << q; break;
        case token_type::in_file:      os << q << "<<<"  << q; break;

        case token_type::out_pass:     os << q << ">+"   << q; break;
        case token_type::out_null:     os << q << ">-"   << q; break;
        case token_type::out_str:      os << q << '>'    << q; break;
        case token_type::out_str_nn:   os << q << ">:"   << q; break;
        case token_type::out_doc:      os << q << ">>"   << q; break;
        case token_type::out_doc_nn:   os << q << ">>:"  << q; break;
        case token_type::out_file:     os << q << ">>>"  << q; break;
        case token_type::out_file_app: os << q << ">>>&" << q; break;

        default: build2::token_printer (os, t, d);
        }
      }
    }
  }
}
