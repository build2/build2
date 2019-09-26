// file      : libbuild2/lexer.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_LEXER_HXX
#define LIBBUILD2_LEXER_HXX

#include <stack>

#include <libbutl/char-scanner.mxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/token.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Context-dependent lexing mode. Quoted modes are internal and should not
  // be set explicitly. In the value mode we don't treat certain characters
  // (e.g., '+', '=') as special so that we can use them in the variable
  // values, e.g., 'foo = g++'. In contrast, in the variable mode, we restrict
  // certain character (e.g., '/') from appearing in the name. The values mode
  // is like value but recogizes ',' as special (used in contexts where we
  // need to list multiple values). The attribute mode is also like value
  // except it doesn't treat '{' and '}' as special (so we cannot have name
  // groups in attributes). The eval mode is used in the evaluation context.
  //
  // A number of modes are "derived" from the value/values mode by recognizing
  // a few extra characters: case_patterns (values plus '|').
  //
  // Note that the normal, value/values and derived, as well as eval modes
  // split words separated by the pair character (to disable pairs one can
  // pass '\0' as a pair character).
  //
  // The alternative modes must be set manually. The value/values and derived
  // modes automatically expires after the end of the line. The attribute mode
  // expires after the closing ']'. The variable mode expires after the word
  // token. And the eval mode expires after the closing ')'.
  //
  // Note that normally it is only safe to switch mode when the current token
  // is not quoted (or, more generally, when you are not in the double-quoted
  // mode) unless the mode treats the double-quote as a separator (e.g.,
  // variable name mode). Failed that your mode (which now will be the top of
  // the mode stack) will prevent proper recognition of the closing quote.
  //

  // Extendable/inheritable enum-like class.
  //
  struct lexer_mode: lexer_mode_base
  {
    using base_type = lexer_mode_base;

    enum
    {
      normal = base_type::value_next,
      variable,
      value,
      values,
      case_patterns,
      attribute,
      eval,
      single_quoted,
      double_quoted,
      buildspec,

      value_next
    };

    lexer_mode () = default;
    lexer_mode (value_type v): base_type (v) {}
    lexer_mode (base_type v): base_type (v) {}
  };

  class LIBBUILD2_SYMEXPORT lexer: public butl::char_scanner
  {
  public:
    // If escape is not NULL then only escape sequences with characters from
    // this string are considered "effective escapes" with all others passed
    // through as is. Note that the escape string is not copied.
    //
    lexer (istream& is,
           const path& name,
           uint64_t line = 1, // Start line in the stream.
           const char* escapes = nullptr)
        : lexer (is, name, line, escapes, true /* set_mode */) {}

    const path&
    name () const {return name_;}

    // Note: sets mode for the next token. The second argument can be used to
    // specifythe pair separator character (if the mode supports pairs). If
    // escapes not specified, then inherit the current mode's (thought a mode
    // can also override it).
    //
    virtual void
    mode (lexer_mode,
          char pair_separator = '\0',
          optional<const char*> escapes = nullopt);

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

    // Peek at the first character of the next token. Return the character
    // or '\0' if the next token will be eos. Also return an indicator of
    // whether the next token will be separated.
    //
    pair<char, bool>
    peek_char ();

  protected:
    struct state
    {
      lexer_mode mode;

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

    // Lex a word assuming current is the top state (which may already have
    // been "expired" from the top).
    //
    virtual token
    word (state current, bool separated);

    // Return true if we have seen any spaces. Skipped empty lines
    // don't count. In other words, we are only interested in spaces
    // that are on the same line as the following non-space character.
    //
    bool
    skip_spaces ();

    // Diagnostics.
    //
  protected:
    fail_mark fail;

    // Lexer state.
    //
  protected:
    lexer (istream& is,
           const path& name,
           uint64_t line,
           const char* escapes,
           bool set_mode)
        : char_scanner (is, true /* crlf */, line),
          fail ("error", &name_),
          name_ (name),
          sep_ (false)
    {
      if (set_mode)
        mode (lexer_mode::normal, '@', escapes);
    }

    const path name_;
    std::stack<state> state_;

    bool sep_; // True if we skipped spaces in peek().
  };
}

// Diagnostics plumbing.
//
namespace butl // ADL
{
  inline build2::location
  get_location (const butl::char_scanner::xchar& c, const void* data)
  {
    using namespace build2;

    assert (data != nullptr); // E.g., must be &lexer::name_.
    return location (static_cast<const path*> (data), c.line, c.column);
  }
}

#endif // LIBBUILD2_LEXER_HXX
