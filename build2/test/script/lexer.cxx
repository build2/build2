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
      mode (base_mode m, char)
      {
        const char* s1 (nullptr);
        const char* s2 (nullptr);
        bool s (true);

        switch (m)
        {
        case lexer_mode::script_line:
          {
            s1 = "=+!|&<> $(#\t\n";
            s2 = " ==          ";
            break;
          }
        case lexer_mode::variable_line:
          {
            // Like value except we don't recognize {.
            //
            s1 = " $([]#\t\n";
            s2 = "        ";
            break;
          }
        case lexer_mode::test_line:
          {
            // As script_line but without variable assignments.
            //
            s1 = "=!|&<> $(#\t\n";
            s2 = "==          ";
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
            // newlines as a separator.
            //
            s1 = "$(\n";
            s2 = "   ";
            s = false;
            break;
          }
        case lexer_mode::single_quoted:
        case lexer_mode::double_quoted:
          quoted_ = true;
          // Fall through.
        default:
          {
            // Disable pair separator.
            //
            base_lexer::mode (m, '\0');
          }
        }

        state_.push (state {m, '\0', s, s1, s2});
      }

      token lexer::
      next_impl ()
      {
        switch (state_.top ().mode)
        {
        case lexer_mode::script_line:
        case lexer_mode::variable_line:
        case lexer_mode::test_line:
        case lexer_mode::command_line:
        case lexer_mode::here_line:      return next_line ();
        default:                         return base_lexer::next_impl ();
        }
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

        lexer_mode m (state_.top ().mode);

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

        // Command line operator/separators.
        //
        if (m == lexer_mode::script_line || m == lexer_mode::test_line)
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
        if (m == lexer_mode::script_line ||
            m == lexer_mode::test_line   ||
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

              if (p == '!' || p == '<')
              {
                get ();
                return make_token (
                  p == '!' ? type::in_null : type::in_document);
              }
              else
                return make_token (type::in_string);

            }
            // >
            //
          case '>':
            {
              xchar p (peek ());

              if (p == '!' || p == '>')
              {
                get ();
                return make_token (
                  p == '!' ? type::out_null : type::out_document);
              }
              else
                return make_token (type::out_string);
            }
          }
        }

        // Variable assignment (=, +=, =+).
        //
        if (m == lexer_mode::script_line)
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

        // Otherwise it is a name.
        //
        unget (c);
        return name (sep);
      }

      token lexer::
      name (bool sep)
      {
        // Customized implementation that handles special variable names ($*,
        // $~, $NNN).
        //
        if (state_.top ().mode != lexer_mode::variable)
          return base_lexer::name (sep);

        xchar c (peek ());

        if (c != '*' && c != '~' && !digit (c))
          return base_lexer::name (sep);

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
