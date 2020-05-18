// file      : libbuild2/test/script/token.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/script/token.hxx>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      void
      token_printer (ostream& os, const token& t, print_mode m)
      {
        // Only quote non-name tokens for diagnostics.
        //
        const char* q (m == print_mode::diagnostics ? "'" : "");

        switch (t.type)
        {
        case token_type::semi:  os << q << ';' << q; break;

        case token_type::dot:   os << q << '.' << q; break;

        case token_type::plus:  os << q << '+' << q; break;
        case token_type::minus: os << q << '-' << q; break;

        default: build2::script::token_printer (os, t, m);
        }
      }
    }
  }
}
