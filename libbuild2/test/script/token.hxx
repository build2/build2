// file      : libbuild2/test/script/token.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_SCRIPT_TOKEN_HXX
#define LIBBUILD2_TEST_SCRIPT_TOKEN_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/token.hxx>

namespace build2
{
  namespace test
  {
    namespace script
    {
      struct token_type: build2::token_type
      {
        using base_type = build2::token_type;

        enum
        {
          // NOTE: remember to update token_printer()!

          semi = base_type::value_next, // ;

          dot,                          // .

          plus,                         // +
          minus,                        // -

          pipe,                         // |
          clean,                        // &{?!}   (modifiers in value)

          in_pass,                      // <|
          in_null,                      // <-
          in_str,                       // <{:}    (modifiers in value)
          in_doc,                       // <<{:}   (modifiers in value)
          in_file,                      // <<<

          out_pass,                     // >|
          out_null,                     // >-
          out_trace,                    // >!
          out_merge,                    // >&
          out_str,                      // >{:~}   (modifiers in value)
          out_doc,                      // >>{:~}  (modifiers in value)
          out_file_cmp,                 // >>>
          out_file_ovr,                 // >=
          out_file_app                  // >+
        };

        token_type () = default;
        token_type (value_type v): base_type (v) {}
        token_type (base_type v): base_type (v) {}
      };

      void
      token_printer (ostream&, const token&, bool);
    }
  }
}

#endif // LIBBUILD2_TEST_SCRIPT_TOKEN_HXX
