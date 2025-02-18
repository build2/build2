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

        lexer l (is, *path_, line, lexer_mode::command_line);
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

      // Parse a logical line, handling the flow control constructs
      // recursively.
      //
      // If the flow control construct type is specified, then this line is
      // assumed to belong to such a construct.
      //
      void parser::
      pre_parse_line (token& t, type& tt, optional<line_type> fct)
      {
        // enter: next token is peeked at (type in tt)
        // leave: newline

        assert (!fct                              ||
                *fct == line_type::cmd_if         ||
                *fct == line_type::cmd_while      ||
                *fct == line_type::cmd_for_stream ||
                *fct == line_type::cmd_for_args);

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
            if (special_variable (t.value))
              fail (t) << "attempt to set '" << t.value << "' special "
                       << "variable";

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

            if (tt == type::word && t.qtype == quote_type::unquoted &&
                (n[0] == '_' || alpha (n[0]) || // Variable.
                 n == "*" /* || n == "~" */))   // Special variable.
            {
              // Detect patterns analogous to parse_variable_name() (so we
              // diagnose `for x[string]: ...`).
              //
              if (n.find_first_of ("[*?") != string::npos)
                fail (t) << "expected variable name instead of " << n;

              if (special_variable (n))
                fail (t) << "attempt to set '" << n << "' special variable";

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
                fail (t) << "expected newline instead of " << t << " after for";
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
        case line_type::cmd_end:
          {
            if (!fct)
              fail (t) << lt << " without preceding 'if', 'for', or 'while'";
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

            if (lt != line_type::cmd_else && lt != line_type::cmd_end)
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
        }

        assert (tt == type::newline);

        ln.type = lt;
        ln.tokens = replay_data ();

        script_->body.push_back (move (ln));

        switch (lt)
        {
        case line_type::cmd_if:
        case line_type::cmd_ifn:
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

            pre_parse_loop (t, tt, lt);
            break;
          }
        default: break;
        }
      }

      // Pre-parse the flow control construct block line.
      //
      void parser::
      pre_parse_block_line (token& t, type& tt, line_type bt)
      {
        // enter: peeked first token of the line (type in tt)
        // leave: newline

        const location ll (get_location (peeked ()));

        if (tt == type::eos)
          fail (ll) << "expected closing 'end'";

        line_type fct; // Flow control type the block type relates to.

        switch (bt)
        {
        case line_type::cmd_if:
        case line_type::cmd_ifn:
        case line_type::cmd_elif:
        case line_type::cmd_elifn:
        case line_type::cmd_else:
          {
            fct = line_type::cmd_if;
            break;
          }
        case line_type::cmd_while:
        case line_type::cmd_for_stream:
        case line_type::cmd_for_args:
          {
            fct = bt;
            break;
          }
        default: assert(false);
        }

        pre_parse_line (t, tt, fct);
        assert (tt == type::newline);
      }

      void parser::
      pre_parse_if_else (token& t, type& tt)
      {
        // enter: peeked first token of next line (type in tt)
        // leave: newline

        // Parse lines until we see closing 'end'.
        //
        for (line_type bt (line_type::cmd_if); // Current block.
             ;
             tt = peek (lexer_mode::first_token))
        {
          const location ll (get_location (peeked ()));

          // Parse one line. Note that this one line can still be multiple
          // lines in case of a flow control construct. In this case we want
          // to view it as cmd_if, not cmd_end. Thus remember the start
          // position of the next logical line.
          //
          size_t i (script_->body.size ());

          pre_parse_block_line (t, tt, bt);

          line_type lt (script_->body[i].type);

          // First take care of 'end'.
          //
          if (lt == line_type::cmd_end)
            break;

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

      void parser::
      pre_parse_loop (token& t, type& tt, line_type lt)
      {
        // enter: peeked first token of next line (type in tt)
        // leave: newline

        assert (lt == line_type::cmd_while      ||
                lt == line_type::cmd_for_stream ||
                lt == line_type::cmd_for_args);

        // Parse lines until we see closing 'end'.
        //
        for (;; tt = peek (lexer_mode::first_token))
        {
          size_t i (script_->body.size ());

          pre_parse_block_line (t, tt, lt);

          if (script_->body[i].type == line_type::cmd_end)
            break;
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
      bool parser::
      special_variable (const string& n) noexcept
      {
        return n == "*" || (n.size () == 1 && digit (n[0])) /*|| n == "~"*/;
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
