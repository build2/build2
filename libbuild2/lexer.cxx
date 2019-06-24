// file      : libbuild2/lexer.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/lexer.hxx>

#include <cstring> // strchr()

using namespace std;

namespace build2
{
  using type = token_type;

  pair<char, bool> lexer::
  peek_char ()
  {
    sep_ = skip_spaces ();
    xchar c (peek ());
    return make_pair (eos (c) ? '\0' : char (c), sep_);
  }

  void lexer::
  mode (lexer_mode m, char ps, optional<const char*> esc)
  {
    const char* s1 (nullptr);
    const char* s2 (nullptr);
    bool s (true);
    bool n (true);
    bool q (true);

    if (!esc)
    {
      assert (!state_.empty ());
      esc = state_.top ().escapes;
    }

    switch (m)
    {
    case lexer_mode::normal:
      {
        s1 = ":<>=+ $(){}[]#\t\n";
        s2 = "    =           ";
        break;
      }
    case lexer_mode::value:
      {
        s1 = " $(){}[]#\t\n";
        s2 = "           ";
        break;
      }
    case lexer_mode::attribute:
      {
        s1 = " $(]#\t\n";
        s2 = "       ";
        break;
      }
    case lexer_mode::eval:
      {
        s1 = ":<>=!&|?, $(){}[]#\t\n";
        s2 = "   = &|             ";
        break;
      }
    case lexer_mode::buildspec:
      {
        // Like the value mode with these differences:
        //
        // 1. Returns '(' as a separated token provided the state stack depth
        //    is less than or equal to 3 (initial state plus two buildspec)
        //    (see parse_buildspec() for details).
        //
        // 2. Recognizes comma.
        //
        // 3. Treat newline as an ordinary space.
        //
        s1 = " $(){}[],\t\n";
        s2 = "           ";
        n = false;
        break;
      }
    case lexer_mode::single_quoted:
    case lexer_mode::double_quoted:
      s = false;
      // Fall through.
    case lexer_mode::variable:
      {
        // These are handled in an ad hoc way in word().
        assert (ps == '\0');
        break;
      }
    default: assert (false); // Unhandled custom mode.
    }

    state_.push (state {m, ps, s, n, q, *esc, s1, s2});
  }

  token lexer::
  next ()
  {
    const state& st (state_.top ());
    lexer_mode m (st.mode);

    // For some modes we have dedicated imlementations of next().
    //
    switch (m)
    {
    case lexer_mode::normal:
    case lexer_mode::value:
    case lexer_mode::attribute:
    case lexer_mode::variable:
    case lexer_mode::buildspec:     break;
    case lexer_mode::eval:          return next_eval ();
    case lexer_mode::double_quoted: return next_quoted ();
    default:                        assert (false); // Unhandled custom mode.
    }

    bool sep (skip_spaces ());

    xchar c (get ());
    uint64_t ln (c.line), cn (c.column);

    auto make_token = [&sep, ln, cn] (type t, string v = string ())
    {
      return token (t, move (v),
                    sep, quote_type::unquoted, false,
                    ln, cn, token_printer);
    };

    if (eos (c))
      return make_token (type::eos);

    // Handle pair separator.
    //
    if (c == st.sep_pair)
      return make_token (type::pair_separator, string (1, c));

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

        sep = true; // Treat newline as always separated.
        return make_token (type::newline);
      }
    case '{': return make_token (type::lcbrace);
    case '}': return make_token (type::rcbrace);
    case '[': return make_token (type::lsbrace);
    case ']':
      {
        // Expire attribute mode after closing ']'.
        //
        if (m == lexer_mode::attribute)
          state_.pop ();

        return make_token (type::rsbrace);
      }
    case '$': return make_token (type::dollar);
    case ')': return make_token (type::rparen);
    case '(':
      {
        // Left paren is always separated in the buildspec mode.
        //
        if (m == lexer_mode::buildspec && state_.size () <= 3)
          sep = true;

        return make_token (type::lparen);
      }
    }

    // The following characters are special in the normal and variable modes.
    //
    if (m == lexer_mode::normal || m == lexer_mode::variable)
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

    // The following characters are special in the normal mode.
    //
    if (m == lexer_mode::normal)
    {
      // NOTE: remember to update mode() if adding new special characters.
      //
      switch (c)
      {
      case '<': return make_token (type::labrace);
      case '>': return make_token (type::rabrace);
      }
    }

    // The following characters are special in the buildspec mode.
    //
    if (m == lexer_mode::buildspec)
    {
      // NOTE: remember to update mode() if adding new special characters.
      //
      switch (c)
      {
      case ',': return make_token (type::comma);
      }
    }

    // Otherwise it is a word.
    //
    unget (c);
    return word (st, sep);
  }

  token lexer::
  next_eval ()
  {
    bool sep (skip_spaces ());
    xchar c (get ());

    if (eos (c))
      fail (c) << "unterminated evaluation context";

    const state& st (state_.top ());

    uint64_t ln (c.line), cn (c.column);

    auto make_token = [sep, ln, cn] (type t, string v = string ())
    {
      return token (t, move (v),
                    sep, quote_type::unquoted, false,
                    ln, cn, token_printer);
    };

    // This mode is quite a bit like the value mode when it comes to special
    // characters, except that we have some of our own.
    //

    // Handle pair separator.
    //
    if (c == st.sep_pair)
      return make_token (type::pair_separator, string (1, c));

    // Note: we don't treat [ and ] as special here. Maybe can use them for
    // something later.
    //
    switch (c)
    {
      // NOTE: remember to update mode() if adding new special characters.
      //
    case '\n': fail (c) << "newline in evaluation context" << endf;
    case ':': return make_token (type::colon);
    case '{': return make_token (type::lcbrace);
    case '}': return make_token (type::rcbrace);
    case '[': return make_token (type::lsbrace);
    case ']': return make_token (type::rsbrace);
    case '$': return make_token (type::dollar);
    case '?': return make_token (type::question);
    case ',': return make_token (type::comma);
    case '(': return make_token (type::lparen);
    case ')':
      {
        state_.pop (); // Expire eval mode.
        return make_token (type::rparen);
      }
      // Potentially two-character tokens.
      //
    case '=':
    case '!':
    case '<':
    case '>':
    case '|':
    case '&':
      {
        xchar p (peek ());

        type r (type::eos);
        switch (c)
        {
        case '|': if (p == '|') r = type::log_or; break;
        case '&': if (p == '&') r = type::log_and; break;

        case '<': r = (p == '=' ? type::less_equal : type::less); break;
        case '>': r = (p == '=' ? type::greater_equal : type::greater); break;

        case '=': if (p == '=') r = type::equal; break;

        case '!': r = (p == '=' ? type::not_equal : type::log_not); break;
        }

        if (r == type::eos)
          break;

        switch (r)
        {
        case type::less:
        case type::greater:
        case type::log_not: break;
        default:            get ();
        }

        return make_token (r);
      }
    }

    // Otherwise it is a word.
    //
    unget (c);
    return word (st, sep);
  }

  token lexer::
  next_quoted ()
  {
    xchar c (get ());

    if (eos (c))
      fail (c) << "unterminated double-quoted sequence";

    uint64_t ln (c.line), cn (c.column);

    auto make_token = [ln, cn] (type t)
    {
      return token (t, false, quote_type::double_, ln, cn, token_printer);
    };

    switch (c)
    {
    case '$': return make_token (type::dollar);
    case '(': return make_token (type::lparen);
    }

    // Otherwise it is a word.
    //
    unget (c);
    return word (state_.top (), false);
  }

  token lexer::
  word (state st, bool sep)
  {
    lexer_mode m (st.mode);

    xchar c (peek ());
    assert (!eos (c));

    uint64_t ln (c.line), cn (c.column);

    string lexeme;
    quote_type qtype (m == lexer_mode::double_quoted
                      ? quote_type::double_
                      : quote_type::unquoted);

    // If we are already in the quoted mode then we didn't start with the
    // quote character.
    //
    bool qcomp (false);

    auto append = [&lexeme, &m, &qcomp] (char c)
    {
      lexeme += c;

      // An unquoted character after a quoted fragment.
      //
      if (qcomp && m != lexer_mode::double_quoted)
        qcomp = false;
    };

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

        const char* esc (st.escapes);

        if (esc == nullptr ||
            (*esc != '\0' && !eos (p) && strchr (esc, p) != nullptr))
        {
          get ();

          if (eos (p))
            fail (p) << "unterminated escape sequence";

          if (p != '\n') // Ignore if line continuation.
            append (p);

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

            st = state_.top ();
            m = st.mode;
            continue;
          }
        }
      }
      // We also handle the variable mode in an ad hoc way.
      //
      else if (m == lexer_mode::variable)
      {
        if (c != '_' && !(lexeme.empty () ? alpha (c) : alnum (c)))
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
            done = eos (p) || !(alpha (p) ||  p == '_');
            unget (c);
          }
        }
      }
      else
      {
        // First check if it's a pair separator.
        //
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

        // Handle single and double quotes if enabled for this mode and unless
        // they were considered separators.
        //
        if (st.quotes && !done)
        {
          switch (c)
          {
          case '\'':
            {
              // Enter the single-quoted mode in case the derived lexer needs
              // to notice this.
              //
              mode (lexer_mode::single_quoted);

              switch (qtype)
              {
              case quote_type::unquoted:
                qtype = quote_type::single;
                qcomp = lexeme.empty ();
                break;
              case quote_type::single:
                qcomp = false; // Non-contiguous.
                break;
              case quote_type::double_:
                qtype = quote_type::mixed;
                // Fall through.
              case quote_type::mixed:
                qcomp = false;
                break;
              }

              get ();
              for (c = get (); !eos (c) && c != '\''; c = get ())
                lexeme += c;

              if (eos (c))
                fail (c) << "unterminated single-quoted sequence";

              state_.pop ();
              continue;
            }
          case '\"':
            {
              get ();

              mode (lexer_mode::double_quoted);
              st = state_.top ();
              m = st.mode;

              switch (qtype)
              {
              case quote_type::unquoted:
                qtype = quote_type::double_;
                qcomp = lexeme.empty ();
                break;
              case quote_type::double_:
                qcomp = false; // Non-contiguous.
                break;
              case quote_type::single:
                qtype = quote_type::mixed;
                // Fall through.
              case quote_type::mixed:
                qcomp = false;
                break;
              }

              continue;
            }
          }
        }
      }

      if (done)
        break;

      get ();
      append (c);
    }

    if (m == lexer_mode::double_quoted)
    {
      if (eos (c))
        fail (c) << "unterminated double-quoted sequence";

      // If we are still in the quoted mode then we didn't end with the quote
      // character.
      //
      if (qcomp)
        qcomp = false;
    }

    // Expire variable mode at the end of the word.
    //
    if (m == lexer_mode::variable)
      state_.pop ();

    return token (move (lexeme), sep, qtype, qcomp, ln, cn);
  }

  bool lexer::
  skip_spaces ()
  {
    bool r (sep_);
    sep_ = false;

    const state& s (state_.top ());

    // In some special modes we don't skip spaces.
    //
    if (!s.sep_space)
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
          // In some modes we treat newlines as ordinary spaces.
          //
          if (!s.sep_newline)
          {
            r = true;
            break;
          }

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
          r = true;
          get ();

          // See if this is a multi-line comment in the form:
          //
          /*
             #\
             ...
             #\
          */
          auto ml = [&c, this] () -> bool
          {
            if ((c = peek ()) == '\\')
            {
              get ();
              if ((c = peek ()) == '\n')
                return true;
            }

            return false;
          };

          if (ml ())
          {
            // Scan until we see the closing one.
            //
            for (; !eos (c); c = peek ())
            {
              get ();
              if (c == '#' && ml ())
                break;
            }

            if (eos (c))
              fail (c) << "unterminated multi-line comment";
          }
          else
          {
            // Read until newline or eos.
            //
            for (; !eos (c) && c != '\n'; c = peek ())
              get ();
          }

          continue;
        }
      case '\\':
        {
          get ();

          if (peek () == '\n')
            break; // Ignore.

          unget (c);
        }
        // Fall through.
      default:
        return r; // Not a space.
      }

      get ();
    }

    return r;
  }
}
