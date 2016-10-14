// file      : build2/lexer.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/lexer>

#include <cstring> // strchr()

using namespace std;

namespace build2
{
  using type = token_type;

  token lexer::
  next ()
  {
    token t (next_impl ());
    if (processor_ != nullptr)
      processor_ (t, *this);
    return t;
  }

  pair<char, bool> lexer::
  peek_char ()
  {
    sep_ = skip_spaces ();
    xchar c (peek ());
    return make_pair (eos (c) ? '\0' : char (c), sep_);
  }

  void lexer::
  mode (lexer_mode m, char ps)
  {
    const char* s1 (nullptr);
    const char* s2 (nullptr);
    char p ('\0');
    bool s (true);

    switch (m)
    {
    case lexer_mode::normal:
      {
        s1 = ":=+ $(){}[]#\t\n";
        s2 = "  =           ";
        p = ps;
        break;
      }
    case lexer_mode::value:
      {
        s1 = " $(){}[]#\t\n";
        s2 = "           ";
        p = ps;
        break;
      }
    case lexer_mode::eval:
      {
        s1 = ":<>=! $(){}[]#\t\n";
        s2 = "   ==           ";
        p = ps;
        break;
      }
    case lexer_mode::single_quoted:
    case lexer_mode::double_quoted:
      s = false;
      // Fall through.
    case lexer_mode::variable:
      {
        // These are handled in an ad hoc way in name().
        break;
      }
    default: assert (false); // Unhandled custom mode.
    }

    state_.push (state {m, p, s, s1, s2});
  }

  token lexer::
  next_impl ()
  {
    lexer_mode m (state_.top ().mode);

    // For some modes we have dedicated imlementations of next().
    //
    switch (m)
    {
    case lexer_mode::normal:
    case lexer_mode::variable:
    case lexer_mode::value: break;
    case lexer_mode::eval: return next_eval ();
    case lexer_mode::double_quoted: return next_quoted ();
    default: assert (false); // Unhandled custom mode.
    }

    bool sep (skip_spaces ());

    xchar c (get ());
    uint64_t ln (c.line), cn (c.column);

    auto make_token = [sep, ln, cn] (type t)
    {
      return token (t, sep, ln, cn, token_printer);
    };

    if (eos (c))
      return make_token (type::eos);

    // Handle pair separator.
    //
    if ((m == lexer_mode::normal || m == lexer_mode::value) &&
        c == state_.top ().sep_pair)
      return make_token (type::pair_separator);

    switch (c)
    {
      // NOTE: remember to update mode(), next_eval() if adding new special
      // characters.
      //
    case '\n':
      {
        // Expire value mode at the end of the line.
        //
        if (m == lexer_mode::value)
          state_.pop ();

        return make_token (type::newline);
      }
    case '{': return make_token (type::lcbrace);
    case '}': return make_token (type::rcbrace);
    case '[': return make_token (type::lsbrace);
    case ']': return make_token (type::rsbrace);
    case '$': return make_token (type::dollar);
    case '(': return make_token (type::lparen);
    case ')': return make_token (type::rparen);
    }

    // The following characters are not treated as special in the value mode.
    //
    if (m != lexer_mode::value)
    {
      switch (c)
      {
        // NOTE: remember to update mode(), next_eval() if adding new special
        // characters.
        //
      case ':': return make_token (type::colon);
      case '=':
        {
          if (peek () == '+')
          {
            get ();
            return make_token (type::prepend);
          }
          else
            return make_token (type::assign);
        }
      case '+':
        {
          if (peek () == '=')
          {
            get ();
            return make_token (type::append);
          }
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

    auto make_token = [sep, ln, cn] (type t)
    {
      return token (t, sep, ln, cn, token_printer);
    };

    // This mode is quite a bit like the value mode when it comes to special
    // characters, except that we have some of our own.
    //

    // Handle pair separator.
    //
    if (c == state_.top ().sep_pair)
      return make_token (type::pair_separator);

    // Note: we don't treat [ and ] as special here. Maybe can use them for
    // something later.
    //
    switch (c)
    {
      // NOTE: remember to update mode() if adding new special characters.
      //
    case '\n': fail (c) << "newline in evaluation context";
    case ':': return make_token (type::colon);
    case '{': return make_token (type::lcbrace);
    case '}': return make_token (type::rcbrace);
    case '[': return make_token (type::lsbrace);
    case ']': return make_token (type::rsbrace);
    case '$': return make_token (type::dollar);
    case '(': return make_token (type::lparen);
    case ')':
      {
        state_.pop (); // Expire eval mode.
        return make_token (type::rparen);
      }
    case '=':
    case '!':
      {
        if (peek () == '=')
        {
          get ();
          return make_token (c == '=' ? type::equal : type::not_equal);
        }
        break;
      }
    case '<':
    case '>':
      {
        bool e (peek () == '=');
        if (e)
          get ();

        return make_token (c == '<'
                           ? e ? type::less_equal : type::less
                           : e ? type::greater_equal : type::greater);
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
    case '$': return token (type::dollar, false, ln, cn, token_printer);
    case '(': return token (type::lparen, false, ln, cn, token_printer);
    }

    // Otherwise it is a name.
    //
    unget (c);
    return name (false);
  }

  token lexer::
  name (bool sep)
  {
    lexer_mode m (state_.top ().mode);

    xchar c (peek ());
    assert (!eos (c));

    uint64_t ln (c.line), cn (c.column);

    string lexeme;
    bool quoted (m == lexer_mode::double_quoted);

    for (; !eos (c); c = peek ())
    {
      // First handle escape sequences.
      //
      if (c == '\\')
      {
        // In the variable mode we treat the beginning of the escape sequence
        // as a separator (think \"$foo\").
        //
        if (m == lexer_mode::variable)
          break;

        get ();
        xchar p (peek ());

        if (escapes_ == nullptr ||
            (!eos (p) && strchr (escapes_, p) != nullptr))
        {
          get ();

          if (eos (p))
            fail (p) << "unterminated escape sequence";

          if (p != '\n') // Ignore if line continuation.
            lexeme += p;

          continue;
        }
        else
          unget (c); // Treat as a normal character.
      }

      bool done (false);

      // Next take care of the double-quoted mode. This one is tricky since
      // we push/pop modes while accumulating the same lexeme for example:
      //
      // foo" bar "baz
      //
      if (m == lexer_mode::double_quoted)
      {
        switch (c)
        {
          // Only these two characters are special in the double-quoted mode.
          //
        case '$':
        case '(':
          {
            done = true;
            break;
          }
          // End quote.
          //
        case '\"':
          {
            get ();
            state_.pop ();
            m = state_.top ().mode;
            continue;
          }
        }
      }
      // We also handle the variable mode in an ad hoc way.
      //
      else if (m == lexer_mode::variable)
      {
        if (!alnum (c) &&  c != '_')
        {
          if (c != '.')
            done = true;
          else
          {
            // Normally '.' is part of the variable (namespace separator)
            // unless it is trailing (think $major.$minor).
            //
            get ();
            xchar p (peek ());
            done = eos (p) || !(alnum (p) ||  p == '_');
            unget (c);
          }
        }
      }
      else
      {
        // First check if it's a pair separator.
        //
        const state& st (state_.top ());
        if (c == st.sep_pair)
          done = true;
        else
        {
          // Then see if this character or character sequence is a separator.
          //
          for (const char* p (strchr (st.sep_first, c));
               p != nullptr;
               p = done ? nullptr : strchr (p + 1, c))
          {
            char s (st.sep_second[p - st.sep_first]);

            // See if it has a second.
            //
            if (s != ' ')
            {
              get ();
              done = (peek () == s);
              unget (c);
            }
            else
              done = true;
          }
        }

        // Handle single and double quotes unless they were considered
        // separators.
        //
        if (!done)
        {
          switch (c)
          {
          case '\'':
            {
              // Enter the single-quoted mode in case the derived lexer needs
              // to notice this.
              //
              mode (lexer_mode::single_quoted);

              get ();
              for (c = get (); !eos (c) && c != '\''; c = get ())
                lexeme += c;

              if (eos (c))
                fail (c) << "unterminated single-quoted sequence";

              state_.pop ();

              quoted = true;
              continue;
            }
          case '\"':
            {
              get ();
              mode ((m = lexer_mode::double_quoted));
              quoted = true;
              continue;
            }
          }
        }
      }

      if (done)
        break;

      get ();
      lexeme += c;
    }

    if (eos (c) && m == lexer_mode::double_quoted)
      fail (c) << "unterminated double-quoted sequence";

    // Expire variable mode at the end of the name.
    //
    if (m == lexer_mode::variable)
      state_.pop ();

    return token (move (lexeme), sep, quoted, ln, cn, token_printer);

  }

  bool lexer::
  skip_spaces ()
  {
    bool r (sep_);
    sep_ = false;

    // In some modes we don't skip spaces.
    //
    if (!state_.top ().sep_space)
      return r;

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

  location_prologue lexer::fail_mark_base::
  operator() (const xchar& c) const
  {
    return build2::fail_mark_base<failed>::operator() (
      location (&name_, c.line, c.column));
  }
}
