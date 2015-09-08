// file      : build/lexer.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/lexer>

using namespace std;

namespace build
{
  token lexer::
  next ()
  {
    lexer_mode m (mode_.top ());

    // If we are in the quoted mode, then this means we have seen a
    // variable expansion ($) and had to "break" the quoted sequence
    // into multiple "concatenated" tokens. So what we have now is
    // the "tail" of that quoted sequence which we need to continue
    // scanning. To make this work auto-magically (well, almost) we
    // are going to use a little trick: we will "pretend" that the
    // next character is the opening quote. After all, a sequence
    // like "$foo bar" is semantically equivalent to "$foo"" bar".
    //
    if (m == lexer_mode::quoted)
    {
      xchar c (peek ());

      // Detect the beginning of the "break". After that, we rely
      // on the caller switching to the variable mode.
      //
      if (c != '$')
      {
        mode_.pop ();  // As if we saw closing quote.
        c.value = '"'; // Keep line/column information.
        unget (c);
        return name (false);
      }
    }

    bool sep (skip_spaces ());

    xchar c (get ());
    uint64_t ln (c.line), cn (c.column);

    if (eos (c))
      return token (token_type::eos, sep, ln, cn);

    switch (c)
    {
      // NOTE: remember to update name() if adding new punctuations.
      //
    case '\n':
      {
        // Expire value/pairs mode at the end of the line.
        //
        if (m == lexer_mode::value || m == lexer_mode::pairs)
          mode_.pop ();

        return token (token_type::newline, sep, ln, cn);
      }
    case '{':
      {
        return token (token_type::lcbrace, sep, ln, cn);
      }
    case '}':
      {
        return token (token_type::rcbrace, sep, ln, cn);
      }
    case '$':
      {
        return token (token_type::dollar, sep, ln, cn);
      }
    case '(':
      {
        return token (token_type::lparen, sep, ln, cn);
      }
    case ')':
      {
        return token (token_type::rparen, sep, ln, cn);
      }
    }

    // Handle pair separator.
    //
    if (m == lexer_mode::pairs && c == pair_separator_)
      return token (token_type::pair_separator, sep, ln, cn);

    // The following characters are not treated as special in the
    // value or pairs mode.
    //
    if (m != lexer_mode::value && m != lexer_mode::pairs)
    {
      // NOTE: remember to update name() if adding new punctuations.
      //
      switch (c)
      {
      case ':':
        {
          return token (token_type::colon, sep, ln, cn);
        }
      case '+':
        {
          if (get () != '=')
            fail (c) << "expected = after +";

          return token (token_type::plus_equal, sep, ln, cn);
        }
      case '=':
        {
          return token (token_type::equal, sep, ln, cn);
        }
      }
    }

    // Otherwise it is a name.
    //
    unget (c);
    return name (sep);
  }

  token lexer::
  name (bool sep)
  {
    xchar c (peek ());
    assert (!eos (c));

    uint64_t ln (c.line), cn (c.column);
    string lexeme;

    lexer_mode m (mode_.top ());

    for (; !eos (c); c = peek ())
    {
      bool done (false);

      // Handle pair separator.
      //
      if (m == lexer_mode::pairs && c == pair_separator_)
        break;

      // The following characters are not treated as special in the
      // value or pairs mode.
      //
      if (m != lexer_mode::value && m != lexer_mode::pairs)
      {
        switch (c)
        {
        case ':':
        case '+':
        case '=':
          {
            done = true;
            break;
          }
        }

        if (done)
          break;
      }

      // While these extra characters are treated as the name end in
      // the variable mode.
      //
      if (m == lexer_mode::variable)
      {
        switch (c)
        {
        case '/':
        case '-':
          {
            done = true;
            break;
          }
        }

        if (done)
          break;
      }

      switch (c)
      {
      case ' ':
      case '\t':
      case '\n':
      case '#':
      case '{':
      case '}':
      case '$':
      case '(':
      case ')':
        {
          done = true;
          break;
        }
      case '\\':
        {
          get ();
          lexeme += escape ();
          break;
        }
      case '\'':
      case '\"':
        {
          // If we are in the variable mode, then treat quotes as just
          // another separator.
          //
          if (m == lexer_mode::variable)
            done = true;
          else
          {
            get ();

            if (c == '\'')
              single_quote (lexeme);
            else
            {
              mode_.push (lexer_mode::quoted);
              done = double_quote (lexeme);
            }
          }
          break;
        }
      default:
        {
          get ();
          lexeme += c;
          break;
        }
      }

      if (done)
        break;
    }

    // Expire variable mode at the end of the name.
    //
    if (m == lexer_mode::variable)
      mode_.pop ();

    return token (lexeme, sep, ln, cn);
  }

  // Assuming the previous character is the opening single quote, scan
  // the stream until the closing quote or eos, accumulating characters
  // in between in lexeme. Fail if eos is reached before the closing
  // quote.
  //
  void lexer::
  single_quote (string& lexeme)
  {
    xchar c (get ());

    for (; !eos (c) && c != '\''; c = get ())
      lexeme += c;

    if (eos (c))
      fail (c) << "unterminated single-quoted sequence";
  }

  // Assuming the previous character is the opening double quote, scan
  // the stream until the closing quote, $, or eos, accumulating
  // characters in between in lexeme. Return false if we stopped
  // because of the closing quote (which means the normal name
  // scanning can continue) and true if we stopped at $ (meaning this
  // name is done and what follows is another token). Fail if eos is
  // reached before the closing quote.
  //
  bool lexer::
  double_quote (string& lexeme)
  {
    xchar c (peek ());

    for (; !eos (c); c = peek ())
    {
      if (c == '$')
        return true;

      get ();

      if (c == '"')
      {
        mode_.pop (); // Expire quoted mode.
        return false;
      }

      lexeme += c;
    }

    fail (c) << "unterminated double-quoted sequence";
    return false; // Never reached.
  }

  bool lexer::
  skip_spaces ()
  {
    bool r (false);

    xchar c (peek ());
    bool start (c.column == 1);

    for (; !eos (c); c = peek ())
    {
      switch (c)
      {
      case ' ':
      case '\t':
        {
          r = true;
          break;
        }
      case '\n':
        {
          // Skip empty lines.
          //
          if (start)
          {
            r = false;
            break;
          }

          return r;
        }
      case '#':
        {
          get ();

          // Read until newline or eos.
          //
          for (c = peek (); !eos (c) && c != '\n'; c = peek ())
            get ();

          r = true;
          continue;
        }
      case '\\':
        {
          get ();

          if (peek () == '\n')
          {
            r = true;
            break;
          }

          unget (c);
          // Fall through.
        }
      default:
        return r; // Not a space.
      }

      get ();
    }

    return r;
  }

  lexer::xchar lexer::
  escape ()
  {
    xchar c (get ());

    if (eos (c))
      fail (c) << "unterminated escape sequence";

    return c;
  }

  location_prologue lexer::fail_mark_base::
  operator() (const xchar& c) const
  {
    return build::fail_mark_base<failed>::operator() (
      location (name_.c_str (), c.line, c.column));
  }
}
