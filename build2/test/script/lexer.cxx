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
            s1 = "=+!|&<> $()#\t\n";
            s2 = " ==           ";
            break;
          }
        case lexer_mode::variable_line:
          {
            // Like value except we don't recognize {.
            //
            s1 = " $()[]#\t\n";
            s2 = "         ";
            break;
          }
        case lexer_mode::test_line:
          {
            // As script_line but without variable assignments.
            //
            s1 = "=!|&<> $()#\t\n";
            s2 = "==           ";
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
        case lexer_mode::command_line: return next_line ();
        default:                       return base_lexer::next_impl ();
        }
      }

      token lexer::
      next_line ()
      {
        bool sep (skip_spaces ());

        xchar c (get ());
        uint64_t ln (c.line), cn (c.column);

        if (eos (c))
          return token (type::eos, sep, ln, cn);

        lexer_mode m (state_.top ().mode);

        // NOTE: remember to update mode() if adding new special characters.

        if (m != lexer_mode::command_line)
        {
          switch (c)
          {
          case '\n':
            {
              return token (type::newline, sep, ln, cn);
            }

            // Variable expansion, function call, and evaluation context.
            //
          case '$': return token (type::dollar, sep, ln, cn);
          case '(': return token (type::lparen, sep, ln, cn);
          case ')': return token (type::rparen, sep, ln, cn);
          }
        }

        if (m == lexer_mode::variable_line)
        {
          switch (c)
          {
            // Attributes.
            //
          case '[': return token (type::lsbrace, sep, ln, cn);
          case ']': return token (type::rsbrace, sep, ln, cn);
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
                return token (
                  c == '=' ? type::equal : type::not_equal, sep, ln, cn);
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
                return token (type::log_or, sep, ln, cn);
              }
              else
                return token (type::pipe, sep, ln, cn);
            }
            // &, &&
            //
          case '&':
            {
              if (peek () == '&')
              {
                get ();
                return token (type::log_and, sep, ln, cn);
              }
              else
                return token (type::clean, sep, ln, cn);
            }
            // <
            //
          case '<':
            {
              xchar p (peek ());

              if (p == '!' || p == '<')
              {
                get ();
                return token (
                  p == '!' ? type::in_null : type::in_document, sep, ln, cn);
              }
              else
                return token (type::in_string, sep, ln, cn);

            }
            // >
            //
          case '>':
            {
              xchar p (peek ());

              if (p == '!' || p == '>')
              {
                get ();
                return token (
                  p == '!' ? type::out_null : type::out_document, sep, ln, cn);
              }
              else
                return token (type::out_string, sep, ln, cn);
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
                return token (type::prepend, sep, ln, cn);
              }
              else
                return token (type::assign, sep, ln, cn);
            }
          case '+':
            {
              if (peek () == '=')
              {
                get ();
                return token (type::append, sep, ln, cn);
              }
            }
          }
        }

        // Otherwise it is a name.
        //
        unget (c);
        return name (sep);
      }
    }
  }
}
