// file      : libbuild2/script/token.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/script/token.hxx>

using namespace std;

namespace build2
{
  namespace script
  {
    void
    token_printer (ostream& os, const token& t, print_mode m)
    {
      const string& v (t.value);

      // Only quote non-name tokens for diagnostics.
      //
      const char* q (m == print_mode::diagnostics ? "'" : "");

      switch (t.type)
      {
      case token_type::clean:        os << q << '&'    << v << q; break;
      case token_type::pipe:         os << q << '|'         << q; break;

      case token_type::in_pass:      os << q << "<|"        << q; break;
      case token_type::in_null:      os << q << "<-"        << q; break;
      case token_type::in_file:      os << q << "<="        << q; break;
      case token_type::in_doc:       os << q << "<<="  << v << q; break;
      case token_type::in_str:       os << q << "<<<=" << v << q; break;

      case token_type::out_pass:     os << q << ">|"        << q; break;
      case token_type::out_null:     os << q << ">-"        << q; break;
      case token_type::out_trace:    os << q << ">!"        << q; break;
      case token_type::out_merge:    os << q << ">&"        << q; break;
      case token_type::out_file_ovr: os << q << ">="        << q; break;
      case token_type::out_file_app: os << q << ">+"        << q; break;
      case token_type::out_file_cmp: os << q << ">?"        << q; break;
      case token_type::out_doc:      os << q << ">>?"  << v << q; break;
      case token_type::out_str:      os << q << ">>>?" << v << q; break;

      case token_type::in_l:         os << q << '<'    << v << q; break;
      case token_type::in_ll:        os << q << "<<"   << v << q; break;
      case token_type::in_lll:       os << q << "<<<"  << v << q; break;
      case token_type::out_g:        os << q << '>'    << v << q; break;
      case token_type::out_gg:       os << q << ">>"   << v << q; break;
      case token_type::out_ggg:      os << q << ">>>"  << v << q; break;

      default: build2::token_printer (os, t, m);
      }
    }
  }
}
