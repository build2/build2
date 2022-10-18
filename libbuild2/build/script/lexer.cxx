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

      build2::script::redirect_aliases lexer::redirect_aliases {
        type (type::in_file),
        type (type::in_doc),
        type (type::in_str),
        type (type::out_file_ovr),
        type (type::out_file_app),
        nullopt};

      void lexer::
      mode (build2::lexer_mode m,
            char ps,
            optional<const char*> esc,
            uintptr_t data)
      {
        const char* s1 (nullptr);
        const char* s2 (nullptr);

        bool s (true); // space
        bool n (true); // newline
        bool q (true); // quotes

        if (!esc)
          esc = current_state ().escapes;

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
        case lexer_mode::for_loop:
          {
            // Leading tokens of the for-loop. Like command_line but
            // recognizes colon as a separator and lsbrace like value.
            //
            // Note that while sensing the form of the for-loop (`for x:...`
            // vs `for x <...`) we need to make sure that the pre-parsed token
            // types are valid for the execution phase.
            //
            s1 = ":=!|&<> $(#\t\n";
            s2 = " ==          ";
            break;
          }
        default:
          {
            // Recognize special variable names ($>, $<, $~).
            //
            if (m == lexer_mode::variable)
            {
              assert (data == 0);
              data = reinterpret_cast<uintptr_t> ("><~");
            }

            base_lexer::mode (m, ps, esc, data);
            return;
          }
        }

        assert (ps == '\0');
        mode_impl (
          state {m, data, nullopt, false, false, ps, s, n, q, *esc, s1, s2});
      }

      token lexer::
      next ()
      {
        token r;

        switch (mode ())
        {
        case lexer_mode::command_line:
        case lexer_mode::first_token:
        case lexer_mode::second_token:
        case lexer_mode::variable_line:
        case lexer_mode::for_loop:
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

        state st (current_state ()); // Make copy (see first/second_token).
        lexer_mode m (st.mode);

        auto make_token = [&sep, ln, cn] (type t)
        {
          return token (t, sep, ln, cn, token_printer);
        };

        // Handle `[` (do it first to make sure the flag is cleared regardless
        // of what we return).
        //
        if (st.lsbrace)
        {
          assert (m == lexer_mode::variable_line ||
                  m == lexer_mode::for_loop);

          current_state ().lsbrace = false; // Note: st is a copy.

          if (c == '[' && (!st.lsbrace_unsep || !sep))
            return make_token (type::lsbrace);
        }

        if (eos (c))
          return make_token (type::eos);

        // Expire certain modes at the end of the token. Do it early in case
        // we push any new mode (e.g., double quote).
        //
        if (m == lexer_mode::first_token || m == lexer_mode::second_token)
          expire_mode ();

        // NOTE: remember to update mode() if adding new special characters.

        switch (c)
        {
        case '\n':
          {
            // Expire variable value mode at the end of the line.
            //
            if (m == lexer_mode::variable_line)
              expire_mode ();

            sep = true; // Treat newline as always separated.
            return make_token (type::newline);
          }

          // Variable expansion, function call, and evaluation context.
          //
        case '$': return make_token (type::dollar);
        case '(': return make_token (type::lparen);
        }

        if (m == lexer_mode::for_loop)
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
            m == lexer_mode::second_token ||
            m == lexer_mode::for_loop)
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
            m == lexer_mode::second_token ||
            m == lexer_mode::for_loop)
        {
          if (optional<token> t = next_cmd_op (c, sep))
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
    }
  }
}
