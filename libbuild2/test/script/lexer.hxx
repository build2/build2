// file      : libbuild2/test/script/lexer.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TEST_SCRIPT_LEXER_HXX
#define LIBBUILD2_TEST_SCRIPT_LEXER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/lexer.hxx>

#include <libbuild2/test/script/token.hxx>

namespace build2
{
  namespace test
  {
    namespace script
    {
      struct lexer_mode: build2::lexer_mode
      {
        using base_type = build2::lexer_mode;

        enum
        {
          command_line = base_type::value_next,
          first_token,     // Expires at the end of the token.
          second_token,    // Expires at the end of the token.
          variable_line,   // Expires at the end of the line.
          command_expansion,
          here_line_single,
          here_line_double,
          description_line // Expires at the end of the line.
        };

        lexer_mode () = default;
        lexer_mode (value_type v): base_type (v) {}
        lexer_mode (base_type v): base_type (v) {}
      };

      class lexer: public build2::lexer
      {
      public:
        using base_lexer = build2::lexer;
        using base_mode = build2::lexer_mode;

        // Note that neither the name nor escape arguments are copied.
        //
        lexer (istream& is,
               const path_name& name,
               lexer_mode m,
               const char* escapes = nullptr)
            : base_lexer (is, name, 1 /* line */,
                          nullptr     /* escapes */,
                          false       /* set_mode */)
        {
          mode (m, '\0', escapes);
        }

        virtual void
        mode (base_mode,
              char = '\0',
              optional<const char*> = nullopt,
              uintptr_t = 0) override;

        // Number of quoted (double or single) tokens since last reset.
        //
        size_t
        quoted () const {return quoted_;}

        void
        reset_quoted (size_t q) {quoted_ = q;}

        virtual token
        next () override;

      protected:
        token
        next_line ();

        token
        next_description ();

        virtual token
        word (state, bool) override;

      protected:
        size_t quoted_;
      };
    }
  }
}

#endif // LIBBUILD2_TEST_SCRIPT_LEXER_HXX
