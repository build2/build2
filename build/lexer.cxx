// file      : build/lexer.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/lexer>

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
      return token (token_type::eos, ln, cn);

    switch (c)
    {
      // NOTE: remember to update name() if adding new punctuations.
      //
    case '\n':
      {
        // Restore the normal mode at the end of the line.
        //
        if (mode_ == mode::value)
          mode_ = mode::normal;

        return token (token_type::newline, ln, cn);
      }
    case '{':
      {
        return token (token_type::lcbrace, ln, cn);
      }
    case '}':
      {
        return token (token_type::rcbrace, ln, cn);
      }
    }

    // The following characters are not treated as special in the
    // value mode.
    //
    if (mode_ != mode::value)
    {
      // NOTE: remember to update name() if adding new punctuations.
      //
      switch (c)
      {
      case ':':
        {
          return token (token_type::colon, ln, cn);
        }
      case '=':
        {
          mode_ = mode::value;
          return token (token_type::equal, ln, cn);
        }
      case '+':
        {
          if (get () != '=')
            fail (c) << "expected = after +";

          mode_ = mode::value;
          return token (token_type::plus_equal, ln, cn);
        }
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

    if (is_eos (c))
      fail (c) << "unterminated escape sequence";

    return c;
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
      bool done (false);

      // The following characters are not treated as special in the
      // value mode.
      //
      if (mode_ != mode::value)
      {
        switch (c)
        {
        case ':':
        case '=':
        case '+':
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

  location_prologue lexer::fail_mark_base::
  operator() (const xchar& c) const
  {
    return build::fail_mark_base<failed>::operator() (
      location (name_.c_str (), c.line (), c.column ()));
  }
}
