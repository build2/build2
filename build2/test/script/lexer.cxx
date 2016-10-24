// file      : build2/test/script/lexer.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/lexer>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      using type = token_type;

      void lexer::
      mode (base_mode m, char ps)
      {
        const char* s1 (nullptr);
        const char* s2 (nullptr);
        bool s (true);
        bool q (true);

        switch (m)
        {
        case lexer_mode::script_line:
          {
            s1 = ";=!|&<> $(#\t\n";
            s2 = " ==          ";
            break;
          }
        case lexer_mode::first_token:
          {
            // First token on the script line. Like script_line but recognizes
            // leading plus/minus and variable assignments as separators.
            //
            // Note that to recognize only leading plus/minus we shouldn't add
            // them to the separator strings.
            //
            s1 = ";=+!|&<> $(#\t\n";
            s2 = "  ==          ";
            break;
          }
        case lexer_mode::second_token:
          {
            // Second token on the script line. Like script_line but
            // recognizes leading variable assignments.
            //
            // Note that to recognize only leading assignments we shouldn't
            // add them to the separator strings (so this is identical to
            // script_line).
            //
            s1 = ";=!|&<> $(#\t\n";
            s2 = " ==          ";
            break;
          }
        case lexer_mode::variable_line:
          {
            // Like value except we recognize ';' and don't recognize '{'.
            //
            s1 = "; $([]#\t\n";
            s2 = "         ";
            break;
          }

        case lexer_mode::command_line:
          {
            // Note that whitespaces are not word separators in this mode.
            //
            s1 = "|&<>";
            s2 = "    ";
            s = false;
            break;
          }
        case lexer_mode::here_line:
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
        default:
          {
            // Disable pair separator except for attributes.
            //
            base_lexer::mode (m, m != lexer_mode::attribute ? '\0' : ps);
            return;
          }
        }

        state_.push (state {m, '\0', s, q, s1, s2});
      }

      token lexer::
      next_impl ()
      {
        token r;

        switch (state_.top ().mode)
        {
        case lexer_mode::script_line:
        case lexer_mode::first_token:
        case lexer_mode::second_token:
        case lexer_mode::variable_line:
        case lexer_mode::command_line:
        case lexer_mode::here_line:     r = next_line ();             break;
        default:                        r = base_lexer::next_impl (); break;
        }

        if (r.quoted)
          ++quoted_;

        return r;
      }

      token lexer::
      next_line ()
      {
        bool sep (skip_spaces ());

        xchar c (get ());
        uint64_t ln (c.line), cn (c.column);

        auto make_token = [sep, ln, cn] (type t)
        {
          return token (t, sep, ln, cn, token_printer);
        };

        if (eos (c))
          return make_token (type::eos);

        state st (state_.top ()); // Make copy (see first/second_token).
        lexer_mode m (st.mode);

        // Expire certain modes at the end of the token. Do it early in case
        // we push any new mode (e.g., double quote).
        //
        if (m == lexer_mode::first_token || m == lexer_mode::second_token)
          state_.pop ();

        // NOTE: remember to update mode() if adding new special characters.

        if (m != lexer_mode::command_line)
        {
          switch (c)
          {
          case '\n':
            {
              // Expire variable value mode at the end of the line.
              //
              if (m == lexer_mode::variable_line)
                state_.pop ();

              return make_token (type::newline);
            }

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
        if (m == lexer_mode::script_line  ||
            m == lexer_mode::first_token  ||
            m == lexer_mode::second_token ||
            m == lexer_mode::variable_line)
        {
          switch (c)
          {
          case ';': return make_token (type::semi);
          }
        }

        // Command line operator/separators.
        //
        if (m == lexer_mode::script_line ||
            m == lexer_mode::first_token ||
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
        if (m == lexer_mode::script_line  ||
            m == lexer_mode::first_token  ||
            m == lexer_mode::second_token ||
            m == lexer_mode::command_line)
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
              if (peek () == '&')
              {
                get ();
                return make_token (type::log_and);
              }
              else
                return make_token (type::clean);
            }
            // <
            //
          case '<':
            {
              xchar p (peek ());

              if (p == '+' || p == '-' || p == ':' || p == '<')
              {
                get ();

                switch (p)
                {
                case '+': return make_token (type::in_pass);
                case '-': return make_token (type::in_null);
                case ':': return make_token (type::in_str_nn);
                case '<':
                  {
                    p = peek ();

                    if (p == ':')
                    {
                      get ();
                      return make_token (type::in_doc_nn);
                    }
                    else
                      return make_token (type::in_doc);
                  }
                }
              }
              else
                return make_token (type::in_str);

            }
            // >
            //
          case '>':
            {
              xchar p (peek ());

              if (p == '+' || p == '-' || p == ':' || p == '>')
              {
                get ();

                switch (p)
                {
                case '+': return make_token (type::out_pass);
                case '-': return make_token (type::out_null);
                case ':': return make_token (type::out_str_nn);
                case '>':
                  {
                    p = peek ();

                    if (p == ':')
                    {
                      get ();
                      return make_token (type::out_doc_nn);
                    }
                    else
                      return make_token (type::out_doc);
                  }
                }
              }
              else
                return make_token (type::out_str);
            }
          }
        }

        // Plus/minus.
        //
        if (m == lexer_mode::first_token)
        {
          switch (c)
          {
          case '+': return make_token (type::plus);
          case '-': return make_token (type::minus);
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
      word (state st, bool sep)
      {
        lexer_mode m (st.mode);

        // Customized implementation that handles special variable names ($*,
        // $NN, $~, $@).
        //
        if (m != lexer_mode::variable)
          return base_lexer::word (st, sep);

        xchar c (peek ());

        if (c != '*' && c != '~' && c != '@' && !digit (c))
          return base_lexer::word (st, sep);

        uint64_t ln (c.line), cn (c.column);
        string lexeme;

        get ();
        lexeme += c;

        if (digit (c))
        {
          for (; digit (c = peek ()); get ())
            lexeme += c;
        }

        state_.pop (); // Expire the variable mode.
        return token (move (lexeme), sep, false, ln, cn);
      }
    }
  }
}
