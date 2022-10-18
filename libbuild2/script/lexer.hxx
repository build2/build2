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

    // Actual redirects (as tokens) for the the <, <<, <<<, and >, >>, >>>
    // aliases.
    //
    struct redirect_aliases
    {
      optional<token_type> l;   // <
      optional<token_type> ll;  // <<
      optional<token_type> lll; // <<<
      optional<token_type> g;   // >
      optional<token_type> gg;  // >>
      optional<token_type> ggg; // >>>

      // If the token type is a redirect alias then return the token type it
      // resolves to and the passed token type otherwise. Note that it's the
      // caller's responsibility to make sure that the corresponding alias is
      // present (normally by not recognizing absent aliases as tokens).
      //
      token_type
      resolve (token_type t) const noexcept
      {
        switch (t)
        {
        case token_type::in_l:    assert (l);   return *l;
        case token_type::in_ll:   assert (ll);  return *ll;
        case token_type::in_lll:  assert (lll); return *lll;
        case token_type::out_g:   assert (g);   return *g;
        case token_type::out_gg:  assert (gg);  return *gg;
        case token_type::out_ggg: assert (ggg); return *ggg;
        }

        return t;
      }
    };

    class lexer: public build2::lexer
    {
    public:
      using base_lexer = build2::lexer;
      using base_mode = build2::lexer_mode;

      using redirect_aliases_type = script::redirect_aliases;

      // Note that none of the name, redirect aliases, and escape arguments
      // are copied.
      //
      lexer (istream& is,
             const path_name& name,
             lexer_mode m,
             const redirect_aliases_type& ra,
             const char* escapes = nullptr)
          : base_lexer (is, name, 1 /* line */,
                        nullptr     /* escapes */,
                        false       /* set_mode */),
            redirect_aliases (ra)
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

    public:
      const redirect_aliases_type& redirect_aliases;

    protected:
      using build2::lexer::mode; // Getter.

      lexer (istream& is, const path_name& name, uint64_t line,
             const char* escapes,
             bool set_mode,
             const redirect_aliases_type& ra)
          : base_lexer (is, name, line, escapes, set_mode),
            redirect_aliases (ra) {}

      // Return the next token if it is a command operator (|, ||, &&,
      // redirect, or cleanup) and nullopt otherwise.
      //
      optional<token>
      next_cmd_op (const xchar&, // The token first character (last got char).
                   bool sep);    // The token is separated.

    private:
      token
      next_line ();

    protected:
      size_t quoted_;
    };
  }
}

#endif // LIBBUILD2_SCRIPT_LEXER_HXX
