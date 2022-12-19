// file      : libbuild2/lexer.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/lexer.hxx>

#include <cstring> // strchr()

using namespace std;

namespace build2
{
  using type = token_type;

  [[noreturn]] void lexer::
  fail_char (const xchar& c)
  {
    fail (c) << ebuf_ << endf;
  }

  pair<pair<char, char>, bool> lexer::
  peek_chars ()
  {
    auto p (skip_spaces ());
    assert (!p.second);
    sep_ = p.first;

    char r[2] = {'\0', '\0'};

    xchar c0 (peek ());
    if (!eos (c0))
    {
      get (c0);
      r[0] = c0;

      xchar c1 (peek ());
      if (!eos (c1))
        r[1] = c1;

      unget (c0);
    }

    return make_pair (make_pair (r[0], r[1]), sep_);
  }

  pair<char, bool> lexer::
  peek_char ()
  {
    auto p (skip_spaces ());
    assert (!p.second);
    sep_ = p.first;

    char r ('\0');

    xchar c (peek ());
    if (!eos (c))
      r = c;

    return make_pair (r, sep_);
  }

  void lexer::
  mode (lexer_mode m, char ps, optional<const char*> esc, uintptr_t data)
  {
    bool lsb (false); // Enable `[` recognition.

    const char* s1 (nullptr);
    const char* s2 (nullptr);

    bool s (true); // space
    bool n (true); // newline
    bool q (true); // quotes

    if (!esc)
    {
      assert (!state_.empty ());
      esc = state_.top ().escapes;
    }

    switch (m)
    {
    case lexer_mode::normal:
    case lexer_mode::cmdvar:
      {
        // Note: `%` is only recognized at the beginning of the line so it
        // should not be included here.
        //
        s1 = ":<>=+? $(){}#\t\n";
        s2 = "    ==         ";
        lsb  = true;
        break;
      }
    case lexer_mode::value:
      {
        s1 = " $(){}#\t\n";
        s2 = "         ";
        break;
      }
    case lexer_mode::values:
      {
        s1 = " $(){},#\t\n";
        s2 = "          ";
        break;
      }
    case lexer_mode::switch_expressions:
      {
        s1 = " $(){},:#\t\n";
        s2 = "           ";
        break;
      }
    case lexer_mode::case_patterns:
      {
        s1 = " $(){},|:#\t\n";
        s2 = "            ";
        break;
      }
    case lexer_mode::attributes:
      {
        s1 = " $()=,]#\t\n";
        s2 = "          ";
        break;
      }
    case lexer_mode::attribute_value:
      {
        s1 = " $(),]#\t\n";
        s2 = "         ";
        break;
      }
    case lexer_mode::subscript:
      {
        s1 = " $()]#\t\n";
        s2 = "        ";
        break;
      }
    case lexer_mode::eval:
      {
        // NOTE: remember to update special() lambda in parse_names() if
        //       adding any new single-character tokens to the eval mode.
        //
        s1 = ":<>=!&|?,` $(){}#\t\n";
        s2 = "   = &             ";
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
        // Note that because we use this mode for both the command line
        // buildspec and ad hoc recipe actions, we control the recognition of
        // newlines as tokens via the auxiliary data.
        //
        s1 = " $(){},\t\n";
        s2 = "         ";
        n = (data != 0);
        break;
      }
    case lexer_mode::foreign:
      {
        assert (ps == '\0' && data > 1);
        s = false;
        break;
      }
    case lexer_mode::single_quoted:
    case lexer_mode::double_quoted:
      {
        assert (false); // Can only be set manually in word().
        break;
      }
    case lexer_mode::variable:
      {
        // These are handled in an ad hoc way in word().
        assert (ps == '\0');
        break;
      }
    default: assert (false); // Unhandled custom mode.
    }

    mode_impl (state {m, data, nullopt, lsb, false, ps, s, n, q, *esc, s1, s2});
  }

  void lexer::
  mode_impl (state&& s)
  {
    // If we are in the double-quoted mode then, unless the new mode is eval
    // or variable, delay the state switch until the current mode is expired.
    // Note that we delay by injecting the new state beneath the current
    // state.
    //
    if (!state_.empty ()                                &&
        state_.top ().mode == lexer_mode::double_quoted &&
        s.mode != lexer_mode::eval                      &&
        s.mode != lexer_mode::variable)
    {
      state qs (move (state_.top ())); // Save quoted state.
      state_.top () = move (s);        // Overwrite quoted state with new state.
      state_.push (move (qs));         // Restore quoted state.
    }
    else
      state_.push (move (s));
  }

  void lexer::
  expire_mode ()
  {
    // If we are in the double-quoted mode, then delay the state expiration
    // until the current mode is expired. Note that we delay by overwriting
    // the being expired state with the current state.
    //
    assert (!state_.empty () &&
            (state_.top ().mode != lexer_mode::double_quoted ||
             state_.size () > 1));

    if (state_.top ().mode == lexer_mode::double_quoted)
    {
      state qs (move (state_.top ())); // Save quoted state.
      state_.pop ();                   // Pop quoted state.
      state_.top () = move (qs);       // Expire state, restoring quoted state.
    }
    else
      state_.pop ();
  }

  token lexer::
  next ()
  {
    state& st (state_.top ());
    lexer_mode m (st.mode);

    // For some modes we have dedicated imlementations of next().
    //
    switch (m)
    {
    case lexer_mode::normal:
    case lexer_mode::cmdvar:
    case lexer_mode::value:
    case lexer_mode::values:
    case lexer_mode::switch_expressions:
    case lexer_mode::case_patterns:
    case lexer_mode::attributes:
    case lexer_mode::attribute_value:
    case lexer_mode::subscript:
    case lexer_mode::variable:
    case lexer_mode::buildspec:     break;
    case lexer_mode::eval:          return next_eval ();
    case lexer_mode::double_quoted: return next_quoted ();
    case lexer_mode::foreign:       return next_foreign ();
    default:                        assert (false); // Unhandled custom mode.
    }

    pair<bool, bool> skip (skip_spaces ());
    bool sep (skip.first);    // Separated from a previous character.
    bool first (skip.second); // First non-whitespace character of a line.

    xchar c (get ());
    uint64_t ln (c.line), cn (c.column);

    auto make_token = [&sep, ln, cn] (type t, string v = string ())
    {
      return token (t, move (v), sep,
                    quote_type::unquoted, false, false,
                    ln, cn,
                    token_printer);
    };

    // Handle `[` (do it first to make sure the flag is cleared regardless of
    // what we return).
    //
    if (st.lsbrace)
    {
      st.lsbrace = false;

      if (c == '[' && (!st.lsbrace_unsep || !sep))
        return make_token (type::lsbrace);
    }

    if (eos (c))
      return make_token (type::eos);

    // Handle pair separator.
    //
    if (c == st.sep_pair)
      return make_token (type::pair_separator, string (1, c));

    // NOTE: remember to update mode(), next_eval() if adding any new special
    // characters.

    // These are special in all the modes handled by this function.
    //
    switch (c)
    {
    case '\n':
      {
        // Expire value/values modes at the end of the line.
        //
        if (m == lexer_mode::value              ||
            m == lexer_mode::values             ||
            m == lexer_mode::switch_expressions ||
            m == lexer_mode::case_patterns)
          state_.pop ();

        // Re-enable `[` recognition (attributes) in the normal mode (should
        // never be needed in cmdvar).
        //
        state& st (state_.top ());
        if (st.mode == lexer_mode::normal)
        {
          st.lsbrace = true;
          st.lsbrace_unsep = false;
        }

        sep = true; // Treat newline as always separated.
        return make_token (type::newline);
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

    // Line-leading tokens in the normal mode.
    //
    // Note: must come before any other (e.g., `{`) tests below.
    //
    if (m == lexer_mode::normal && first)
    {
      switch (c)
      {
      case '%': return make_token (type::percent);
      case '{':
        {
          string v;
          while (peek () == '{')
            v += get ();

          if (!v.empty ())
          {
            v += '{';
            return make_token (type::multi_lcbrace, move (v));
          }

          break;
        }
      }
    }

    // The following characters are special in all modes except attributes
    // and subscript.
    //
    if (m != lexer_mode::attributes      &&
        m != lexer_mode::attribute_value &&
        m != lexer_mode::subscript)
    {
      switch (c)
      {
      case '{': return make_token (type::lcbrace);
      case '}': return make_token (type::rcbrace);
      }
    }

    // The following characters are special in the attributes modes.
    //
    if (m == lexer_mode::attributes)
    {
      switch (c)
      {
      case '=': return make_token (type::assign);
      }
    }

    if (m == lexer_mode::attributes      ||
        m == lexer_mode::attribute_value ||
        m == lexer_mode::subscript)
    {
      switch (c)
      {
      case ']':
        {
          state_.pop (); // Expire the mode after closing `]`.
          return make_token (type::rsbrace);
        }
      }
    }

    // The following characters are special in the normal and
    // switch_expressions modes.
    //
    if (m == lexer_mode::normal             ||
        m == lexer_mode::cmdvar             ||
        m == lexer_mode::switch_expressions ||
        m == lexer_mode::case_patterns)
    {
      switch (c)
      {
      case ':': return make_token (type::colon);
      }
    }

    // The following characters are special in the normal mode.
    //
    if (m == lexer_mode::normal ||
        m == lexer_mode::cmdvar)
    {
      switch (c)
      {
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
          break;
        }
      case '?':
        {
          if (peek () == '=')
          {
            get ();
            return make_token (type::default_assign);
          }
          break;
        }
      }
    }

    // The following characters are special in the normal mode.
    //
    if (m == lexer_mode::normal ||
        m == lexer_mode::cmdvar)
    {
      switch (c)
      {
      case '<': return make_token (type::labrace);
      case '>': return make_token (type::rabrace);
      }
    }

    // The following characters are special in the values and alike modes.
    //
    if (m == lexer_mode::buildspec          ||
        m == lexer_mode::values             ||
        m == lexer_mode::switch_expressions ||
        m == lexer_mode::case_patterns      ||
        m == lexer_mode::attributes         ||
        m == lexer_mode::attribute_value)
    {
      switch (c)
      {
      case ',': return make_token (type::comma);
      }
    }

    // The following characters are special in the case_patterns mode.
    //
    if (m == lexer_mode::case_patterns)
    {
      switch (c)
      {
      case '|': return make_token (type::bit_or);
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
    // This mode is quite a bit like the value mode when it comes to special
    // characters, except that we have some of our own.

    bool sep (skip_spaces ().first);
    xchar c (get ());

    if (eos (c))
      fail (c) << "unterminated evaluation context";

    state& st (state_.top ());

    uint64_t ln (c.line), cn (c.column);

    auto make_token = [sep, ln, cn] (type t, string v = string ())
    {
      return token (t, move (v), sep,
                    quote_type::unquoted, false, false,
                    ln, cn,
                    token_printer);
    };

    // Handle `[` (do it first to make sure the flag is cleared regardless of
    // what we return).
    //
    if (st.lsbrace)
    {
      st.lsbrace = false;

      if (c == '[' && (!st.lsbrace_unsep || !sep))
        return make_token (type::lsbrace);
    }

    // Handle pair separator.
    //
    if (c == st.sep_pair)
      return make_token (type::pair_separator, string (1, c));

    // NOTE: remember to update mode() if adding any new special characters.

    switch (c)
    {
    case '\n': fail (c) << "newline in evaluation context" << endf;
    case ':': return make_token (type::colon);
    case '{': return make_token (type::lcbrace);
    case '}': return make_token (type::rcbrace);
    case '$': return make_token (type::dollar);
    case '?': return make_token (type::question);
    case ',': return make_token (type::comma);
    case '`': return make_token (type::backtick);
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
        case '|': r = (p == '|' ? type::log_or : type::bit_or); break;
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
        case type::bit_or:
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
  next_foreign ()
  {
    state& st (state_.top ());

    if (st.hold)
    {
      token r (move (*st.hold));
      state_.pop (); // Expire foreign mode.
      return r;
    }

    auto count (state_.top ().data); // Number of closing braces to expect.

    xchar c (get ()); // First character of first line after `{{...`.
    uint64_t ln (c.line), cn (c.column);

    string lexeme;
    for (bool first (true); !eos (c); c = get ())
    {
      // If this is the first character of a line, recognize closing braces.
      //
      if (first)
      {
        first = false;

        // If this turns not to be the closing braces, we need to add any
        // characters we have extracted to lexeme. Instead of saving these
        // characters in a temporary we speculatively add them to the lexeme
        // but then chop them off if this turned out to be the closing braces.
        //
        size_t chop (lexeme.size ());

        // Skip leading whitespaces, if any.
        //
        for (; c == ' ' || c == '\t'; c = get ())
          lexeme += c;

        uint64_t bln (c.line), bcn (c.column); // Position of first `}`.

        // Count braces.
        //
        auto i (count);
        for (; c == '}'; c = get ())
        {
          lexeme += c;

          if (--i == 0)
            break;
        }

        if (i == 0) // Got enough braces.
        {
          // Make sure there are only whitespaces/comments after. Note that
          // now we must start peeking since newline is not "ours".
          //
          for (c = peek (); c == ' ' || c == '\t'; c = peek ())
            lexeme += get ();

          if (c == '\n' || c == '#' || eos (c))
          {
            st.hold = token (type::multi_rcbrace, string (count, '}'), false,
                             quote_type::unquoted, false, false,
                             bln, bcn,
                             token_printer);

            lexeme.resize (chop);
            return token (move (lexeme), false,
                          quote_type::unquoted, false, false,
                          ln, cn);
          }

          get (); // And fall through (not eos).
        }
        else
        {
          if (eos (c))
            break;

          // Fall through.
        }
      }

      if (c == '\n')
        first = true;

      lexeme += c;
    }

    return token (type::eos, false, c.line, c.column, token_printer);
  }

  token lexer::
  word (const state& rst, bool sep)
  {
    lexer_mode m (rst.mode);

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
    bool qfirst (false);

    auto append = [&lexeme, &m, &qcomp, &qfirst] (char c, bool escaped = false)
    {
      if (lexeme.empty () && (escaped || m == lexer_mode::double_quoted))
          qfirst = true;

      // An unquoted character after a quoted fragment.
      //
      if (m != lexer_mode::double_quoted && qcomp)
        qcomp = false;

      lexeme += c;
    };

    const state* st (&rst);
    for (bool first (true); !eos (c); first = false, c = peek ())
    {
      // First handle escape sequences.
      //
      if (c == '\\')
      {
        // In the variable mode we treat immediate `\` as the escape sequence
        // literal and any following as a separator (think \"$foo\").
        //
        if (m == lexer_mode::variable)
        {
          if (!first)
            break;

          get ();
          c = get ();

          if (eos (c))
            fail (c) << "unterminated escape sequence";

          // For now we only support all the simple C/C++ escape sequences
          // plus \0 (which in C/C++ is an octal escape sequence).
          //
          // In the future we may decide to support more elaborate sequences
          // such as \xNN, \uNNNN, etc.
          //
          // Note: we return it in the literal form instead of translating for
          // easier printing.
          //
          switch (c)
          {
          case '\'':
          case '"':
          case '?':
          case '\\':
          case '0':
          case 'a':
          case 'b':
          case 'f':
          case 'n':
          case 'r':
          case 't':
          case 'v': lexeme = c; break;
          default:
            fail (c) << "unknown escape sequence \\" << c;
          }

          state_.pop ();
          return token (type::escape,
                        move (lexeme),
                        sep,
                        qtype, qcomp, qfirst,
                        ln, cn);
        }

        get ();
        xchar p (peek ());

        const char* esc (st->escapes);

        if (esc == nullptr ||
            (*esc != '\0' && !eos (p) && strchr (esc, p) != nullptr))
        {
          get ();

          if (eos (p))
            fail (p) << "unterminated escape sequence";

          if (p != '\n') // Ignore if line continuation.
            append (p, true);

          continue;
        }
        else
          unget (c); // Fall through to treat as a normal character.
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

            st = &state_.top ();
            m = st->mode;
            continue;
          }
        }
      }
      // We also handle the variable mode in an ad hoc way.
      //
      else if (m == lexer_mode::variable)
      {
        // Handle special variable names, if any.
        //
        if (first         &&
            st->data != 0 &&
            strchr (reinterpret_cast<const char*> (st->data), c) != nullptr)
        {
          get ();
          lexeme += c;
          done = true;
        }
        else if (c != '_' && !(lexeme.empty () ? alpha (c) : alnum (c)))
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
        if (c == st->sep_pair)
          done = true;
        else
        {
          // Then see if this character or character sequence is a separator.
          //
          for (const char* p (strchr (st->sep_first, c));
               p != nullptr;
               p = done ? nullptr : strchr (p + 1, c))
          {
            char s (st->sep_second[p - st->sep_first]);

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
        if (st->quotes && !done)
        {
          auto quoted_mode = [this] (lexer_mode m)
          {
            // In the double-quoted mode we only do effective escaping of the
            // special `$("\` characters, line continuations, plus `)` for
            // symmetry. Nothing can be escaped in single-quoted.
            //
            const char* esc (m == lexer_mode::double_quoted ? "$()\"\\\n" : "");

            state_.push (state {
              m, 0, nullopt, false, false, '\0', false, true, true,
              esc, nullptr, nullptr});
          };

          switch (c)
          {
          case '\'':
            {
              // Enter the single-quoted mode in case the derived lexer needs
              // to notice this.
              //
              quoted_mode (lexer_mode::single_quoted);

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

              // Note that we will treat plus in ''+ as quoted. This is
              // probably the better option considering the "$empty"+ case
              //
              if (lexeme.empty ())
                qfirst = true;

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

              quoted_mode (lexer_mode::double_quoted);

              st = &state_.top ();
              m = st->mode;

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

              // The same reasoning as above.
              //
              if (lexeme.empty ())
                qfirst = true;

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

    return token (move (lexeme), sep, qtype, qcomp, qfirst, ln, cn);
  }

  pair<bool, bool> lexer::
  skip_spaces ()
  {
    bool r (sep_);
    sep_ = false;

    const state& s (state_.top ());

    // In some special modes we don't skip spaces.
    //
    if (!s.sep_space)
      return make_pair (r, false);

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
          // Note that in this case we don't adjust start.
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

          return make_pair (r, start);
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
              if ((c = peek ()) == '\n' || eos (c))
                return true;
            }

            return false;
          };

          if (ml ())
          {
            // Scan until we see the closing one.
            //
            for (;;)
            {
              if (c == '#' && ml ())
                break;

              if (eos (c = peek ()))
                fail (c) << "unterminated multi-line comment";

              get ();
            }
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
          // See if this is line continuation.
          //
          get ();

          if (peek () == '\n')
            break; // Ignore.

          unget (c);
        }
        // Fall through.
      default:
        return make_pair (r, start); // Not a space.
      }

      get ();
    }

    return make_pair (r, start);
  }
}
