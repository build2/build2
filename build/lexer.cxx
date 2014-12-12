// file      : build/lexer.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/lexer>

#include <iostream>

using namespace std;

namespace build
{
  token lexer::
  next ()
  {
    skip_spaces ();

    xchar c (get ());
    uint64_t ln (c.line ()), cn (c.column ());

    if (is_eos (c))
      return token (ln, cn);

    switch (c)
    {
      // NOTE: remember to update name() if adding new punctuations.
      //
    case '\n':
      {
        return token (token_punctuation::newline, ln, cn);
      }
    case ':':
      {
        return token (token_punctuation::colon, ln, cn);
      }
    case '{':
      {
        return token (token_punctuation::lcbrace, ln, cn);
      }
    case '}':
      {
        return token (token_punctuation::rcbrace, ln, cn);
      }
    }

    // Otherwise it is a name.
    //
    return name (c);
  }

  lexer::xchar lexer::
  escape ()
  {
    xchar c (get ());

    if (!is_eos (c))
      return c;

    if (!name_.empty ())
      cerr << name_ << ':' << c.line () << ':' << c.column () << ": error: " <<
        "unterminated escape sequence" << endl;

    throw lexer_error ();
  }

  void lexer::
  skip_spaces ()
  {
    xchar c (peek ());
    bool start (c.column () == 1);

    for (; !is_eos (c); c = peek ())
    {
      switch (c)
      {
      case ' ':
      case '\t':
        break;
      case '\n':
        {
          // Skip empty lines.
          //
          if (start)
            break;

          return;
        }
      case '#':
        {
          get ();

          // Read until newline or eos.
          //
          for (c = peek (); !is_eos (c) && c != '\n'; c = peek ())
            get ();
          continue;
        }
      case '\\':
        {
          get ();

          if (peek () == '\n')
            break;

          unget (c);
          // Fall through.
        }
      default:
        return; // Not a space.
      }

      get ();
    }
  }

  token lexer::
  name (xchar c)
  {
    uint64_t ln (c.line ()), cn (c.column ());
    string lexeme;
    lexeme += (c != '\\' ? c : escape ());

    for (c = peek (); !is_eos (c); c = peek ())
    {
      switch (c)
      {
      case ' ':
      case '\t':
      case '\n':
      case ':':
      case '{':
      case '}':
      case '#':
        {
          break;
        }
      case '\\':
        {
          get ();
          lexeme += escape ();
          continue;
        }
      default:
        {
          get ();
          lexeme += c;
          continue;
        }
      }

      break;
    }

    return token (lexeme, ln, cn);
  }

  lexer::xchar lexer::
  peek ()
  {
    if (unget_)
      return buf_;
    else
    {
      if (eos_)
        return xchar (xchar::traits_type::eof (), l_, c_);
      else
      {
        xchar::int_type v (is_.peek ());

        if (v == xchar::traits_type::eof ())
          eos_ = true;

        return xchar (v, l_, c_);
      }
    }
  }

  lexer::xchar lexer::
  get ()
  {
    if (unget_)
    {
      unget_ = false;
      return buf_;
    }
    else
    {
      // When is_.get () returns eof, the failbit is also set (stupid,
      // isn't?) which may trigger an exception. To work around this
      // we will call peek() first and only call get() if it is not
      // eof. But we can only call peek() on eof once; any subsequent
      // calls will spoil the failbit (even more stupid).
      //
      xchar c (peek ());

      if (!is_eos (c))
      {
        is_.get ();

        if (c == '\n')
        {
          l_++;
          c_ = 1;
        }
        else
          c_++;
      }

      return c;
    }
  }

  void lexer::
  unget (const xchar& c)
  {
    // Because iostream::unget cannot work once eos is reached,
    // we have to provide our own implementation.
    //
    buf_ = c;
    unget_ = true;
  }
}
