// file      : build2/token.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/token>

using namespace std;

namespace build2
{
  ostream&
  operator<< (ostream& os, const token& t)
  {
    switch (t.type)
    {
    case token_type::eos:            os << "<end-of-file>"; break;
    case token_type::newline:        os << "<newline>"; break;
    case token_type::pair_separator: os << "<pair separator>"; break;
    case token_type::colon:          os << "':'"; break;
    case token_type::lcbrace:        os << "'{'"; break;
    case token_type::rcbrace:        os << "'}'"; break;
    case token_type::lsbrace:        os << "'['"; break;
    case token_type::rsbrace:        os << "']'"; break;
    case token_type::assign:         os << "'='"; break;
    case token_type::prepend:        os << "'=+'"; break;
    case token_type::append:         os << "'+='"; break;
    case token_type::equal:          os << "'=='"; break;
    case token_type::not_equal:      os << "'!='"; break;
    case token_type::less:           os << "'<'";  break;
    case token_type::greater:        os << "'>'";  break;
    case token_type::less_equal:     os << "'<='"; break;
    case token_type::greater_equal:  os << "'>='"; break;
    case token_type::dollar:         os << "'$'"; break;
    case token_type::lparen:         os << "'('"; break;
    case token_type::rparen:         os << "')'"; break;
    case token_type::name:           os << '\'' << t.value << '\''; break;
    }

    return os;
  }
}
