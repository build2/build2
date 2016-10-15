// file      : build2/test/script/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/parser>

#include <build2/test/script/lexer>
#include <build2/test/script/runner>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      using type = token_type;

      void parser::
      parse (istream& is, const path& p, script& s, runner& r)
      {
        path_ = &p;

        lexer l (is, *path_, lexer_mode::script_line);
        lexer_ = &l;
        base_parser::lexer_ = &l;

        script_ = &s;
        runner_ = &r;

        token t;
        type tt;
        next (t, tt);

        parse_script (t, tt);

        if (tt != type::eos)
          fail (t) << "unexpected " << t;
      }

      void parser::
      parse_script (token& t, token_type& tt)
      {
        for (; tt != type::eos; next (t, tt))
        {
          parse_script_line (t, tt);
        }
      }

      void parser::
      parse_script_line (token& t, token_type& tt)
      {
        // Parse first chunk. Keep track of whether anything in it was quoted.
        //
        names ns;
        location nl (get_location (t));
        lexer_->reset_quoted (t.quoted);
        parse_names (t, tt, ns, true, "variable or program name");

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

          parse_variable_line (t, tt, move (ns[0].value));
        }
        else
          parse_test_line (t, tt, move (ns), move (nl));
      }

      void parser::
      parse_variable_line (token& t, token_type& tt, string name)
      {
        type kind (tt); // Assignment kind.
        const variable& var (script_->var_pool.insert (move (name)));

        // We cannot reuse the value mode since it will recognize { which
        // we want to treat as a literal.
        //
        value rhs (parse_variable_value (t, tt, lexer_mode::variable_line));

        if (tt != type::newline)
          fail (t) << "unexpected " << t;

        value& lhs (kind == type::assign
                    ? script_->assign (var)
                    : script_->append (var));

        // @@ Need to adjust to make strings the default type.
        //
        apply_value_attributes (&var, lhs, move (rhs), kind);
      }

      void parser::
      parse_test_line (token& t, token_type& tt, names ns, location nl)
      {
        // Stop recognizing variable assignments.
        //
        mode (lexer_mode::test_line);

        test ts;

        // Pending positions where the next word should go.
        //
        enum class pending
        {
          none,
          program,
          in_string,
          in_document,
          out_string,
          out_document,
          err_string,
          err_document
        };
        pending p (pending::program);

        // Ordered sequence of here-document redirects that we can expect to
        // see after the command line. We temporarily store the end marker
        // as the redirect's value.
        //
        vector<reference_wrapper<redirect>> hd;

        // Add the next word to either one of the pending positions or
        // to program arguments by default.
        //
        auto add_word = [&ts, &p, &hd, this] (string&& w, const location& l)
        {
          switch (p)
          {
          case pending::none: ts.arguments.push_back (move (w)); break;
          case pending::program:
          {
            try
            {
              ts.program = path (move (w));

              if (ts.program.empty ())
                fail (l) << "empty program path";
            }
            catch (const invalid_path& e)
            {
              fail (l) << "invalid program path '" << e.path << "'";
            }
            break;
          }
          case pending::in_document: hd.push_back (ts.in); // Fall through.
          case pending::in_string: ts.in.value = move (w); break;

          case pending::out_document: hd.push_back (ts.out); // Fall through.
          case pending::out_string: ts.out.value = move (w); break;

          case pending::err_document: hd.push_back (ts.err); // Fall through.
          case pending::err_string: ts.err.value = move (w); break;
          }

          p = pending::none;
        };

        // Make sure we don't have any pending positions to fill.
        //
        auto check_pending = [&p, this] (const location& l)
        {
          const char* what (nullptr);

          switch (p)
          {
          case pending::none:                                            break;
          case pending::program:      what = "program";                  break;
          case pending::in_string:    what = "stdin here-string";        break;
          case pending::in_document:  what = "stdin here-document end";  break;
          case pending::out_string:   what = "stdout here-string";       break;
          case pending::out_document: what = "stdout here-document end"; break;
          case pending::err_string:   what = "stderr here-string";       break;
          case pending::err_document: what = "stderr here-document end"; break;
          }

          if (what != nullptr)
            fail (l) << "missing " << what;
        };

        // Parse the redirect operator.
        //
        auto parse_redirect =
          [&ts, &p, this] (const token& t, const location& l)
        {
          // Our semantics is the last redirect seen takes effect.
          //
          assert (p == pending::none);

          // See if we have the file descriptor.
          //
          unsigned long fd (3);
          if (!t.separated)
          {
            if (!ts.arguments.empty ())
              fail (l) << "missing redirect file descriptor";

            const string& s (ts.arguments.back ());

            try
            {
              size_t n;
              fd = stoul (s, &n);

              if (n != s.size () || fd > 2)
                throw invalid_argument (string ());
            }
            catch (const exception&)
            {
              fail (l) << "invalid redirect file descriptor '" << s << "'";
            }

            ts.arguments.pop_back ();
          }

          type tt (t.type);

          // Validate/set default file descriptor.
          //
          switch (tt)
          {
          case type::in_null:
          case type::in_string:
          case type::in_document:
            {
              if ((fd = fd == 3 ? 0 : fd) != 0)
                fail (l) << "invalid in redirect file descriptor " << fd;

              break;
            }
          case type::out_null:
          case type::out_string:
          case type::out_document:
            {
              if ((fd = fd == 3 ? 1 : fd) == 0)
                fail (l) << "invalid out redirect file descriptor " << fd;

              break;
            }
          }

          redirect_type rt;
          switch (tt)
          {
          case type::in_null:
          case type::out_null:     rt = redirect_type::null;          break;
          case type::in_string:
          case type::out_string:   rt = redirect_type::here_string;   break;
          case type::in_document:
          case type::out_document: rt = redirect_type::here_document; break;
          }

          redirect& r (fd == 0 ? ts.in : fd == 1 ? ts.out : ts.err);
          r.type = rt;

          switch (rt)
          {
          case redirect_type::none:
          case redirect_type::null:
            break;
          case redirect_type::here_string:
            switch (fd)
            {
            case 0: p = pending::in_string;  break;
            case 1: p = pending::out_string; break;
            case 2: p = pending::err_string; break;
            }
            break;
          case redirect_type::here_document:
            switch (fd)
            {
            case 0: p = pending::in_document;  break;
            case 1: p = pending::out_document; break;
            case 2: p = pending::err_document; break;
            }
            break;
          }
        };

        // Keep parsing chunks of the command line until we see the newline or
        // the exit status comparison.
        //
        for (bool done (false); !done; )
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
              add_word (move (s), nl);
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

              // Treat the first "sub-token" as always separated from what we
              // saw earlier.
              //
              // Note that this is not "our" token so we cannot do fail(t).
              // Rather we should do fail(l).
              //
              token t (lex.next ());
              location l (build2::get_location (t, name));
              t.separated = true;

              for (; t.type != type::eos; t = lex.next ())
              {
                type tt (t.type);
                l = build2::get_location (t, name);

                // Re-lexing double-quotes will recognize $, ( inside as
                // tokens so we have to reverse them back. Since we don't
                // treat spaces as separators we can be sure we will get it
                // right.
                //
                switch (tt)
                {
                case type::dollar: w += '$'; continue;
                case type::lparen: w += '('; continue;
                }

                // Retire the current word. We need to distinguish between
                // empty and non-existent (e.g., > vs >"").
                //
                if (!w.empty () || f)
                {
                  add_word (move (w), l);
                  f = false;
                }

                if (tt == type::name)
                {
                  w = move (t.value);
                  f = true;
                  continue;
                }

                // If this is one of the operators/separators, check that we
                // don't have any pending locations to be filled.
                //
                check_pending (l);

                // Note: there is another one in the outer loop below.
                //
                switch (tt)
                {
                case type::in_null:
                case type::in_string:
                case type::in_document:
                case type::out_null:
                case type::out_string:
                case type::out_document:
                  parse_redirect (t, l);
                  break;
                }
              }

              // Don't forget the last word.
              //
              if (!w.empty () || f)
                add_word (move (w), l);
            }
          }

          switch (tt)
          {
          case type::equal:
          case type::not_equal:
          case type::newline:
            {
              done = true;
              continue;
            }
          default:
            {
              // Parse the next chunk.
              //
              ns.clear ();
              lexer_->reset_quoted (t.quoted);
              nl = get_location (t);
              parse_names (t, tt, ns, true, "command");
              continue;
            }
          }

          // If this is one of the operators/separators, check that we don't
          // have any pending locations to be filled.
          //
          check_pending (nl);

          // Note: there is another one in the inner loop above.
          //
          switch (tt)
          {
          case type::in_null:
          case type::in_string:
          case type::in_document:
          case type::out_null:
          case type::out_string:
          case type::out_document:
            parse_redirect (t, get_location (t));
            next (t, tt);
            break;
          }
        }

        // Verify we don't have anything pending to be filled.
        //
        check_pending (nl);

        // While we no longer need to recognize command line operators, we
        // also don't expect a valid test trailer to contain them. So we are
        // going to continue lexing in the test_line mode.
        //
        if (tt == type::equal || tt == type::not_equal)
        {
          next (t, tt);
          ts.exit = parse_command_exit (t, tt);
        }

        if (tt != type::newline)
          fail (t) << "unexpected " << t;

        expire_mode (); // Done parsing test-line.

        // Parse here-document fragments in the order they were mentioned on
        // the command line.
        //
        for (redirect& r: hd)
        {
          // Switch to the here-line mode which is like double-quoted but
          // recognized the newline as a separator.
          //
          mode (lexer_mode::here_line);
          next (t, tt);

          // The end marker is temporarily stored as the redirect's value.
          //
          r.value = parse_here_document (t, tt, r.value);

          expire_mode ();
        }

        // Now that we have all the pieces, run the test.
        //
        runner_->run (ts);
      }

      command_exit parser::
      parse_command_exit (token& t, token_type& tt)
      {
        // The next chunk should be the exit status.
        //
        names ns (parse_names (t, tt, true, "exit status"));

        //@@ TODO: validate to be single, simple, non-empty name that
        //         converts to integer (is exit status always non-negative).

        return command_exit {exit_comparison::eq, 0};
      }

      string parser::
      parse_here_document (token& t, token_type& tt, const string& em)
      {
        string r;

        while (tt != type::eos)
        {
          // Check if this is the end marker.
          //
          if (tt == type::name &&
              !t.quoted        &&
              t.value == em    &&
              peek () == type::newline)
          {
            next (t, tt); // Get the newline.
            break;
          }

          // Expand the line.
          //
          names ns (parse_names (t, tt, false, "here-document line"));

          // What shall we do if the expansion results in multiple names? For,
          // example if the line contains just the variable expansion and it
          // is of type strings. Adding all the elements space-separated seems
          // like the natural thing to do.
          //
          for (auto b (ns.begin ()), i (b); i != ns.end (); ++i)
          {
            string s;

            try
            {
              s = value_traits<string>::convert (move (*i), nullptr);
            }
            catch (const invalid_argument&)
            {
              fail (t) << "invalid string value '" << *i << "'";
            }

            if (i != b)
              r += ' ';

            r += s;
            r += '\n'; // Here-document line always includes a newline.
          }

          // We should expand the whole line at once so this would normally be
          // a newline but can also be an end-of-stream.
          //
          if (tt == type::newline)
            next (t, tt);
          else
            assert (tt == type::eos);
        }

        if (tt == type::eos)
          fail (t) << "missing here-document end marker '" << em << "'";

        return r;
      }

      lookup parser::
      lookup_variable (name&& qual, string&& name, const location& l)
      {
        if (!qual.empty ())
          fail (l) << "qualified variable name";

        const variable& var (script_->var_pool.insert (move (name)));
        return script_->find (var);
      }
    }
  }
}
