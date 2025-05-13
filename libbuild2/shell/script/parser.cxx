// file      : libbuild2/shell/script/parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/shell/script/parser.hxx>

#include <libbuild2/script/run.hxx>

#include <libbuild2/shell/script/lexer.hxx>
#include <libbuild2/shell/script/runner.hxx>
#include <libbuild2/shell/script/builtin-options.hxx>

using namespace std;

namespace build2
{
  namespace shell
  {
    namespace script
    {
      using type = token_type;

      //
      // Pre-parse.
      //

      script parser::
      pre_parse (const scope& gs, const path& p)
      {
        try
        {
          ifdstream ifs (p);
          return pre_parse (gs, ifs, path_name (p), 1 /* line */);
        }
        catch (const io_error& e)
        {
          fail << "unable to read " << p << ": " << e << endf;
        }
      }

      script parser::
      pre_parse (const scope& gs,
                 istream& is, const path_name& pn, uint64_t line)
      {
        script s;

        path_ = &*s.paths_.insert (path_name_value (pn)).first;

        pre_parse_ = true;

        lexer l (is, *path_, line, lexer_mode::command_line, syntax_);
        set_lexer (&l);

        // The script shouldn't be able to modify the scope.
        //
        root_    = &gs.rw ();
        scope_   = root_;
        target_  = nullptr;

        pbase_ = &work; // Use current working directory.

        script_ = &s;
        runner_ = nullptr;
        environment_ = nullptr;

        s.start_loc = location (*path_, line, 1);

        try_parse_syntax_version ("shellscript.syntax",
                                  lexer_mode::first_token,
                                  2 /* min_syntax */);

        s.syntax = syntax_;

        token t (pre_parse_script ());

        assert (t.type == type::eos);

        s.end_loc = get_location (t);

        return s;
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
          // Start lexing each line recognizing leading '{}'.
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
          case type::lcbrace:
          case type::rcbrace:
            {
              const token& p (peeked ());
              fail (get_location (p)) << "expected command instead of " << p
                                      << endf;
            }
          default:
            {
              pre_parse_line (t, tt);
              break;
            }
          }
        }
      }

      // Parse a logical line, handling the flow control constructs
      // recursively.
      //
      // If the flow control construct type is specified, then it is assumed
      // that this line can control further parsing/execution of such a
      // construct. Note that it should not be specified for the first line of
      // a construct (since its parsing has not yet begun) and for the script
      // command blocks it controls (since such blocks are not terminated with
      // keywords). For example:
      //
      // if true     # nullopt
      // {           # Not parsed as a line.
      //   echo ''   # nullopt
      // }           # Not parsed as a line.
      // elif false  # cmd_if
      //   echo ''   # nullopt
      // else        # cmd_if
      //   echo ''   # nullopt
      //
      void parser::
      pre_parse_line (token& t, type& tt, optional<line_type> fct)
      {
        // enter: next token is peeked at (type in tt)
        // leave: newline

        assert (!fct || *fct == line_type::cmd_if);

        // Determine the line type/start token.
        //
        line_type lt (pre_parse_line_start (t, tt, lexer_mode::second_token));

        line ln;

        switch (lt)
        {
        case line_type::var:
          {
            // Check if we are trying to modify any of the special variables.
            //
            verify_variable_assignment (t.value, get_location (t));

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
        case line_type::cmd_elif_null:
        case line_type::cmd_elifn_null:
        case line_type::cmd_elif_empty:
        case line_type::cmd_elifn_empty:
          {
            if (!fct || *fct != line_type::cmd_if)
              fail (t) << lt << " without preceding 'if'";
          }
          // Fall through.
        case line_type::cmd_if_null:
        case line_type::cmd_ifn_null:
        case line_type::cmd_if_empty:
        case line_type::cmd_ifn_empty:
          {
            type ft;
            mode (lexer_mode::variable_line);
            parse_variable_line (t, tt, &ft);

            if (ft == type::newline)
              fail (t) << "expected value after " << lt;

            if (tt != type::newline)
              fail (t) << "expected newline instead of " << t;

            break;
          }
          //
          // See pre_parse_line_start() for details.
          //
        case line_type::cmd_for_args: assert (false); break;
        case line_type::cmd_for_stream:
          {
            // First we need to sense the next few tokens and detect which
            // form of the for-loop that actually is (see
            // libbuild2/build/script/parser.cxx for details).
            //
            token pt (t);
            assert (pt.type == type::word && pt.value == "for");

            mode (lexer_mode::for_loop);
            next (t, tt);

            // Note that we also consider special variable names (those that
            // don't clash with the command line elements like redirects, etc)
            // to later fail gracefully.
            //
            string& n (t.value);

            // @@ If we add support for the temporary directory special
            //    variable, then check for it here as well.
            //
            if (tt == type::word && t.qtype == quote_type::unquoted &&
                (n[0] == '_' || alpha (n[0]) || // Variable.
                 n == "*"    || n == "~"))      // Special variable.
            {
              // Detect patterns analogous to parse_variable_name() (so we
              // diagnose `for x[string]: ...`).
              //
              if (n.find_first_of ("[*?") != string::npos)
                fail (t) << "expected variable name instead of " << n;

              verify_variable_assignment (n, get_location (t));

              if (lexer_->peek_char ().first == '[')
              {
                token vt (move (t));
                next_with_attributes (t, tt);

                attributes_push (t, tt,
                                 true /* standalone */,
                                 false /* next_token */);

                t = move (vt);
                tt = t.type;
              }

              if (lexer_->peek_char ().first == ':')
                lt = line_type::cmd_for_args;
            }

            if (lt == line_type::cmd_for_stream) // for x <...
            {
              expire_mode ();

              parse_command_expr_result r (
                parse_command_expr (t, tt,
                                    lexer::redirect_aliases,
                                    move (pt)));

              assert (r.for_loop);

              if (tt != type::newline)
                fail (t) << "expected newline instead of " << t;

              parse_here_documents (t, tt, r);
            }
            else                                 // for x: ...
            {
              next (t, tt);

              assert (tt == type::colon);

              expire_mode ();

              // Parse the value similar to the var line type (see above).
              //
              mode (lexer_mode::variable_line);
              parse_variable_line (t, tt);

              if (tt != type::newline)
                fail (t) << "expected newline instead of " << t
                         << " after 'for'";
            }

            ln.var = nullptr;
            break;
          }
        case line_type::cmd_elif:
        case line_type::cmd_elifn:
        case line_type::cmd_else:
          {
            if (!fct || *fct != line_type::cmd_if)
              fail (t) << lt << " without preceding 'if'";
          }
          // Fall through.
        case line_type::cmd_if:
        case line_type::cmd_ifn:
        case line_type::cmd_while:
          next (t, tt); // Skip to start of command.
          // Fall through.
        case line_type::cmd:
          {
            parse_command_expr_result r;

            if (lt != line_type::cmd_else)
              r = parse_command_expr (t, tt, lexer::redirect_aliases);

            if (r.for_loop)
            {
              lt     = line_type::cmd_for_stream;
              ln.var = nullptr;
            }

            if (tt != type::newline)
              fail (t) << "expected newline instead of " << t;

            parse_here_documents (t, tt, r);

            break;
          }
        case line_type::cmd_end:
          assert (false); // Not recognized as a keyword.
        }

        assert (tt == type::newline);

        ln.type = lt;
        ln.tokens = replay_data ();

        script_->body.push_back (move (ln));

        switch (lt)
        {
        case line_type::cmd_if:
        case line_type::cmd_ifn:
        case line_type::cmd_if_null:
        case line_type::cmd_ifn_null:
        case line_type::cmd_if_empty:
        case line_type::cmd_ifn_empty:
          {
            tt = peek (lexer_mode::first_token);

            pre_parse_if_else (t, tt);
            break;
          }
        case line_type::cmd_while:
        case line_type::cmd_for_stream:
        case line_type::cmd_for_args:
          {
            tt = peek (lexer_mode::first_token);

            pre_parse_loop (t, tt);
            break;
          }
        default: break;
        }

        assert (tt == type::newline);
      }

      // Pre-parse the flow control construct block line.
      //
      void parser::
      pre_parse_block_line (token& t, type& tt)
      {
        // enter: peeked first token of the line (type in tt)
        // leave: newline

        switch (tt)
        {
        case type::eos:
        case type::lcbrace:
        case type::rcbrace:
          {
            const token& p (peeked ());
            fail (get_location (p)) << "expected command instead of " << p;
          }
        }

        pre_parse_line (t, tt);
      }

      void parser::
      pre_parse_block (token& t, type& tt)
      {
        // enter: peeked first token of the line (lcbrace)
        // leave: newline after rcbrace

        next (t, tt); // Get '{'.

        if (next (t, tt) != type::newline)
          fail (t) << "expected newline after '{'";

        // Parse block lines until we see '}'.
        //
        for (;;)
        {
          // Start lexing each line recognizing leading '{}'.
          //
          tt = peek (lexer_mode::first_token);

          if (tt == type::rcbrace)
            break;

          pre_parse_block_line (t, tt);
        }

        next (t, tt); // Get '}'.

        if (next (t, tt) != type::newline)
          fail (t) << "expected newline after '}'";
      }

      void parser::
      pre_parse_if_else (token& t, type& tt)
      {
        // enter: peeked first token of next line (type in tt)
        // leave: newline

        // Parse the if-else block chain.
        //
        for (line_type bt (line_type::cmd_if); // Current block.
             ;
             tt = peek (lexer_mode::first_token))
        {
          if (tt == type::lcbrace)
            pre_parse_block (t, tt);
          else
            pre_parse_block_line (t, tt);

          // See if what comes next is another chain element.
          //
          line_type lt (line_type::cmd_end);
          type pt (peek (lexer_mode::first_token));
          const token& p (peeked ());

          if (pt == type::word && p.qtype == quote_type::unquoted)
          {
            if      (p.value == "elif")   lt = line_type::cmd_elif;
            else if (p.value == "elif!")  lt = line_type::cmd_elifn;
            else if (p.value == "elifn")  lt = line_type::cmd_elif_null;
            else if (p.value == "elifn!") lt = line_type::cmd_elifn_null;
            else if (p.value == "elife")  lt = line_type::cmd_elif_empty;
            else if (p.value == "elife!") lt = line_type::cmd_elifn_empty;
            else if (p.value == "else")   lt = line_type::cmd_else;
          }

          // Bail out if we reached the end of the if-construct.
          //
          if (lt == line_type::cmd_end)
            break;

          // Check if-else block sequencing.
          //
          if (bt == line_type::cmd_else)
            fail (p) << lt << " after " << bt;

          pre_parse_line (t, (tt = pt), line_type::cmd_if);

          // Can either be '{' or the first token of the command line.
          //
          tt = peek (lexer_mode::first_token);

          // Update current if-else block.
          //
          switch (lt)
          {
          case line_type::cmd_elif:
          case line_type::cmd_elifn:
          case line_type::cmd_elif_null:
          case line_type::cmd_elifn_null:
          case line_type::cmd_elif_empty:
          case line_type::cmd_elifn_empty: bt = line_type::cmd_elif; break;
          case line_type::cmd_else:        bt = line_type::cmd_else; break;
          default: break;
          }
        }

        // Terminate the construct with the special `end` line.
        //
        script_->body.push_back (end_line);
      }

      void parser::
      pre_parse_loop (token& t, type& tt)
      {
        // enter: peeked first token of next line (type in tt)
        // leave: newline

        if (tt == type::lcbrace)
          pre_parse_block (t, tt);
        else
          pre_parse_block_line (t, tt);

        // Terminate the construct with the special `end` line.
        //
        script_->body.push_back (end_line);
      }

      command_expr parser::
      parse_command_line (token& t, type& tt)
      {
        // enter: first token of the command line
        // leave: <newline>

        // Note: this one is only used during execution.
        //
        assert (!pre_parse_);

        parse_command_expr_result pr (
          parse_command_expr (t, tt, lexer::redirect_aliases));

        assert (tt == type::newline);

        parse_here_documents (t, tt, pr);
        assert (tt == type::newline);

        return move (pr.expr);
      }

      //
      // Execute.
      //

      int parser::
      execute (environment& e, const script& s, runner& r)
      {
        path_ = nullptr; // Set by replays.

        pre_parse_ = false;

        set_lexer (nullptr);

        // The script shouldn't be able to modify the scope.
        //
        root_    = &e.scope.rw ();
        scope_   = root_;
        target_  = nullptr;

        pbase_ = &work; // Use current working directory.

        script_ = const_cast<script*> (&s);
        runner_ = &r;
        environment_ = &e;

        runner_->enter (e, s.start_loc);

        // Note that we rely on "small function object" optimization here.
        //
        auto exec_cmd = [this] (token& t, build2::script::token_type& tt,
                                const iteration_index* ii, size_t li,
                                bool single,
                                const function<command_function>& cf,
                                const location& ll)
        {
          // We use the 0 index to signal that this is the only command.
          //
          if (single)
            li = 0;

          command_expr ce (
            parse_command_line (t, static_cast<token_type&> (tt)));

          runner_->run (*environment_, ce, ii, li, cf, ll);
        };

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

        auto exec_cond = [this] (token& t, build2::script::token_type& tt,
                                 const iteration_index* ii, size_t li,
                                 const location& ll)
        {
          command_expr ce (
            parse_command_line (t, static_cast<token_type&> (tt)));

          // Assume a flow control construct always involves multiple
          // commands.
          //
          return runner_->run_cond (*environment_, ce, ii, li, ll);
        };

        auto exec_for = [this] (const variable& var,
                                value&& val,
                                const attributes& val_attrs,
                                const location&)
        {
          value& lhs (environment_->assign (var));

          attributes_.push_back (val_attrs);

          apply_value_attributes (&var, lhs, move (val), type::assign);
        };

        optional<uint8_t> ec (
          exec_lines (
            s.body.begin (), s.body.end (),
            exec_set, exec_cmd, exec_cond, exec_for,
            nullptr /* iteration_index */,
            environment_->exec_line,
            false /* throw_on_failure */,
            &environment_->var_pool));

        runner_->leave (e, s.end_loc);
        return ec ? *ec : 0;
      }

      // When add a special variable don't forget to update lexer::word() and
      // for-loop parsing in pre_parse_line().
      //
      // @@ If we add support for the temporary directory special variable,
      //    then check for it here as well.
      //
      bool parser::
      special_variable (const string& n) noexcept
      {
        return n == "*" || (n.size () == 1 && digit (n[0])) || n == "~";
      }

      void parser::
      verify_variable_assignment (const string& name, const location& loc)
      {
        if (special_variable (name))
          build2::fail (loc) << "attempt to set '" << name
                             << "' special variable";

        if (name == "shellscript.syntax")
          build2::fail (loc) << "variable shellscript.syntax can only be "
                             << "assigned to on the first line of the script";
      }

      lookup parser::
      lookup_variable (names&& qual, string&& name, const location& loc)
      {
        if (pre_parse_)
          return lookup ();

        if (!qual.empty ())
          fail (loc) << "qualified variable name";

        return environment_->lookup (name);
      }
    }
  }
}
