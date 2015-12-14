// file      : build/token.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/token>

#include <ostream>

using namespace std;

namespace build
{
  ostream&
  operator<< (ostream& os, const token& t)
  {
    switch (t.type)
    {
    case token_type::eos:            os << "<end-of-file>"; break;
    case token_type::newline:        os << "<newline>"; break;
    case token_type::pair_separator: os << "<pair separator>"; break;
    case token_type::colon:          os << ":"; break;
    case token_type::lcbrace:        os << "{"; break;
    case token_type::rcbrace:        os << "}"; break;
    case token_type::equal:          os << "="; break;
    case token_type::equal_plus:     os << "=+"; break;
    case token_type::plus_equal:     os << "+="; break;
    case token_type::dollar:         os << "$"; break;
    case token_type::lparen:         os << "("; break;
    case token_type::rparen:         os << ")"; break;
    case token_type::name:           os << t.value; break;
    }

    return os;
  }
}
