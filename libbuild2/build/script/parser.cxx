// file      : libbuild2/build/script/parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/parser.hxx>

#include <libbuild2/build/script/lexer.hxx>
#include <libbuild2/build/script/runner.hxx>

using namespace std;

namespace build2
{
  namespace build
  {
    namespace script
    {
      using type = token_type;

      //
      // Pre-parse.
      //

      void parser::
      pre_parse (istream& is, const path_name& pn, uint64_t line, script& s)
      {
        path_ = &pn;

        pre_parse_ = true;

        lexer l (is, *path_, line, lexer_mode::command_line);
        set_lexer (&l);

        script_ = &s;
        runner_ = nullptr;
        environment_ = nullptr;

        script_->start_loc = location (*path_, line, 1);

        token t (pre_parse_script ());

        assert (t.type == type::eos);

        script_->end_loc = get_location (t);
      }

      token parser::
      pre_parse_script ()
      {
        // enter: next token is first token of the script
        // leave: eos (returned)

        token t;
        type tt;

        // Parse lines until we see eos.
        //
        for (;;)
        {
          // Start lexing each line.
          //
          tt = peek (lexer_mode::first_token);

          // Determine the line type by peeking at the first token.
          //
          switch (tt)
          {
          case type::eos:
            {
              next (t, tt);
              return t;
            }
          default:
            {
              pre_parse_line (t, tt);
              assert (tt == type::newline);
              break;
            }
          }
        }
      }

      void parser::
      pre_parse_line (token& t, type& tt, bool if_line)
      {
        // Determine the line type/start token.
        //
        line_type lt (
          pre_parse_line_start (t, tt, lexer_mode::second_token));

        line ln;
        switch (lt)
        {
        case line_type::var:
          {
            // Check if we are trying to modify any of the special variables
            // ($>).
            //
            if (t.value == ">")
              fail (t) << "attempt to set '" << t.value << "' variable";

            // We don't pre-enter variables.
            //
            ln.var = nullptr;

            next (t, tt); // Assignment kind.

            mode (lexer_mode::variable_line);
            parse_variable_line (t, tt);

            if (tt != type::newline)
              fail (t) << "expected newline instead of " << t;

            break;
          }
        case line_type::cmd_elif:
        case line_type::cmd_elifn:
        case line_type::cmd_else:
        case line_type::cmd_end:
          {
            if (!if_line)
            {
              fail (t) << lt << " without preceding 'if'";
            }
          }
          // Fall through.
        case line_type::cmd_if:
        case line_type::cmd_ifn:
          next (t, tt); // Skip to start of command.
          // Fall through.
        case line_type::cmd:
          {
            pair<command_expr, here_docs> p;

            if (lt != line_type::cmd_else && lt != line_type::cmd_end)
              p = parse_command_expr (t, tt);

            if (tt != type::newline)
              fail (t) << "expected newline instead of " << t;

            parse_here_documents (t, tt, p);
            break;
          }
        }

        assert (tt == type::newline);

        ln.type = lt;
        ln.tokens = replay_data ();
        script_->lines.push_back (move (ln));

        if (lt == line_type::cmd_if || lt == line_type::cmd_ifn)
        {
          tt = peek (lexer_mode::first_token);

          pre_parse_if_else (t, tt);
        }
      }

      void parser::
      pre_parse_if_else (token& t, type& tt)
      {
        // enter: peeked first token of next line (type in tt)
        // leave: newline

        // Parse lines until we see closing 'end'. Nested if-else blocks are
        // handled recursively.
        //
        for (line_type bt (line_type::cmd_if); // Current block.
             ;
             tt = peek (lexer_mode::first_token))
        {
          const location ll (get_location (peeked ()));

          if (tt == type::eos)
            fail (ll) << "expected closing 'end'";

          // Parse one line. Note that this one line can still be multiple
          // lines in case of if-else. In this case we want to view it as
          // cmd_if, not cmd_end. Thus remember the start position of the
          // next logical line.
          //
          size_t i (script_->lines.size ());

          pre_parse_line (t, tt, true /* if_line */);
          assert (tt == type::newline);

          line_type lt (script_->lines[i].type);

          // First take care of 'end'.
          //
          if (lt == line_type::cmd_end)
            return;

          // Check if-else block sequencing.
          //
          if (bt == line_type::cmd_else)
          {
            if (lt == line_type::cmd_else ||
                lt == line_type::cmd_elif ||
                lt == line_type::cmd_elifn)
              fail (ll) << lt << " after " << bt;
          }

          // Update current if-else block.
          //
          switch (lt)
          {
          case line_type::cmd_elif:
          case line_type::cmd_elifn: bt = line_type::cmd_elif; break;
          case line_type::cmd_else:  bt = line_type::cmd_else; break;
          default: break;
          }
        }
      }

      command_expr parser::
      parse_command_line (token& t, type& tt)
      {
        // enter: first token of the command line
        // leave: <newline>

        // Note: this one is only used during execution.
        //
        assert (!pre_parse_);

        pair<command_expr, here_docs> p (parse_command_expr (t, tt));
        assert (tt == type::newline);

        parse_here_documents (t, tt, p);
        assert (tt == type::newline);

        return move (p.first);
      }

      //
      // Execute.
      //

      void parser::
      execute (environment& e, runner& r)
      {
        path_ = nullptr; // Set by replays.

        pre_parse_ = false;

        set_lexer (nullptr);

        script_ = nullptr;
        runner_ = &r;
        environment_ = &e;

        exec_script ();
      }

      void parser::
      exec_script ()
      {
        const script& s (environment_->script);

        runner_->enter (*environment_, s.start_loc);

        // Note that we rely on "small function object" optimization for the
        // exec_*() lambdas.
        //
        auto exec_set = [this] (const variable& var,
                                token& t, build2::script::token_type& tt,
                                const location&)
        {
          next (t, tt);
          type kind (tt); // Assignment kind.

          mode (lexer_mode::variable_line);
          value rhs (parse_variable_line (t, tt));

          assert (tt == type::newline);

          // Assign.
          //
          value& lhs (kind == type::assign
                      ? environment_->assign (var)
                      : environment_->append (var));

          apply_value_attributes (&var, lhs, move (rhs), kind);
        };

        auto exec_cmd = [this] (token& t, build2::script::token_type& tt,
                                size_t li,
                                bool single,
                                const location& ll)
        {
          // We use the 0 index to signal that this is the only command.
          //
          if (single)
            li = 0;

          command_expr ce (
            parse_command_line (t, static_cast<token_type&> (tt)));

          runner_->run (*environment_, ce, li, ll);
        };

        auto exec_if = [this] (token& t, build2::script::token_type& tt,
                               size_t li,
                               const location& ll)
        {
          command_expr ce (
            parse_command_line (t, static_cast<token_type&> (tt)));

          // Assume if-else always involves multiple commands.
          //
          return runner_->run_if (*environment_, ce, li, ll);
        };

        size_t li (1);

        exec_lines (s.lines.begin (), s.lines.end (),
                    exec_set, exec_cmd, exec_if,
                    li,
                    &environment_->var_pool);

        runner_->leave (*environment_, s.end_loc);
      }

      lookup parser::
      lookup_variable (name&& qual, string&& name, const location& loc)
      {
        assert (!pre_parse_);

        if (!qual.empty ())
          fail (loc) << "qualified variable name";

        return environment_->lookup (name);
      }
    }
  }
}
