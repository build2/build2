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
    if (mode_ != next_mode_)
    {
      prev_mode_ = mode_;
      mode_ = next_mode_;
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
        // Restore the normal mode at the end of the line.
        //
        if (mode_ == lexer_mode::value || mode_ == lexer_mode::pairs)
          mode_ = next_mode_ = lexer_mode::normal;

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
        // The following name is lexed in the variable mode.
        //
        next_mode_ = lexer_mode::variable;

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
    if (mode_ == lexer_mode::pairs && c == pair_separator_)
      return token (token_type::pair_separator, sep, ln, cn);

    // The following characters are not treated as special in the
    // value or pairs mode.
    //
    if (mode_ != lexer_mode::value && mode_ != lexer_mode::pairs)
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

          next_mode_ = lexer_mode::value;
          return token (token_type::plus_equal, sep, ln, cn);
        }
      case '=':
        {
          next_mode_ = lexer_mode::value;
          return token (token_type::equal, sep, ln, cn);
        }
      }
    }

    // Otherwise it is a name.
    //
    return name (c, sep);
  }

  token lexer::
  name (xchar c, bool sep)
  {
    uint64_t ln (c.line), cn (c.column);
    string lexeme;
    lexeme += (c != '\\' ? c : escape ());

    for (c = peek (); !eos (c); c = peek ())
    {
      bool done (false);

      // Handle pair separator.
      //
      if (mode_ == lexer_mode::pairs && c == pair_separator_)
        break;

      // The following characters are not treated as special in the
      // value or pairs mode.
      //
      if (mode_ != lexer_mode::value && mode_ != lexer_mode::pairs)
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
      if (mode_ == lexer_mode::variable)
      {
        switch (c)
        {
        case '/':
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

    if (mode_ == lexer_mode::variable)
      next_mode_ = prev_mode_;

    return token (lexeme, sep, ln, cn);
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
