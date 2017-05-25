// file      : build2/cc/lexer.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_CC_LEXER_HXX
#define BUILD2_CC_LEXER_HXX

#include <libbutl/char-scanner.hxx>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/diagnostics.hxx>

namespace build2
{
  namespace cc
  {
    // Preprocessor-level tokenization of C/C++ source. In other words, the
    // sequence of tokens returned is similar to what a real C/C++ compiler
    // would see from its preprocessor.
    //
    // The input is a (partially-)preprocessed translation unit that may still
    // contain comments, line continuations, and preprocessor directives such
    // as #line, #pragma, etc. Currently all preprocessor directives are
    // discarded and no values are saved for literals.
    //
    enum class token_type
    {
      // NOTE: remember to update operator<<() if changing anything here!
      //
      eos,

      dot,         // .
      semi,        // ;
      lcbrace,     // {
      rcbrace,     // }
      punctuation, // Other punctuation.

      identifier,

      number,      // Number literal.
      character,   // Char   literal.
      string,      // String literal.

      other        // Other token.
    };

    struct token
    {
      token_type type;
      string     value;

      uint64_t line;
      uint64_t column;

    public:
      token ()
          : token (token_type::eos, 0, 0) {}

      token (token_type t, uint64_t l, uint64_t c)
          : token (t, string (), l, c) {}

      token (token_type t, string v, uint64_t l, uint64_t c)
          : type (t), value (move (v)), line (l), column (c) {}
    };

    // Output the token value in a format suitable for diagnostics.
    //
    ostream&
    operator<< (ostream&, const token&);

    class lexer: protected butl::char_scanner
    {
    public:
      lexer (istream& is, const path& name)
          : char_scanner (is, false), name_ (name), fail ("error", &name_) {}

      const path&
      name () const {return name_;}

      // Note that it is ok to call next() again after getting eos.
      //
      token
      next ()
      {
        token t;
        next (t, skip_spaces (), true);
        return t;
      }

      // As above but reuse the token to avoid a (potential) memory
      // allocation. Typical usage:
      //
      // for (token t; l.next (t) != token_type::eos; )
      //   ...
      //
      token_type
      next (token& t)
      {
        next (t, skip_spaces (), true);
        return t.type;
      }

    private:
      void
      next (token&, xchar, bool);

      void
      number_literal (token&, xchar);

      void
      char_literal (token&, xchar);

      void
      string_literal (token&, xchar);

      void
      raw_string_literal (token&, xchar);

      void
      literal_suffix (xchar);

      xchar
      skip_spaces (bool newline = true);

      // The char_scanner adaptation for newline escape sequence processing.
      // Enabled by default and is only disabled in the raw string literals.
      //
    private:
      using base = char_scanner;

      xchar
      get (bool escape = true);

      void
      get (const xchar& peeked) {base::get (peeked);}

      xchar
      peek (bool escape = true);

    private:
      const path name_;
      const fail_mark fail;
    };

    // Diagnostics plumbing. We assume that any diag stream for which we can
    // use token as location has its aux data pointing to pointer to path.
    //
    inline location
    get_location (const token& t, const path& p)
    {
      return location (&p, t.line, t.column);
    }

    inline location
    get_location (const token& t, const void* data)
    {
      assert (data != nullptr); // E.g., must be &parser::path_.
      const path* p (*static_cast<const path* const*> (data));
      return get_location (t, *p);
    }
  }
}

#endif // BUILD2_CC_LEXER_HXX
