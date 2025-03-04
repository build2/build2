// file      : libbuild2/shell/script/token.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SHELL_SCRIPT_TOKEN_HXX
#define LIBBUILD2_SHELL_SCRIPT_TOKEN_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/script/token.hxx>

namespace build2
{
  namespace shell
  {
    namespace script
    {
      struct token_type: build2::script::token_type
      {
        using base_type = build2::script::token_type;

        // No shellscript-specific tokens so far.
        //

        token_type () = default;
        token_type (value_type v): base_type (v) {}
        token_type (build2::token_type v): base_type (v) {}
      };

      void
      token_printer (ostream&, const token&, print_mode);
    }
  }
}

#endif // LIBBUILD2_SHELL_SCRIPT_TOKEN_HXX
