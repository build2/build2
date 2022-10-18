// file      : libbuild2/script/lexer.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/script/lexer.hxx>

#include <cstring> // strchr()

using namespace std;

namespace build2
{
  namespace script
  {
    using type = token_type;

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
      default:
        {
          // Make sure pair separators are only enabled where we expect
          // them.
          //
          // @@ Should we disable pair separators in the eval mode?
          //
          assert (ps == '\0' ||
                  m == lexer_mode::eval ||
                  m == lexer_mode::attribute_value);

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
      case lexer_mode::command_expansion:
      case lexer_mode::here_line_single:
      case lexer_mode::here_line_double:
        r = next_line ();
        break;
      default:
        r = base_lexer::next ();
        break;
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

      const state& st (current_state ());
      lexer_mode m (st.mode);

      auto make_token = [&sep, &m, ln, cn] (type t)
      {
        bool q (m == lexer_mode::here_line_double);

        return token (t, string (), sep,
                      (q ? quote_type::double_ : quote_type::unquoted), q, q,
                      ln, cn,
                      token_printer);
      };

      if (eos (c))
        return make_token (type::eos);

      // NOTE: remember to update mode() if adding new special characters.

      if (m != lexer_mode::command_expansion)
      {
        switch (c)
        {
        case '\n':
          {
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

      // Command operators.
      //
      if (m == lexer_mode::command_expansion)
      {
        if (optional<token> t = next_cmd_op (c, sep))
          return move (*t);
      }

      // Otherwise it is a word.
      //
      unget (c);
      return word (st, sep);
    }

    optional<token> lexer::
    next_cmd_op (const xchar& c, bool sep)
    {
      auto make_token = [&sep, &c] (type t, string v = string ())
      {
        return token (t, move (v), sep,
                      quote_type::unquoted, false, false,
                      c.line, c.column,
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
          optional<type> r;
          xchar p (peek ());

          if (p == '|' || p == '-' || p == '=' || p == '<') // <| <- <= <<
          {
            xchar c (get ());

            switch (p)
            {
            case '|': return make_token (type::in_pass);    // <|
            case '-': return make_token (type::in_null);    // <-
            case '=': return make_token (type::in_file);    // <=
            case '<':                                       // <<
              {
                p = peek ();

                if (p == '=' || p == '<')                   // <<= <<<
                {
                  xchar c (get ());

                  switch (p)
                  {
                  case '=':
                    {
                      r = type::in_doc;                     // <<=
                      break;
                    }
                  case '<':
                    {
                      p = peek ();

                      if (p == '=')
                      {
                        get ();
                        r = type::in_str;                   // <<<=
                      }

                      if (!r && redirect_aliases.lll)
                        r = type::in_lll;                   // <<<

                      // We can still end up with the << or < redirect alias,
                      // if any of them is present.
                      //
                      if (!r)
                        unget (c);
                    }

                    break;
                  }
                }

                if (!r && redirect_aliases.ll)
                  r = type::in_ll;                          // <<

                // We can still end up with the < redirect alias, if it is
                // present.
                //
                if (!r)
                  unget (c);

                break;
              }
            }
          }

          if (!r && redirect_aliases.l)
            r = type::in_l;                                 // <

          if (!r)
            return nullopt;

          // Handle modifiers.
          //
          const char* mods (nullptr);

          switch (redirect_aliases.resolve (*r))
          {
          case type::in_str:
          case type::in_doc: mods = ":/"; break;
          }

          token t (make_token_with_modifiers (*r, mods));

          return t;
        }
        // >
        //
      case '>':
        {
          optional<type> r;
          xchar p (peek ());

          if (p == '|' || p == '-' || p == '!' || p == '&' || // >| >- >! >&
              p == '=' || p == '+' || p == '?' || p == '>')   // >= >+ >? >>
          {
            xchar c (get ());

            switch (p)
            {
            case '|': return make_token (type::out_pass);     // >|
            case '-': return make_token (type::out_null);     // >-
            case '!': return make_token (type::out_trace);    // >!
            case '&': return make_token (type::out_merge);    // >&
            case '=': return make_token (type::out_file_ovr); // >=
            case '+': return make_token (type::out_file_app); // >+
            case '?': return make_token (type::out_file_cmp); // >?
            case '>':                                         // >>
              {
                p = peek ();

                if (p == '?' || p == '>')                     // >>? >>>
                {
                  xchar c (get ());

                  switch (p)
                  {
                  case '?':
                    {
                      r = type::out_doc;                       // >>?
                      break;
                    }
                  case '>':
                    {
                      p = peek ();

                      if (p == '?')
                      {
                        get ();
                        r = type::out_str;                     // >>>?
                      }

                      if (!r && redirect_aliases.ggg)
                        r = type::out_ggg;                     // >>>

                      // We can still end up with the >> or > redirect alias,
                      // if any of themis present.
                      //
                      if (!r)
                        unget (c);
                    }

                    break;
                  }
                }

                if (!r && redirect_aliases.gg)
                  r = type::out_gg;                          // >>

                // We can still end up with the > redirect alias, if it is
                // present.
                //
                if (!r)
                  unget (c);

                break;
              }
            }
          }

          if (!r && redirect_aliases.g)
            r = type::out_g;                                 // >

          if (!r)
            return nullopt;

          // Handle modifiers.
          //
          const char* mods (nullptr);
          const char* stop (nullptr);

          switch (redirect_aliases.resolve (*r))
          {
          case type::out_str:
          case type::out_doc: mods = ":/~"; stop = "~"; break;
          }

          return make_token_with_modifiers (*r, mods, stop);
        }
      }

      return nullopt;
    }
  }
}
