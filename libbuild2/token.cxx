// file      : libbuild2/token.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/token.hxx>

using namespace std;

namespace build2
{
  void
  token_printer (ostream& os, const token& t, print_mode m)
  {
    // Only quote non-name tokens for diagnostics.
    //
    const char* q (m == print_mode::diagnostics ? "'" : "");
    bool r (m == print_mode::raw);

    switch (t.type)
    {
    case token_type::eos:
      {
        if (!r)
          os <<"<end of file>";

        break;
      }
    case token_type::newline:
      {
        os << (r ? "\n" : "<newline>");
        break;
      }
    case token_type::word:
      {
        if (r)
          os << t.value;
        else
          os << '\'' << t.value << '\'';

        break;
      }
    case token_type::escape:
      {
        if (r)
          os << '\\' << t.value;
        else
          os << "<escape sequence \\" << t.value << ">";

        break;
      }
    case token_type::pair_separator:
      {
        if (r)
          os << t.value[0];
        else
          os << "<pair separator " << t.value[0] << ">";

        break;
      }

    case token_type::colon:          os << q << ':'     << q; break;
    case token_type::dollar:         os << q << '$'     << q; break;
    case token_type::question:       os << q << '?'     << q; break;
    case token_type::percent:        os << q << '%'     << q; break;
    case token_type::comma:          os << q << ','     << q; break;
    case token_type::backtick:       os << q << '`'     << q; break;

    case token_type::lparen:         os << q << '('     << q; break;
    case token_type::rparen:         os << q << ')'     << q; break;

    case token_type::lcbrace:        os << q << '{'     << q; break;
    case token_type::rcbrace:        os << q << '}'     << q; break;

    case token_type::multi_lcbrace:  os << q << t.value << q; break;
    case token_type::multi_rcbrace:  os << q << t.value << q; break;

    case token_type::lsbrace:        os << q << '['     << q; break;
    case token_type::rsbrace:        os << q << ']'     << q; break;

    case token_type::labrace:        os << q << '<'     << q; break;
    case token_type::rabrace:        os << q << '>'     << q; break;

    case token_type::assign:         os << q << '='     << q; break;
    case token_type::prepend:        os << q << "=+"    << q; break;
    case token_type::append:         os << q << "+="    << q; break;
    case token_type::default_assign: os << q << "?="    << q; break;

    case token_type::equal:          os << q << "=="    << q; break;
    case token_type::not_equal:      os << q << "!="    << q; break;
    case token_type::less:           os << q << '<'     << q; break;
    case token_type::greater:        os << q << '>'     << q; break;
    case token_type::less_equal:     os << q << "<="    << q; break;
    case token_type::greater_equal:  os << q << ">="    << q; break;

    case token_type::bit_or:         os << q << '|'     << q; break;

    case token_type::log_or:         os << q << "||"    << q; break;
    case token_type::log_and:        os << q << "&&"    << q; break;
    case token_type::log_not:        os << q << '!'     << q; break;

    default: assert (false); // Unhandled extended token.
    }
  }
}
