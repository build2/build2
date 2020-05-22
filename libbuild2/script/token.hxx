// file      : libbuild2/script/token.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCRIPT_TOKEN_HXX
#define LIBBUILD2_SCRIPT_TOKEN_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/token.hxx>

namespace build2
{
  namespace script
  {
    struct token_type: build2::token_type
    {
      using base_type = build2::token_type;

      enum
      {
        // NOTE: remember to update token_printer()!

        pipe = base_type::value_next, // |
        clean,                        // &{?!}     (modifiers in value)

        in_pass,                      // <|
        in_null,                      // <-
        in_file,                      // <=
        in_doc,                       // <<={:/}   (modifiers in value)
        in_str,                       // <<<={:/}  (modifiers in value)

        out_pass,                     // >|
        out_null,                     // >-
        out_trace,                    // >!
        out_merge,                    // >&
        out_file_ovr,                 // >=
        out_file_app,                 // >+
        out_file_cmp,                 // >?
        out_doc,                      // >>?{:/~}  (modifiers in value)
        out_str,                      // >>>?{:/~} (modifiers in value)

        // The modifiers are in the token value, if the redirect the alias
        // resolves to supports the modifiers.
        //
        in_l,                         // <
        in_ll,                        // <<
        in_lll,                       // <<<
        out_g,                        // >
        out_gg,                       // >>
        out_ggg,                      // >>>

        value_next
      };

      token_type () = default;
      token_type (value_type v): base_type (v) {}
      token_type (base_type v): base_type (v) {}
    };

    void
    token_printer (ostream&, const token&, print_mode);
  }
}

#endif // LIBBUILD2_SCRIPT_TOKEN_HXX
