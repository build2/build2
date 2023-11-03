// file      : libbuild2/cc/lexer.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_LEXER_HXX
#define LIBBUILD2_CC_LEXER_HXX

#include <libbutl/sha256.hxx>
#include <libbutl/char-scanner.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/diagnostics.hxx>

#include <libbuild2/cc/export.hxx>

namespace build2
{
  namespace cc
  {
    // Preprocessor-level tokenization of C/C++ source. In other words, the
    // sequence of tokens returned is similar to what a real C/C++ compiler
    // would see from its preprocessor.
    //
    // The input is a potentially (partially-)preprocessed translation unit
    // that may still contain comments, line continuations, and preprocessor
    // directives such as #line and #pragma. If the input is said to be
    // (partially-)preprocessed then #include directives are diagnosed.
    // Currently, all preprocessor directives except #line are ignored and no
    // values are saved from literals. The #line directive (and its shorthand
    // notation) is recognized to provide the logical token location. Note
    // that the modules-related pseudo-directives are not recognized or
    // handled.
    //
    // While at it we also calculate the checksum of the input ignoring
    // comments, whitespaces, etc. This is used to detect changes that do not
    // alter the resulting token stream.
    //
    enum class token_type
    {
      // NOTE: remember to update operator<<() if changing anything here!
      //
      eos,

      dot,         // .
      semi,        // ;
      colon,       // :
      scope,       // ::
      less,        // <
      greater,     // >
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
      bool       first = false;          // First token of a logical line.
      string     value;

      // Logical position.
      //
      // Note that file is a shallow pointer to the state maintained by the
      // lexer.
      //
      const path_name* file   = nullptr;
      uint64_t         line   = 0;
      uint64_t         column = 0;

      // Physical position in the stream, currently only for identifiers.
      //
      uint64_t position = 0;
    };

    // Output the token value in a format suitable for diagnostics.
    //
    LIBBUILD2_CC_SYMEXPORT ostream&
    operator<< (ostream&, const token&);

    class LIBBUILD2_CC_SYMEXPORT lexer: protected butl::char_scanner<>
    {
    public:
      // If preprocessed is true, then assume the input is at least partially
      // preprocessed and therefore should not contain #include directives.
      //
      lexer (ifdstream& is, const path_name& name, bool preprocessed)
          : char_scanner (is, false /* crlf */),
            name_ (name),
            preprocessed_ (preprocessed),
            fail ("error", &name_),
            log_file_ (name)
      {
      }

      const path_name&
      name () const {return name_;}

      string
      checksum () const {return cs_.string ();}

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
      next (token&, pair<xchar, bool /* first */>, bool);

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

      pair<xchar, bool /* first */>
      skip_spaces (bool newline = true);

      // The char_scanner adaptation for newline escape sequence processing.
      // Enabled by default and is only disabled in the raw string literals.
      //
    private:
      using base = char_scanner;

      xchar
      peek (bool escape = true);

      xchar
      get (bool escape = true);

      void
      get (const xchar& peeked);

      // Hashing versions.
      //
      xchar
      geth (bool escape = true);

      void
      geth (const xchar& peeked);

    private:
      const path_name& name_;
      bool preprocessed_;

      const fail_mark fail;

      // Logical file and line as set by the #line directives. Note that the
      // lexer diagnostics still uses the physical file/lines.
      //
      path_name_value    log_file_;
      optional<uint64_t> log_line_;

      string tmp_file_;
      sha256 cs_;
    };

    // Diagnostics plumbing.
    //
    inline location
    get_location (const token& t, const void* = nullptr)
    {
      return location (*t.file, t.line, t.column);
    }
  }
}

#endif // LIBBUILD2_CC_LEXER_HXX
