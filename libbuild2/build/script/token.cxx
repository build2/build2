// file      : libbuild2/build/script/token.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/token.hxx>

using namespace std;

namespace build2
{
  namespace build
  {
    namespace script
    {
      void
      token_printer (ostream& os, const token& t, bool d)
      {
        // No build script-specific tokens so far.
        //
        build2::script::token_printer (os, t, d);
      }
    }
  }
}
