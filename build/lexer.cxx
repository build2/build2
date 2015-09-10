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
    token t (next_impl ());
    if (processor_ != nullptr)
      processor_ (t, *this);
    return t;
  }

  token lexer::
  next_impl ()
  {
    lexer_mode m (mode_.top ());

    // For some modes we have dedicated imlementations of next().
    //
    switch (m)
    {
    case lexer_mode::eval: return next_eval ();
    case lexer_mode::quoted: return next_quoted ();
    default: break;
    }

    bool sep (skip_spaces ());

    xchar c (get ());
    uint64_t ln (c.line), cn (c.column);

    if (eos (c))
      return token (token_type::eos, sep, ln, cn);

    switch (c)
    {
      // NOTE: remember to update name(), next_eval() if adding new
      // special characters.
      //
    case '\n':
      {
        // Expire value/pairs mode at the end of the line.
        //
        if (m == lexer_mode::value || m == lexer_mode::pairs)
          mode_.pop ();

        return token (token_type::newline, sep, ln, cn);
      }
    case '{': return token (token_type::lcbrace, sep, ln, cn);
    case '}': return token (token_type::rcbrace, sep, ln, cn);
    case '$': return token (token_type::dollar, sep, ln, cn);
    case '(': return token (token_type::lparen, sep, ln, cn);
    case ')': return token (token_type::rparen, sep, ln, cn);
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
      switch (c)
      {
        // NOTE: remember to update name(), next_eval() if adding new
        // special characters.
        //
      case ':': return token (token_type::colon, sep, ln, cn);
      case '=': return token (token_type::equal, sep, ln, cn);
      case '+':
        {
          if (get () != '=')
            fail (c) << "expected = after +";

          return token (token_type::plus_equal, sep, ln, cn);
        }
      }
    }

    // Otherwise it is a name.
    //
    unget (c);
    return name (sep);
  }

  token lexer::
  next_eval ()
  {
    bool sep (skip_spaces ());
    xchar c (get ());

    if (eos (c))
      fail (c) << "unterminated evaluation context";

    uint64_t ln (c.line), cn (c.column);

    // This mode is quite a bit like the value mode when it comes
    // to special characters.
    //
    switch (c)
    {
      // NOTE: remember to update name() if adding new special characters.
      //
    case '\n': fail (c) << "newline in evaluation context";
    case '{': return token (token_type::lcbrace, sep, ln, cn);
    case '}': return token (token_type::rcbrace, sep, ln, cn);
    case '$': return token (token_type::dollar, sep, ln, cn);
    case '(': return token (token_type::lparen, sep, ln, cn);
    case ')':
      {
        mode_.pop (); // Expire eval mode.
        return token (token_type::rparen, sep, ln, cn);
      }
    }

    // Otherwise it is a name.
    //
    unget (c);
    return name (sep);
  }

  token lexer::
  next_quoted ()
  {
    xchar c (get ());

    if (eos (c))
      fail (c) << "unterminated double-quoted sequence";

    uint64_t ln (c.line), cn (c.column);

    switch (c)
    {
    case '$': return token (token_type::dollar, false, ln, cn);
    case '(': return token (token_type::lparen, false, ln, cn);
    }

    // Otherwise it is a name.
    //
    unget (c);
    return name (false);
  }

  token lexer::
  name (bool sep)
  {
    xchar c (peek ());
    assert (!eos (c));

    uint64_t ln (c.line), cn (c.column);
    string lexeme;

    lexer_mode m (mode_.top ());
    bool quoted (m == lexer_mode::quoted);

    for (; !eos (c); c = peek ())
    {
      bool done (false);

      // Handle pair separator.
      //
      if (m == lexer_mode::pairs && c == pair_separator_)
        break;

      // The following characters are not treated as special in the
      // value/pairs, eval, and quoted modes.
      //
      if (m != lexer_mode::value &&
          m != lexer_mode::pairs &&
          m != lexer_mode::eval  &&
          m != lexer_mode::quoted)
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

      // If we are quoted, these are ordinary characters.
      //
      if (m != lexer_mode::quoted)
      {
        switch (c)
        {
        case ' ':
        case '\t':
        case '\n':
        case '#':
        case '{':
        case '}':
        case ')':
          {
            done = true;
            break;
          }
        case '\\':
          {
            get ();
            c = escape ();
            if (c != '\n') // Ignore.
              lexeme += c;
            continue;
          }
        case '\'':
          {
            // If we are in the variable mode, then treat quote as just
            // another separator.
            //
            if (m == lexer_mode::variable)
            {
              done = true;
              break;
            }
            else
            {
              get ();

              for (c = get (); !eos (c) && c != '\''; c = get ())
                lexeme += c;

              if (eos (c))
                fail (c) << "unterminated single-quoted sequence";

              quoted = true;
              continue;
            }
          }
        }

        if (done)
          break;
      }

      switch (c)
      {
      case '$':
      case '(':
        {
          done = true;
          break;
        }
      case '\"':
        {
          // If we are in the variable mode, then treat quote as just
          // another separator.
          //
          if (m == lexer_mode::variable)
          {
            done = true;
            break;
          }
          else
          {
            get ();

            if (m == lexer_mode::quoted)
              mode_.pop ();
            else
            {
              mode_.push (lexer_mode::quoted);
              quoted = true;
            }

            m = mode_.top ();
            continue;
          }
        }
      default:
        {
          get ();
          lexeme += c;
          continue;
        }
      }

      assert (done);
      break;
    }

    if (m == lexer_mode::quoted && eos (c))
      fail (c) << "unterminated double-quoted sequence";

    // Expire variable mode at the end of the name.
    //
    if (m == lexer_mode::variable)
      mode_.pop ();

    return token (lexeme, sep, quoted, ln, cn);
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
            break; // Ignore.

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
