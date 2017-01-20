// file      : build2/test/script/lexer.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/lexer>

#include <cstring> // strchr()

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      using type = token_type;

      void lexer::
      mode (base_mode m, char ps, optional<const char*> esc)
      {
        const char* s1 (nullptr);
        const char* s2 (nullptr);
        bool s (true);
        bool q (true);

        if (!esc)
        {
          assert (!state_.empty ());
          esc = state_.top ().escapes;
        }

        switch (m)
        {
        case lexer_mode::command_line:
          {
            s1 = ":;=!|&<> $(#\t\n";
            s2 = "  ==          ";
            break;
          }
        case lexer_mode::first_token:
          {
            // First token on the script line. Like command_line but
            // recognizes leading '.+-{}' as tokens as well as variable
            // assignments as separators.
            //
            // Note that to recognize only leading '.+-{}' we shouldn't add
            // them to the separator strings.
            //
            s1 = ":;=+!|&<> $(#\t\n";
            s2 = "   ==          ";
            break;
          }
        case lexer_mode::second_token:
          {
            // Second token on the script line. Like command_line but
            // recognizes leading variable assignments.
            //
            // Note that to recognize only leading assignments we shouldn't
            // add them to the separator strings (so this is identical to
            // command_line).
            //
            s1 = ":;=!|&<> $(#\t\n";
            s2 = "  ==          ";
            break;
          }
        case lexer_mode::variable_line:
          {
            // Like value except we recognize ';' and don't recognize '{'.
            // Note that we don't recognize ':' since having a trailing
            // variable assignment is illegal.
            //
            s1 = "; $([]#\t\n";
            s2 = "         ";
            break;
          }

        case lexer_mode::command_expansion:
          {
            // Note that whitespaces are not word separators in this mode.
            //
            s1 = "|&<>";
            s2 = "    ";
            s = false;
            break;
          }
        case lexer_mode::here_line_single:
          {
            // This one is like a single-quoted string except it treats
            // newlines as a separator. We also treat quotes as literals.
            //
            // Note that it might be tempting to enable line continuation
            // escapes. However, we will then have to also enable escaping of
            // the backslash, which makes it a lot less tempting.
            //
            s1 = "\n";
            s2 = " ";
            esc = ""; // Disable escape sequences.
            s = false;
            q = false;
            break;
          }
        case lexer_mode::here_line_double:
          {
            // This one is like a double-quoted string except it treats
            // newlines as a separator. We also treat quotes as literals.
            //
            s1 = "$(\n";
            s2 = "   ";
            s = false;
            q = false;
            break;
          }
        case lexer_mode::description_line:
          {
            // This one is like a single-quoted string and has an ad hoc
            // implementation.
            //
            break;
          }
        default:
          {
            // Make sure pair separators are only enabled where we expect
            // them.
            //
            // @@ Should we disable pair separators in the eval mode?
            //
            assert (ps == '\0' ||
                    m == lexer_mode::eval ||
                    m == lexer_mode::attribute);

            base_lexer::mode (m, ps, esc);
            return;
          }
        }

        assert (ps == '\0');
        state_.push (state {m, ps, s, q, *esc, s1, s2});
      }

      token lexer::
      next_impl ()
      {
        token r;

        switch (state_.top ().mode)
        {
        case lexer_mode::command_line:
        case lexer_mode::first_token:
        case lexer_mode::second_token:
        case lexer_mode::variable_line:
        case lexer_mode::command_expansion:
        case lexer_mode::here_line_single:
        case lexer_mode::here_line_double:
          r = next_line ();
          break;
        case lexer_mode::description_line:
          r = next_description ();
          break;
        default:
          r = base_lexer::next_impl ();
          break;
        }

        if (r.qtype != quote_type::unquoted)
          ++quoted_;

        return r;
      }

      token lexer::
      next_line ()
      {
        bool sep (skip_spaces ());

        xchar c (get ());
        uint64_t ln (c.line), cn (c.column);

        if (eos (c))
          return token (type::eos, sep, ln, cn, token_printer);

        state st (state_.top ()); // Make copy (see first/second_token).
        lexer_mode m (st.mode);

        auto make_token = [&sep, &m, ln, cn] (type t, string v = string ())
        {
          bool q (m == lexer_mode::here_line_double);

          return token (t, move (v), sep,
                        (q ? quote_type::double_ : quote_type::unquoted), q,
                        ln, cn,
                        token_printer);
        };

        auto make_token_with_modifiers =
          [&make_token, this] (type t,
                               const char* mods,           // To recorgnize.
                               const char* stop = nullptr) // To stop after.
        {
          string v;
          if (mods != nullptr)
          {
            for (xchar p (peek ());
                 (strchr (mods, p) != nullptr &&      // Modifier.
                  strchr (v.c_str (), p) == nullptr); // Not already seen.
                 p = peek ())
            {
              get ();
              v += p;

              if (stop != nullptr && strchr (stop, p) != nullptr)
                break;
            }
          }

          return make_token (t, move (v));
        };

        // Expire certain modes at the end of the token. Do it early in case
        // we push any new mode (e.g., double quote).
        //
        if (m == lexer_mode::first_token || m == lexer_mode::second_token)
          state_.pop ();

        // NOTE: remember to update mode() if adding new special characters.

        if (m != lexer_mode::command_expansion)
        {
          switch (c)
          {
          case '\n':
            {
              // Expire variable value mode at the end of the line.
              //
              if (m == lexer_mode::variable_line)
                state_.pop ();

              sep = true; // Treat newline as always separated.
              return make_token (type::newline);
            }
          }
        }

        if (m != lexer_mode::here_line_single)
        {
          switch (c)
          {
            // Variable expansion, function call, and evaluation context.
            //
          case '$': return make_token (type::dollar);
          case '(': return make_token (type::lparen);
          }
        }


        if (m == lexer_mode::variable_line)
        {
          switch (c)
          {
            // Attributes.
            //
          case '[': return make_token (type::lsbrace);
          case ']': return make_token (type::rsbrace);
          }
        }

        // Line separators.
        //
        if (m == lexer_mode::command_line ||
            m == lexer_mode::first_token  ||
            m == lexer_mode::second_token ||
            m == lexer_mode::variable_line)
        {
          switch (c)
          {
          case ';': return make_token (type::semi);
          }
        }

        if (m == lexer_mode::command_line ||
            m == lexer_mode::first_token  ||
            m == lexer_mode::second_token)
        {
          switch (c)
          {
          case ':': return make_token (type::colon);
          }
        }

        // Command line operator/separators.
        //
        if (m == lexer_mode::command_line ||
            m == lexer_mode::first_token  ||
            m == lexer_mode::second_token)
        {
          switch (c)
          {
            // Comparison (==, !=).
            //
          case '=':
          case '!':
            {
              if (peek () == '=')
              {
                get ();
                return make_token (c == '=' ? type::equal : type::not_equal);
              }
            }
          }
        }

        // Command operators/separators.
        //
        if (m == lexer_mode::command_line ||
            m == lexer_mode::first_token  ||
            m == lexer_mode::second_token ||
            m == lexer_mode::command_expansion)
        {
          switch (c)
          {
            // |, ||
            //
          case '|':
            {
              if (peek () == '|')
              {
                get ();
                return make_token (type::log_or);
              }
              else
                return make_token (type::pipe);
            }
            // &, &&
            //
          case '&':
            {
              xchar p (peek ());

              if (p == '&')
              {
                get ();
                return make_token (type::log_and);
              }

              // These modifiers are mutually exclusive so stop after seeing
              // either one.
              //
              return make_token_with_modifiers (type::clean, "!?", "!?");
            }
            // <
            //
          case '<':
            {
              type r (type::in_str);
              xchar p (peek ());

              if (p == '|' || p == '-' || p == '<')
              {
                get ();

                switch (p)
                {
                case '|': return make_token (type::in_pass);
                case '-': return make_token (type::in_null);
                case '<':
                  {
                    r = type::in_doc;
                    p = peek ();

                    if (p == '<')
                    {
                      get ();
                      r = type::in_file;
                    }
                    break;
                  }
                }
              }

              // Handle modifiers.
              //
              const char* mods (nullptr);
              switch (r)
              {
              case type::in_str:
              case type::in_doc: mods = ":/"; break;
              }

              return make_token_with_modifiers (r, mods);
            }
            // >
            //
          case '>':
            {
              type r (type::out_str);
              xchar p (peek ());

              if (p == '|' || p == '-' || p == '&' ||
                  p == '=' || p == '+' || p == '>')
              {
                get ();

                switch (p)
                {
                case '|': return make_token (type::out_pass);
                case '-': return make_token (type::out_null);
                case '&': return make_token (type::out_merge);
                case '=': return make_token (type::out_file_ovr);
                case '+': return make_token (type::out_file_app);
                case '>':
                  {
                    r = type::out_doc;
                    p = peek ();

                    if (p == '>')
                    {
                      get ();
                      r = type::out_file_cmp;
                    }
                    break;
                  }
                }
              }

              // Handle modifiers.
              //
              const char* mods (nullptr);
              const char* stop (nullptr);
              switch (r)
              {
              case type::out_str:
              case type::out_doc:  mods = ":/~"; stop = "~"; break;
              }

              return make_token_with_modifiers (r, mods, stop);
            }
          }
        }

        // Dot, plus/minus, and left/right curly braces.
        //
        if (m == lexer_mode::first_token)
        {
          switch (c)
          {
          case '.': return make_token (type::dot);
          case '+': return make_token (type::plus);
          case '-': return make_token (type::minus);
          case '{': return make_token (type::lcbrace);
          case '}': return make_token (type::rcbrace);
          }
        }

        // Variable assignment (=, +=, =+).
        //
        if (m == lexer_mode::second_token)
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
            }
          }
        }

        // Otherwise it is a word.
        //
        unget (c);
        return word (st, sep);
      }

      token lexer::
      next_description ()
      {
        xchar c (peek ());

        if (eos (c))
          fail (c) << "expected newline at the end of description line";

        uint64_t ln (c.line), cn (c.column);

        if (c == '\n')
        {
          get ();
          state_.pop (); // Expire the description mode.
          return token (type::newline, true, ln, cn, token_printer);
        }

        string lexeme;

        // For now no line continutions though we could support them.
        //
        for (; !eos (c) && c != '\n'; c = peek ())
        {
          get ();
          lexeme += c;
        }

        return token (move (lexeme),
                      false,
                      quote_type::unquoted, false,
                      ln, cn);
      }

      token lexer::
      word (state st, bool sep)
      {
        lexer_mode m (st.mode);

        // Customized implementation that handles special variable names ($*,
        // $N, $~, $@).
        //
        if (m != lexer_mode::variable)
          return base_lexer::word (st, sep);

        xchar c (peek ());

        if (c != '*' && c != '~' && c != '@' && !digit (c))
          return base_lexer::word (st, sep);

        get ();

        if (digit (c) && digit (peek ()))
          fail (c) << "multi-digit special variable name";

        state_.pop (); // Expire the variable mode.
        return token (string (1, c),
                      sep,
                      quote_type::unquoted, false,
                      c.line, c.column);
      }
    }
  }
}
