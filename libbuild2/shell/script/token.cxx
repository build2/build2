// file      : libbuild2/shell/script/token.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/shell/script/token.hxx>

using namespace std;

namespace build2
{
  namespace shell
  {
    namespace script
    {
      void
      token_printer (ostream& os, const token& t, print_mode m)
      {
        // No shellscript-specific tokens so far.
        //
        build2::script::token_printer (os, t, m);
      }
    }
  }
}
