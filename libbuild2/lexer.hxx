// file      : libbuild2/lexer.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_LEXER_HXX
#define LIBBUILD2_LEXER_HXX

#include <stack>

#include <libbutl/utf8.mxx>
#include <libbutl/unicode.mxx>
#include <libbutl/char-scanner.mxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/token.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Context-dependent lexing mode.
  //
  // Quoted modes are internal and should not be set explicitly. In the value
  // mode we don't treat certain characters (e.g., `+`, `=`) as special so
  // that we can use them in the variable values, e.g., `foo = g++`. In
  // contrast, in the variable mode, we restrict certain character (e.g., `/`)
  // from appearing in the name. The values mode is like value but recogizes
  // `,` as special (used in contexts where we need to list multiple
  // values). The attributes/attribute_value modes are like values where each
  // value is potentially a variable assignment; they don't treat `{` and `}`
  // as special (so we cannot have name groups in attributes) as well as
  // recognizes `=` and `]`. The eval mode is used in the evaluation context.
  //
  // A number of modes are "derived" from the value/values mode by recognizing
  // a few extra characters:
  //
  //   switch_expressions  values plus `:`
  //   case_patterns       values plus `|` and `:`
  //
  // Note that the normal, value/values and derived, as well as eval modes
  // split words separated by the pair character (to disable pairs one can
  // pass `\0` as a pair character).
  //
  // The normal mode recognizes `%` and `{{...` at the beginning of the line
  // as special. The cmdvar mode is like normal but does not treat these
  // character sequences as special.
  //
  // Finally, the foreign mode reads everything until encountering a line that
  // contains nothing (besides whitespaces) other than the closing multi-
  // curly-brace (`}}...`) (or eos) returning the contents as the word token
  // followed by the multi_rcbrace (or eos). In a way it is similar to the
  // single-quote mode. The number of closing braces to expect is passed as
  // mode data.
  //
  // The alternative modes must be set manually. The value/values and derived
  // modes automatically expires after the end of the line. The attribute mode
  // expires after the closing `]`. The variable mode expires after the word
  // token. The eval mode expires after the closing `)`. And the foreign mode
  // expires after the closing braces.
  //
  // Note that normally it is only safe to switch mode when the current token
  // is not quoted (or, more generally, when you are not in the double-quoted
  // mode) unless the mode treats the double-quote as a separator (e.g.,
  // variable name mode). Failed that your mode (which now will be the top of
  // the mode stack) will prevent proper recognition of the closing quote.
  //
  // Finally, attributes recognition (the `[` token) cuts across most of the
  // modes and is handled with a flag. In the normal mode it is automatically
  // set at the beginning and after each newline. In all other modes it must
  // be explicitly set at points where attributes are recognized. In all the
  // cases it is automatically reset after lexing the next token (whether `[`
  // or not).
  //

  // Extendable/inheritable enum-like class.
  //
  struct lexer_mode: lexer_mode_base
  {
    using base_type = lexer_mode_base;

    enum
    {
      normal = base_type::value_next,
      cmdvar,
      variable,
      value,
      values,
      case_patterns,
      switch_expressions,
      attributes,
      attribute_value,
      eval,
      single_quoted,
      double_quoted,
      foreign,
      buildspec,

      value_next
    };

    lexer_mode () = default;
    lexer_mode (value_type v): base_type (v) {}
    lexer_mode (base_type v): base_type (v) {}
  };

  class LIBBUILD2_SYMEXPORT lexer:
    public butl::char_scanner<butl::utf8_validator>
  {
  public:
    // If escape is not NULL then only escape sequences with characters from
    // this string are considered "effective escapes" with all others passed
    // through as is. Note that neither the name nor escape arguments are
    // copied.
    //
    lexer (istream& is,
           const path_name& name,
           uint64_t line = 1, // Start line in the stream.
           const char* escapes = nullptr)
      : lexer (is, name, line, escapes, true /* set_mode */) {}

    const path_name&
    name () const {return name_;}

    // Note: sets mode for the next token. The second argument can be used to
    // specify the pair separator character (if the mode supports pairs). If
    // escapes is not specified, then inherit the current mode's (though a
    // mode can also override it).
    //
    virtual void
    mode (lexer_mode,
          char pair_separator = '\0',
          optional<const char*> escapes = nullopt,
          uintptr_t data = 0);

    // Enable attributes recognition for the next token.
    //
    void
    enable_attributes () {state_.top ().attributes = true;}

    // Expire the current mode early.
    //
    void
    expire_mode () {state_.pop ();}

    lexer_mode
    mode () const {return state_.top ().mode;}

    char
    pair_separator () const {return state_.top ().sep_pair;}

    // Scanner. Note that it is ok to call next() again after getting eos.
    //
    // If you extend the lexer and add a custom lexer mode, then you must
    // override next() and handle the custom mode there.
    //
    virtual token
    next ();

    // Peek at the first two characters of the next token(s). Return the
    // characters or '\0' if either would be eos. Also return an indicator of
    // whether the next token would be separated. Note: cannot be used to peek
    // at the first character of a line.
    //
    // Note also that it assumes that the current mode and the potential new
    // mode in which these characters will actually be parsed use the same
    // whitespace separation (the sep_space and sep_newline values).
    //
    pair<pair<char, char>, bool>
    peek_chars ();

  protected:
    struct state
    {
      lexer_mode      mode;
      uintptr_t       data;
      optional<token> hold;

      bool       attributes;

      char sep_pair;
      bool sep_space;    // Are whitespaces separators (see skip_spaces())?
      bool sep_newline;  // Is newline special (see skip_spaces())?
      bool quotes;       // Recognize quoted fragments.

      const char* escapes; // Effective escape sequences to recognize.

      // Word separator characters. For two-character sequence put the first
      // one in sep_first and the second one in the corresponding position of
      // sep_second. If it's a single-character sequence, then put space in
      // sep_second. If there are multiple sequences that start with the same
      // character, then repeat the first character in sep_first.
      //
      const char* sep_first;
      const char* sep_second;
    };

    token
    next_eval ();

    token
    next_quoted ();

    token
    next_foreign ();

    // Lex a word assuming current is the top state (which may already have
    // been "expired" from the top).
    //
    virtual token
    word (state current, bool separated);

    // Return true in first if we have seen any spaces. Skipped empty lines
    // don't count. In other words, we are only interested in spaces that are
    // on the same line as the following non-space character. Return true in
    // second if we have started skipping spaces from column 1 (note that
    // if this mode does not skip spaces, then second will always be false).
    //
    pair<bool, bool>
    skip_spaces ();

    // Diagnostics.
    //
  protected:
    fail_mark fail;

    // Lexer state.
    //
  protected:
    lexer (istream& is, const path_name& name, uint64_t line,
           const char* escapes,
           bool set_mode)
      : char_scanner (is,
                      butl::utf8_validator (butl::codepoint_types::graphic,
                                            U"\n\r\t"),
                      true /* crlf */,
                      line),
        fail ("error", &name),
        name_ (name),
        sep_ (false)
    {
      if (set_mode)
        mode (lexer_mode::normal, '@', escapes);
    }

    const path_name& name_;
    std::stack<state> state_;

    bool sep_; // True if we skipped spaces in peek().
  };
}

// Diagnostics plumbing.
//
namespace butl // ADL
{
  inline build2::location
  get_location (const butl::char_scanner<butl::utf8_validator>::xchar& c,
                const void* data)
  {
    using namespace build2;

    assert (data != nullptr); // E.g., must be &lexer::name_.
    return location (*static_cast<const path_name*> (data), c.line, c.column);
  }
}

#endif // LIBBUILD2_LEXER_HXX
