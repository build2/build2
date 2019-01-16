// file      : build2/test/script/token.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/token.hxx>

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
        const string& v (t.value);

        // Only quote non-name tokens for diagnostics.
        //
        const char* q (d ? "'" : "");

        switch (t.type)
        {
        case token_type::semi:         os << q << ';'        << q; break;

        case token_type::dot:          os << q << '.'        << q; break;

        case token_type::plus:         os << q << '+'        << q; break;
        case token_type::minus:        os << q << '-'        << q; break;

        case token_type::clean:        os << q << '&'   << v << q; break;
        case token_type::pipe:         os << q << '|'        << q; break;

        case token_type::in_pass:      os << q << "<|"       << q; break;
        case token_type::in_null:      os << q << "<-"       << q; break;
        case token_type::in_str:       os << q << '<'   << v << q; break;
        case token_type::in_doc:       os << q << "<<"  << v << q; break;
        case token_type::in_file:      os << q << "<<<"      << q; break;

        case token_type::out_pass:     os << q << ">|"       << q; break;
        case token_type::out_null:     os << q << ">-"       << q; break;
        case token_type::out_trace:    os << q << ">!"       << q; break;
        case token_type::out_merge:    os << q << ">&"       << q; break;
        case token_type::out_str:      os << q << '>'   << v << q; break;
        case token_type::out_doc:      os << q << ">>"  << v << q; break;
        case token_type::out_file_cmp: os << q << ">>>" << v << q; break;
        case token_type::out_file_ovr: os << q << ">="  << v << q; break;
        case token_type::out_file_app: os << q << ">+"  << v << q; break;

        default: build2::token_printer (os, t, d);
        }
      }
    }
  }
}
