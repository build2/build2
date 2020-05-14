// file      : libbuild2/build/script/lexer.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/lexer.hxx>

using namespace std;

namespace build2
{
  namespace build
  {
    namespace script
    {
      using type = token_type;

      void lexer::
      mode (build2::lexer_mode m,
            char ps,
            optional<const char*> esc,
            uintptr_t data)
      {
        bool a (false); // attributes

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
        case lexer_mode::command_line:
          {
            s1 = "=!|&<> $(#\t\n";
            s2 = "==          ";
            break;
          }
        case lexer_mode::first_token:
          {
            // First token on the script line. Like command_line but
            // recognizes variable assignments as separators.
            //
            s1 = "=+!|&<> $(#\t\n";
            s2 = " ==          ";
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
            s1 = "=!|&<> $(#\t\n";
            s2 = "==          ";
            break;
          }
        case lexer_mode::variable_line:
          {
            // Like value except we don't recognize '{'.
            //
            s1 = " $(#\t\n";
            s2 = "      ";
            break;
          }
        default:
          {
            base_lexer::mode (m, ps, esc);
            return;
          }
        }

        assert (ps == '\0');
        state_.push (state {m, data, nullopt, a, ps, s, n, q, *esc, s1, s2});
      }

      token lexer::
      next ()
      {
        token r;

        switch (state_.top ().mode)
        {
        case lexer_mode::command_line:
        case lexer_mode::first_token:
        case lexer_mode::second_token:
        case lexer_mode::variable_line:
          r = next_line ();
          break;
        default: return base_lexer::next ();
        }

        if (r.qtype != quote_type::unquoted)
          ++quoted_;

        return r;
      }

      token lexer::
      next_line ()
      {
        bool sep (skip_spaces ().first);

        xchar c (get ());
        uint64_t ln (c.line), cn (c.column);

        state st (state_.top ()); // Make copy (see first/second_token).
        lexer_mode m (st.mode);

        auto make_token = [&sep, ln, cn] (type t)
        {
          return token (t, sep, ln, cn, token_printer);
        };

        // Handle attributes (do it first to make sure the flag is cleared
        // regardless of what we return).
        //
        if (st.attributes)
        {
          assert (m == lexer_mode::variable_line);

          state_.top ().attributes = false;

          if (c == '[')
            return make_token (type::lsbrace);
        }

        if (eos (c))
          return make_token (type::eos);

        // Expire certain modes at the end of the token. Do it early in case
        // we push any new mode (e.g., double quote).
        //
        if (m == lexer_mode::first_token || m == lexer_mode::second_token)
          state_.pop ();

        // NOTE: remember to update mode() if adding new special characters.

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

          // Variable expansion, function call, and evaluation context.
          //
        case '$': return make_token (type::dollar);
        case '(': return make_token (type::lparen);
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

        // Command operators.
        //
        if (m == lexer_mode::command_line ||
            m == lexer_mode::first_token  ||
            m == lexer_mode::second_token)
        {
          if (optional<token> t = next_cmd_op (c, sep, m))
            return move (*t);
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

        // Customized implementation that handles special variable names ($>).
        //
        if (m != lexer_mode::variable)
          return base_lexer::word (st, sep);

        xchar c (peek ());

        if (c != '>')
          return base_lexer::word (st, sep);

        get ();

        state_.pop (); // Expire the variable mode.
        return token (string (1, c),
                      sep,
                      quote_type::unquoted, false,
                      c.line, c.column);
      }
    }
  }
}
