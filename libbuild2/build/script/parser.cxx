// file      : libbuild2/build/script/parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/parser.hxx>

#include <libbutl/builtin.mxx>

#include <libbuild2/algorithm.hxx>

#include <libbuild2/build/script/lexer.hxx>
#include <libbuild2/build/script/runner.hxx>

using namespace std;
using namespace butl;

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

      script parser::
      pre_parse (const target& tg,
                 istream& is, const path_name& pn, uint64_t line,
                 optional<string> diag, const location& diag_loc)
      {
        path_ = &pn;

        pre_parse_ = true;

        lexer l (is, *path_, line, lexer_mode::command_line);
        set_lexer (&l);

        // The script shouldn't be able to modify the target/scopes.
        //
        target_ = const_cast<target*> (&tg);
        scope_  = const_cast<scope*> (&tg.base_scope ());
        root_   = scope_->root_scope ();

        pbase_  = scope_->src_path_;

        script s;
        script_ = &s;
        runner_ = nullptr;
        environment_ = nullptr;

        if (diag)
        {
          diag_name_   = make_pair (move (*diag), diag_loc);
          diag_weight_ = 4;
        }

        s.start_loc = location (*path_, line, 1);

        token t (pre_parse_script ());

        assert (t.type == type::eos);

        s.end_loc = get_location (t);

        // Diagnose absent/ambigous script name.
        //
        {
          diag_record dr;

          if (!diag_name_ && !diag_line_)
          {
            dr << fail (s.start_loc)
               << "unable to deduce low-verbosity script diagnostics name";
          }
          else if (diag_name2_)
          {
            assert (diag_name_);

            dr << fail (s.start_loc)
               << "low-verbosity script diagnostics name is ambiguous" <<
              info (diag_name_->second) << "could be '" << diag_name_->first
               << "'" <<
              info (diag_name2_->second) << "could be '" << diag_name2_->first
               << "'";
          }

          if (!dr.empty ())
          {
            dr << info << "consider specifying it explicitly with the 'diag' "
               << "recipe attribute";

            dr << info << "or provide custom low-verbosity diagnostics with "
               << "the 'diag' builtin";
          }
        }

        // Save the script name or custom diagnostics line.
        //
        assert (diag_name_.has_value () != diag_line_.has_value ());

        if (diag_name_)
          s.diag_name = move (diag_name_->first);
        else
          s.diag_line = move (diag_line_->first);

        // Save the custom dependency change tracking lines, if present.
        //
        s.depdb_clear = depdb_clear_.has_value ();
        s.depdb_lines = move (depdb_lines_);

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

      void parser::
      pre_parse_line (token& t, type& tt, bool if_line)
      {
        // Determine the line type/start token.
        //
        line_type lt (
          pre_parse_line_start (t, tt, lexer_mode::second_token));

        line ln;

        // Indicates that the parsed line should by default be appended to the
        // script.
        //
        save_line_ = &ln;

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
              p = parse_command_expr (t, tt, lexer::redirect_aliases);

            if (tt != type::newline)
              fail (t) << "expected newline instead of " << t;

            parse_here_documents (t, tt, p);
            break;
          }
        }

        assert (tt == type::newline);

        ln.type = lt;
        ln.tokens = replay_data ();

        if (save_line_ != nullptr)
        {
          if (save_line_ == &ln)
            script_->lines.push_back (move (ln));
          else
            *save_line_ = move (ln);
        }

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

        pair<command_expr, here_docs> p (
          parse_command_expr (t, tt, lexer::redirect_aliases));

        assert (tt == type::newline);

        parse_here_documents (t, tt, p);
        assert (tt == type::newline);

        return move (p.first);
      }

      //
      // Execute.
      //

      optional<process_path> parser::
      parse_program (token& t, build2::script::token_type& tt,
                     bool first,
                     names& ns)
      {
        const location l (get_location (t));

        // Set the current script name if it is not set or its weight is less
        // than the new name weight, skipping names with the zero weight. If
        // the weight is the same but the name is different then record this
        // ambiguity, unless one is already recorded. This ambiguity will be
        // reported at the end of the script pre-parsing, unless discarded by
        // the name with a greater weight.
        //
        auto set_diag = [&l, this] (string d, uint8_t w)
        {
          if (diag_weight_ < w)
          {
            diag_name_   = make_pair (move (d), l);
            diag_weight_ = w;
            diag_name2_  = nullopt;
          }
          else if (w != 0                 &&
                   w == diag_weight_      &&
                   d != diag_name_->first &&
                   !diag_name2_)
            diag_name2_ = make_pair (move (d), l);
        };

        // Handle special builtins.
        //
        // NOTE: update line dumping (script.cxx:dump()) if adding a special
        // builtin. Also review the non-script-local variables tracking while
        // executing a single line in lookup_variable().
        //
        if (pre_parse_ && first && tt == type::word)
        {
          const string& v (t.value);

          if (v == "diag")
          {
            // Check for ambiguity.
            //
            if (diag_weight_ == 4)
            {
              if (diag_name_) // Script name.
              {
                fail (l) << "both low-verbosity script diagnostics name "
                         << "and 'diag' builtin call" <<
                  info (diag_name_->second) << "script name specified here";
              }
              else           // Custom diagnostics.
              {
                assert (diag_line_);

                fail (l) << "multiple 'diag' builtin calls" <<
                  info (diag_line_->second) << "previous call is here";
              }
            }

            // Instruct the parser to save the diag builtin line separately
            // from the script lines, when it is fully parsed. Note that it
            // will be executed prior to the script execution to obtain the
            // custom diagnostics.
            //
            diag_line_   = make_pair (line (), l);
            save_line_   = &diag_line_->first;
            diag_weight_ = 4;

            diag_name_  = nullopt;
            diag_name2_ = nullopt;

            // Note that the rest of the line contains the builtin argument to
            // be printed, thus we parse it in the value lexer mode.
            //
            mode (lexer_mode::value);
            parse_names (t, tt, pattern_mode::ignore);
            return nullopt;
          }
          else if (v == "depdb")
          {
            // Note that the rest of the line contains the builtin command
            // name, potentially followed by the arguments to be
            // hashed/saved. Thus, we parse it in the value lexer mode.
            //
            mode (lexer_mode::value);

            // Obtain and validate the depdb builtin command name.
            //
            next (t, tt);

            if (tt != type::word ||
                (v != "clear" && v != "hash" && v != "string"))
            {
              fail (get_location (t))
                << "expected 'depdb' builtin command instead of " << t;
            }

            if (v == "clear")
            {
              // Make sure the clear depdb command comes first.
              //
              if (depdb_clear_)
                fail (l) << "multiple 'depdb clear' builtin calls" <<
                  info (*depdb_clear_) << "previous call is here";

              if (!depdb_lines_.empty ())
                fail (l) << "'depdb clear' should be the first 'depdb' "
                         << "builtin call" <<
                  info (depdb_lines_[0].tokens[0].location ())
                         << "first 'depdb' call is here";

              // Save the builtin location, cancel the line saving, and clear
              // the referenced variable list, since it won't be used.
              //
              depdb_clear_ = l;
              save_line_   = nullptr;

              script_->vars.clear ();
            }
            else
            {
              // Instruct the parser to save the depdb builtin line separately
              // from the script lines, when it is fully parsed. Note that the
              // builtin command arguments will be validated during execution,
              // when expanded.
              //
              depdb_lines_.push_back (line ());
              save_line_ = &depdb_lines_.back ();
            }

            // Parse the rest of the line and bail out.
            //
            parse_names (t, tt, pattern_mode::ignore);
            return nullopt;
          }
        }

        auto suggest_diag = [this] (const diag_record& dr)
        {
          dr << info << "consider specifying it explicitly with "
             << "the 'diag' recipe attribute";

          dr << info << "or provide custom low-verbosity diagnostics "
             << "with the 'diag' builtin";
        };

        parse_names_result pr;
        {
          // During pre-parse, if the script name is not set manually, we
          // suspend pre-parse, parse the command names for real and try to
          // deduce the script name from the result. Otherwise, we continue to
          // pre-parse and bail out after parsing the names.
          //
          // Note that the later is not just an optimization since expansion
          // that wouldn't fail during execution may fail in this special
          // mode, for example:
          //
          // ...
          // {{
          //    x = true
          //    ba($x ? r : z)
          // }}
          //
          // v = a b
          // ...
          // {{
          //    v = o
          //    fo$v
          // }}
          //
          // This is also the reason why we add a diag frame.
          //
          if (pre_parse_ && diag_weight_ != 4)
          {
            pre_parse_ = false; // Make parse_names() perform expansions.
            pre_parse_suspended_ = true;
          }

          auto df = make_diag_frame (
            [&l, &suggest_diag, this] (const diag_record& dr)
            {
              if (pre_parse_suspended_)
              {
                dr << info (l) << "while deducing low-verbosity script "
                   << "diagnostics name";

                suggest_diag (dr);
              }
            });

          pr = parse_names (t, tt,
                            ns,
                            pattern_mode::ignore,
                            true /* chunk */,
                            "command line",
                            nullptr);

          if (pre_parse_suspended_)
          {
            pre_parse_suspended_ = false;
            pre_parse_ = true;
          }

          if (pre_parse_ && diag_weight_ == 4)
            return nullopt;
        }

        // Try to translate names into a process path, unless there is nothing
        // to translate.
        //
        // We only end up here in the pre-parse mode if we are still searching
        // for the script name.
        //
        if (!pr.not_null || ns.empty ())
        {
          if (pre_parse_)
          {
            diag_record dr (fail (l));
            dr << "unable to deduce low-verbosity script diagnostics name";
            suggest_diag (dr);
          }

          return nullopt;
        }

        // We have to handle process_path[_ex] and executable target. The
        // process_path[_ex] we may have to recognize syntactically because
        // of the loss of type, for example:
        //
        // c = $cxx.path --version
        //
        // {{
        //    $c ...
        // }}
        //
        // This is further complicated by the fact that the first name in
        // process_path[_ex] may or may not be a pair (it's not a pair if
        // recall and effective paths are the same). If it's not a pair and we
        // are dealing with process_path, then we don't need to do anything
        // extra -- it will just be treated as normal program path. However,
        // if it's process_path_ex, then we may end up with something along
        // these lines:
        //
        // /usr/bin/g++ name@c++ checksum@deadbeef --version
        //
        // Which is a bit harder to recognize syntactically. So what we are
        // going to do is have a separate first pass which reduces the
        // syntactic cases to the typed ones.
        //
        names pp_ns;
        if (pr.type == &value_traits<process_path>::value_type ||
            pr.type == &value_traits<process_path_ex>::value_type)
        {
          pp_ns = move (ns);
          ns.clear ();
        }
        else if (ns[0].file ())
        {
          // Find the end of the value.
          //
          auto b (ns.begin ());
          auto i (value_traits<process_path_ex>::find_end (ns));

          if (b->pair || i != b + 1) // First is a pair or pairs after.
          {
            pp_ns = names (make_move_iterator (b), make_move_iterator (i));

            ns.erase (b, i);

            pr.type = i != b + 1
                      ? &value_traits<process_path_ex>::value_type
                      : &value_traits<process_path>::value_type;
          }
        }

        // Handle process_path[_ex], for example:
        //
        // {{
        //    $cxx.path ...
        // }}
        //
        if (pr.type == &value_traits<process_path>::value_type)
        {
          auto pp (convert<process_path> (move (pp_ns)));

          if (pre_parse_)
          {
            diag_record dr (fail (l));
            dr << "unable to deduce low-verbosity script diagnostics name "
               << "from process path " << pp;
            suggest_diag (dr);
          }
          else
            return optional<process_path> (move (pp));
        }
        else if (pr.type == &value_traits<process_path_ex>::value_type)
        {
          auto pp (convert<process_path_ex> (move (pp_ns)));

          if (pre_parse_)
          {
            if (pp.name)
            {
              set_diag (move (*pp.name), 3);
              return nullopt;
            }

            diag_record dr (fail (l));
            dr << "unable to deduce low-verbosity script diagnostics name "
               << "from process path " << pp;
            suggest_diag (dr);
          }
          else
            return optional<process_path> (move (pp));
        }
        //
        // Handle the executable target, for example:
        //
        // import! [metadata] cli = cli%exe{cli}
        // ...
        // {{
        //    $cli ...
        // }}
        //
        else if (!ns[0].simple ())
        {
          if (const target* t = search_existing (
                ns[0], *scope_, ns[0].pair ? ns[1].dir : empty_dir_path))
          {
            if (const auto* et = t->is_a<exe> ())
            {
              if (pre_parse_)
              {
                if (auto* n = et->lookup_metadata<string> ("name"))
                {
                  set_diag (*n, 3);
                  return nullopt;
                }
                // Fall through.
              }
              else
              {
                process_path pp (et->process_path ());

                if (pp.empty ())
                  fail (l) << "target " << *et << " is out of date" <<
                    info << "consider specifying it as a prerequisite of "
                           << environment_->target;

                ns.erase (ns.begin (), ns.begin () + (ns[0].pair ? 2 : 1));
                return optional<process_path> (move (pp));
              }
            }

            if (pre_parse_)
            {
              diag_record dr (fail (l));
              dr << "unable to deduce low-verbosity script diagnostics name "
                 << "from target " << *t;
              suggest_diag (dr);
            }
          }

          if (pre_parse_)
          {
            diag_record dr (fail (l));
            dr << "unable to deduce low-verbosity script diagnostics name "
               << "from " << ns;
            suggest_diag (dr);
          }
          else
            return nullopt;
        }
        else if (pre_parse_)
        {
          // If we are here, the name is simple and is not part of a pair.
          //
          string& v (ns[0].value);

          // Try to interpret the name as a builtin.
          //
          const builtin_info* bi (builtins.find (v));

          if (bi != nullptr)
          {
            set_diag (move (v), bi->weight);
            return nullopt;
          }
          //
          // Try to interpret the name as a pseudo-builtin.
          //
          // Note that both of them has the zero weight and cannot be picked
          // up as a script name.
          //
          else if (v == "set" || v == "exit")
          {
            return nullopt;
          }

          diag_record dr (fail (l));
          dr << "unable to deduce low-verbosity script diagnostics name "
             << "for program " << ns[0];
          suggest_diag (dr);
        }

        return nullopt;
      }

      void parser::
      execute (const scope& rs, const scope& bs,
               environment& e, const script& s, runner& r)
      {
        path_ = nullptr; // Set by replays.

        pre_parse_ = false;

        set_lexer (nullptr);

        // The script shouldn't be able to modify the scopes.
        //
        // Note that for now we don't set target_ since it's not clear what
        // it could be used for (we need scope_ for calling functions such as
        // $target.path()).
        //
        root_ = const_cast<scope*> (&rs);
        scope_ = const_cast<scope*> (&bs);
        pbase_ = scope_->src_path_;

        script_ = const_cast<script*> (&s);
        runner_ = &r;
        environment_ = &e;

        exec_script ();
      }

      void parser::
      exec_script ()
      {
        const script& s (*script_);

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

      names parser::
      execute_special (const scope& rs, const scope& bs,
                       environment& e,
                       const line& ln,
                       bool omit_builtin)
      {
        path_ = nullptr; // Set by replays.

        pre_parse_ = false;

        set_lexer (nullptr);

        // The script shouldn't be able to modify the scopes.
        //
        // Note that for now we don't set target_ since it's not clear what
        // it could be used for (we need scope_ for calling functions such as
        // $target.path()).
        //
        root_ = const_cast<scope*> (&rs);
        scope_ = const_cast<scope*> (&bs);
        pbase_ = scope_->src_path_;

        script_ = nullptr;
        runner_ = nullptr;
        environment_ = &e;

        // Copy the tokens and start playing.
        //
        replay_data (replay_tokens (ln.tokens));

        token t;
        build2::script::token_type tt;
        next (t, tt);

        if (omit_builtin)
        {
          assert (tt != type::newline && tt != type::eos);

          next (t, tt);
        }

        names r (tt != type::newline && tt != type::eos
                 ? parse_names (t, tt, pattern_mode::expand)
                 : names ());

        replay_stop ();
        return r;
      }

      // When add a special variable don't forget to update lexer::word().
      //
      bool parser::
      special_variable (const string& n) noexcept
      {
        return n == ">" || n == "<" || n == "~";
      }

      lookup parser::
      lookup_variable (name&& qual, string&& name, const location& loc)
      {
        // In the pre-parse mode collect the referenced variable names for the
        // script semantics change tracking.
        //
        if (pre_parse_ || pre_parse_suspended_)
        {
          lookup r;

          // Add the variable name skipping special variables and suppressing
          // duplicates, unless the default variables change tracking is
          // canceled with `depdb clear`. While at it, check if the script
          // temporary directory is referenced and set the flag, if that's the
          // case.
          //
          if (special_variable (name))
          {
            if (name == "~")
              script_->temp_dir = true;
          }
          else if (!name.empty ())
          {
            if (pre_parse_suspended_)
            {
              const variable* pvar (scope_->ctx.var_pool.find (name));

              if (pvar != nullptr)
                r = (*target_)[*pvar];
            }

            if (!depdb_clear_)
            {
              auto& vars (script_->vars);

              if (find (vars.begin (), vars.end (), name) == vars.end ())
                vars.push_back (move (name));
            }
          }

          return r;
        }

        if (!qual.empty ())
          fail (loc) << "qualified variable name";

        lookup r (environment_->lookup (name));

        // Fail if non-script-local variable with an untracked name.
        //
        // Note that we don't check for untracked variables when executing a
        // single line with execute_special() (script_ is NULL), since the
        // diag builtin argument change (which can be affected by such a
        // variable expansion) doesn't affect the script semantics and the
        // depdb argument is specifically used for the script semantics change
        // tracking. We also omit this check it the depdb builtin is used in
        // the script, assuming that such variables are tracked manually, if
        // required.
        //
        if (script_ != nullptr    &&
            !script_->depdb_clear &&
            !script_->depdb_lines.empty ())
        {
          if (r.defined () && !r.belongs (*environment_))
          {
            const auto& vars (script_->vars);

            if (find (vars.begin (), vars.end (), name) == vars.end ())
              fail (loc) << "use of untracked variable '" << name << "'" <<
                info << "use the 'depdb' builtin to manually track it";
          }
        }

        return r;
      }
    }
  }
}
