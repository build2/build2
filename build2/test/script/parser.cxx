// file      : build2/test/script/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/parser>

#include <build2/test/script/lexer>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      using type = token_type;

      script parser::
      parse (istream& is, const path& p, target& test_t, target& script_t)
      {
        path_ = &p;

        lexer l (is, *path_, lexer_mode::script_line);
        lexer_ = &l;
        base_parser::lexer_ = &l;

        script_type r (test_t, script_t);
        script_ = &r;

        token t (type::eos, false, 0, 0);
        type tt;
        next (t, tt);

        script (t, tt);

        if (tt != type::eos)
          fail (t) << "unexpected " << t;

        return r;
      }

      void parser::
      script (token& t, token_type& tt)
      {
        while (tt != type::eos)
        {
          script_line (t, tt);
        }
      }

      void parser::
      script_line (token& t, token_type& tt)
      {
        // Parse first chunk. Keep track of whether anything in it was quoted.
        //
        names_type ns;
        location nl (get_location (t));
        lexer_->reset_quoted (t.quoted);
        names (t, tt, ns, true);

        // See if this is a variable assignment or a test command.
        //
        if (tt == type::assign  ||
            tt == type::prepend ||
            tt == type::append)
        {
          // We need to strike a balance between recognizing command lines
          // that contain the assignment operator and variable assignments.
          //
          // If we choose to treat these tokens literally (for example, if we
          // have several names on the LHS), then we have the reversibility
          // problem: we need to restore original whitespaces before and after
          // the assignment operator (e.g., foo=bar vs foo = bar).
          //
          // To keep things simple we will start with the following rule: if
          // the token after the first chunk of input is assignment, then it
          // must be a variable assignment. After all, command lines like this
          // are not expected to be common:
          //
          // $* =x
          //
          // It will also be easy to get the desired behavior with quoting:
          //
          // $* "=x"
          //
          // The only issue here is if $* above expands to a single, simple
          // name (e.g., an executable name) in which case it will be treated
          // as a variable name. One way to resolve it would be to detect
          // "funny" variable names and require that they be quoted (this
          // won't help with built-in commands; maybe we could warn if it's
          // the same as built-in). Note that currently we have no way of
          // knowing it's quoted.
          //
          // Or perhaps we should just let people learn that first assignment
          // needs to be quoted?
          //
          if (ns.size () != 1 || !ns[0].simple () || ns[0].empty ())
            fail (nl) << "variable name expected instead of '" << ns << "'";

          variable_line (t, tt, move (ns[0].value));
        }
        else
          test_line (t, tt, move (ns), move (nl));
      }

      void parser::
      variable_line (token& t, token_type& tt, string name)
      {
        type kind (tt); // Assignment kind.
        const variable_type& var (script_->var_pool.insert (move (name)));

        // We cannot reuse the value mode since it will recognize { which
        // we want to treat as a literal.
        //
        value rhs (variable_value (t, tt, lexer_mode::variable_line));

        value& lhs (kind == type::assign
                    ? script_->assign (var)
                    : script_->append (var));

        // @@ Need to adjust to make strings the default type.
        //
        value_attributes (&var, lhs, move (rhs), kind);
      }

      void parser::
      test_line (token& t, token_type& tt, names_type ns, location nl)
      {
        // Stop recognizing variable assignments.
        //
        mode (lexer_mode::test_line);

        // Keep parsing chunks of the command line until we see the newline or
        // the exit status comparison.
        //
        strings cmd;

        do
        {
          // Process words that we already have.
          //
          bool q (lexer_->quoted ());

          for (name& n: ns)
          {
            string s;

            try
            {
              s = value_traits<string>::convert (move (n), nullptr);
            }
            catch (const invalid_argument&)
            {
              fail (nl) << "invalid string value '" << n << "'";
            }

            // If it is a quoted chunk, then we add the word as is. Otherwise
            // we re-lex it. But if the word doesn't contain any interesting
            // characters (operators plus quotes/escapes), then no need to
            // re-lex.
            //
            if (q || s.find_first_of ("|&<>\'\"\\") == string::npos)
              cmd.push_back (move (s));
            else
            {
              // Come up with a "path" that contains both the original
              // location as well as the expanded string. The resulting
              // diagnostics will look like this:
              //
              // testscript:10:1 ('abc): unterminated single quote
              //
              path name;
              {
                string n (nl.file->string ());
                n += ':';
                n += to_string (nl.line);
                n += ':';
                n += to_string (nl.column);
                n += ": (";
                n += s;
                n += ')';
                name = path (move (n));
              }

              istringstream is (s);
              lexer lex (is, name, lexer_mode::command_line);

              string w;
              bool f (true); // In case the whole thing is empty.
              for (token t (lex.next ()); t.type != type::eos; t = lex.next ())
              {
                // Note that this is not "our" token so we cannot do fail(t).
                // Rather we should do fail(l).
                //
                location l (build2::get_location (t, lex.name ()));

                // Re-lexing double-quotes will recognize $, ( inside as
                // tokens so we have to reverse them back. Since we don't
                // treat spaces as separators we can be sure we will get it
                // right.
                //
                switch (t.type)
                {
                case type::dollar: w += '$'; continue;
                case type::lparen: w += '('; continue;
                }

                // Retire the current word. We need to distinguish between
                // empty and non-existent (e.g., > vs >"").
                //
                if (!w.empty () || f)
                {
                  cmd.push_back (move (w));
                  f = false;
                }

                switch (t.type)
                {
                case type::name: w = move (t.value); f = true; break;

                  // @@ TODO
                  //
                case type::pipe:
                case type::clean:
                case type::log_and:
                case type::log_or:

                case type::in_null:
                case type::in_string:
                case type::in_document:

                case type::out_null:
                case type::out_string:
                case type::out_document:
                  break;
                }
              }

              // Don't forget the last word.
              //
              if (!w.empty () || f)
                cmd.push_back (move (w));
            }
          }

          if (tt == type::newline ||
              tt == type::equal   ||
              tt == type::not_equal)
            break;

          // Parse the next chunk.
          //
          ns.clear ();
          lexer_->reset_quoted (t.quoted);
          names (t, tt, ns, true);

        } while (true);

        //@@ switch mode (we no longer want to recognize command operators)?

        if (tt == type::equal || tt == type::not_equal)
        {
          command_exit (t, tt);
        }

        // here-document
      }

      void parser::
      command_exit (token& t, token_type& tt)
      {
        // The next chunk should be the exit status.
        //
        next (t, tt);
        names_type ns (names (t, tt, true));

        //@@ TODO: validate to be single, simple, non-empty name that
        //         converts to integer (is exit status always non-negative).
      }
    }
  }
}
