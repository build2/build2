// file      : libbuild2/script/lexer.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCRIPT_LEXER_HXX
#define LIBBUILD2_SCRIPT_LEXER_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/lexer.hxx>

#include <libbuild2/script/token.hxx>

namespace build2
{
  namespace script
  {
    struct lexer_mode: build2::lexer_mode
    {
      using base_type = build2::lexer_mode;

      enum
      {
        command_expansion = base_type::value_next,
        here_line_single,
        here_line_double,

        value_next
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
            optional<const char*> = nullopt) override;

      // Number of quoted (double or single) tokens since last reset.
      //
      size_t
      quoted () const {return quoted_;}

      void
      reset_quoted (size_t q) {quoted_ = q;}

      virtual token
      next () override;

    protected:
      lexer (istream& is, const path_name& name, uint64_t line,
             const char* escapes,
             bool set_mode)
          : base_lexer (is, name, line, escapes, set_mode) {}

      // Return the next token if it is a command operator (|, ||, &&,
      // redirect, or cleanup) and nullopt otherwise.
      //
      optional<token>
      next_cmd_op (const xchar&, // The token first character (last got char).
                   bool sep,     // The token is separated.
                   lexer_mode);  // The current (potentially "expired") mode.

    private:
      token
      next_line ();

    protected:
      size_t quoted_;
    };
  }
}

#endif // LIBBUILD2_SCRIPT_LEXER_HXX
