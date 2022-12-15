// file      : libbuild2/test/script/lexer.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_SCRIPT_LEXER_HXX
#define LIBBUILD2_TEST_SCRIPT_LEXER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/script/lexer.hxx>

#include <libbuild2/test/script/token.hxx>

namespace build2
{
  namespace test
  {
    namespace script
    {
      struct lexer_mode: build2::script::lexer_mode
      {
        using base_type = build2::script::lexer_mode;

        enum
        {
          command_line = base_type::value_next,
          first_token,      // Expires at the end of the token.
          second_token,     // Expires at the end of the token.
          variable_line,    // Expires at the end of the line.
          description_line, // Expires at the end of the line.
          for_loop          // Used for sensing the for-loop leading tokens.
        };

        lexer_mode () = default;
        lexer_mode (value_type v): base_type (v) {}
        lexer_mode (build2::lexer_mode v): base_type (v) {}
      };

      class lexer: public build2::script::lexer
      {
      public:
        using base_lexer = build2::script::lexer;

        // Note that neither the name nor escape arguments are copied.
        //
        lexer (istream& is,
               const path_name& name,
               lexer_mode m,
               const char* escapes = nullptr)
            : base_lexer (is, name, 1 /* line */,
                          nullptr     /* escapes */,
                          false       /* set_mode */,
                          redirect_aliases)
        {
          mode (m, '\0', escapes);
        }

        virtual void
        mode (build2::lexer_mode,
              char = '\0',
              optional<const char*> = nullopt,
              uintptr_t = 0) override;

        virtual token
        next () override;

      public:
        static redirect_aliases_type redirect_aliases;

      private:
        using build2::script::lexer::mode; // Getter.

        token
        next_line ();

        token
        next_description ();

        virtual token
        word (const state&, bool) override;
      };
    }
  }
}

#endif // LIBBUILD2_TEST_SCRIPT_LEXER_HXX
