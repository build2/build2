// file      : libbuild2/test/script/lexer.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/script/lexer.hxx>

#include <cstring> // strchr()

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      using type = token_type;

      build2::script::redirect_aliases lexer::redirect_aliases {
        type (type::in_str),
        type (type::in_doc),
        type (type::in_file),
        type (type::out_str),
        type (type::out_doc),
        type (type::out_file_cmp)};

      void lexer::
      mode (base_mode m, char ps, optional<const char*> esc, uintptr_t data)
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
        case lexer_mode::for_loop:
          {
            // Leading tokens of the for-loop. Like command_line but also
            // recognizes lsbrace like value.
          }
          // Fall through.
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
            s1 = "; $(#\t\n";
            s2 = "       ";
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
            // Recognize special variable names ($*, $N, $~, $@). See also an
            // extra check in word() below.
            //
            if (m == lexer_mode::variable)
            {
              assert (data == 0);
              data = reinterpret_cast<uintptr_t> ("*~@0123456789");
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
        case lexer_mode::description_line:
          r = next_description ();
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

        // Line separators.
        //
        if (m == lexer_mode::command_line  ||
            m == lexer_mode::first_token   ||
            m == lexer_mode::second_token  ||
            m == lexer_mode::variable_line ||
            m == lexer_mode::for_loop)
        {
          switch (c)
          {
          case ';': return make_token (type::semi);
          }
        }

        if (m == lexer_mode::command_line ||
            m == lexer_mode::first_token  ||
            m == lexer_mode::second_token ||
            m == lexer_mode::for_loop)
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
          expire_mode (); // Expire the description mode.
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

        return token (move (lexeme), false,
                      quote_type::unquoted, false, false,
                      ln, cn);
      }

      token lexer::
      word (const state& st, bool sep)
      {
        lexer_mode m (st.mode); // Save.

        token r (base_lexer::word (st, sep));

        if (m == lexer_mode::variable)
        {
          if (r.type == type::word &&
              r.value.size () == 1 &&
              digit (r.value[0])) // $N
          {
            xchar c (peek ());

            if (digit (c)) // $NN
              fail (c) << "multi-digit special variable name" <<
                info << "use '($*[NN])' to access elements beyond 9";
          }
        }

        return r;
      }
    }
  }
}
