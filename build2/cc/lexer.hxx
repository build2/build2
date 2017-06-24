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
    // as #line, #pragma, but not #include (which is diagnosed). Currently,
    // all preprocessor directives except #line are ignored and no values are
    // saved from literals. The #line directive (and its shorthand notation)
    // is recognized to provide the logical token location.
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
      token_type type = token_type::eos;
      string     value;

      // Logical position.
      //
      path     file;
      uint64_t line   = 0;
      uint64_t column = 0;

      // Physical position in the stream, currently only for identifiers and
      // only if the stream is ifdstream.
      //
      uint64_t position = 0;
    };

    // Output the token value in a format suitable for diagnostics.
    //
    ostream&
    operator<< (ostream&, const token&);

    class lexer: protected butl::char_scanner
    {
    public:
      lexer (istream& is, const path& name)
          : char_scanner (is, false),
            name_ (name),
            fail ("error", &name_),
            log_file_ (name) {}

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

      void
      line_directive (token&, xchar);

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
      get (const xchar& peeked);

      xchar
      peek (bool escape = true);

    private:
      const path name_;
      const fail_mark fail;

      // Logical file and line as set by the #line directives. Note that the
      // lexer diagnostics still uses the physical file/lines.
      //
      path               log_file_;
      optional<uint64_t> log_line_;
    };

    // Diagnostics plumbing.
    //
    inline location
    get_location (const token& t, const void*)
    {
      return location (&t.file, t.line, t.column);
    }
  }
}

#endif // BUILD2_CC_LEXER_HXX
