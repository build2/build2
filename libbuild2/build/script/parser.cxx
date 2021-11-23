// file      : libbuild2/build/script/parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/parser.hxx>

#include <cstring> // strcmp()
#include <sstream>

#include <libbutl/builtin.hxx>

#include <libbuild2/depdb.hxx>
#include <libbuild2/dyndep.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/make-parser.hxx>

#include <libbuild2/script/run.hxx>

#include <libbuild2/build/script/lexer.hxx>
#include <libbuild2/build/script/runner.hxx>
#include <libbuild2/build/script/builtin-options.hxx>

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
      pre_parse (const scope& bs,
                 const target_type& tt,
                 const small_vector<action, 1>& as,
                 istream& is, const path_name& pn, uint64_t line,
                 optional<string> diag, const location& diag_loc)
      {
        path_ = &pn;

        pre_parse_ = true;

        lexer l (is, *path_, line, lexer_mode::command_line);
        set_lexer (&l);

        // The script shouldn't be able to modify the scopes.
        //
        target_  = nullptr;
        actions_ = &as;
        scope_   = const_cast<scope*> (&bs);
        root_    = scope_->root_scope ();

        pbase_  = scope_->src_path_;

        file_based_ = tt.is_a<file> ();
        perform_update_ = find (as.begin (), as.end (), perform_update_id) !=
                          as.end ();

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

        // Diagnose impure function calls.
        //
        if (impure_func_)
          fail (impure_func_->second)
            << "call to impure function " << impure_func_->first << " is "
            << "only allowed in depdb preamble" <<
            info << "consider using 'depdb' builtin to track its result "
                 << "changes";

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
        if (depdb_dyndep_)
          s.depdb_dyndep = depdb_dyndep_->second;
        s.depdb_preamble = move (depdb_preamble_);

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

          if (lt == line_type::cmd_if || lt == line_type::cmd_ifn)
            ++level_;
          else if (lt == line_type::cmd_end)
            --level_;

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
            script_->body.push_back (move (ln));
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
          size_t i (script_->body.size ());

          pre_parse_line (t, tt, true /* if_line */);
          assert (tt == type::newline);

          line_type lt (script_->body[i].type);

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

        // @@ Note that currently running programs via a runner (e.g., see
        //    test.runner) needs to be handled explicitly in ad hoc recipes.
        //    We could potentially run them via the runner implicitly, similar
        //    to how we do in the testscript. We would need then to match the
        //    command program path against the recipe target ad hoc member
        //    paths (test programs), to detect if it must be run via the
        //    runner. The runner path/options would need to be optionally
        //    passed to the environment constructor, similar to passing the
        //    script deadline.
        //
        return move (p.first);
      }

      //
      // Execute.
      //

      optional<process_path> parser::
      parse_program (token& t, build2::script::token_type& tt,
                     bool first, bool env,
                     names& ns, parse_names_result& pr)
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
        if (pre_parse_ && tt == type::word)
        {
          const string& v (t.value);

          // Verify that the special builtin is not called inside an improper
          // context (flow control construct or complex expression).
          //
          auto verify = [first, env, &v, &l, this] ()
          {
            if (level_ != 0)
              fail (l) << "'" << v << "' call inside flow control construct";

            if (!first)
              fail (l) << "'" << v << "' call must be the only command";

            if (env)
              fail (l) << "'" << v << "' call via 'env' builtin";
          };

          if (v == "diag")
          {
            verify ();

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
            // will be executed prior to the script body execution to obtain
            // the custom diagnostics.
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
            verify ();

            // Verify that depdb is not used for anything other than
            // performing update on a file-based target.
            //
            assert (actions_ != nullptr);

            for (const action& a: *actions_)
            {
              if (a != perform_update_id)
                fail (l) << "'depdb' builtin cannot be used to "
                         << ctx.meta_operation_table[a.meta_operation ()].name
                         << ' ' << ctx.operation_table[a.operation ()];
            }

            if (!file_based_)
              fail (l) << "'depdb' builtin can only be used for file-based "
                       << "targets";

            if (diag_line_)
              fail (diag_line_->second)
                << "'diag' builtin call before 'depdb' call" <<
                info (l) << "'depdb' call is here";

            // Note that the rest of the line contains the builtin command
            // name, potentially followed by the arguments to be hashed/saved.
            // Thus, we parse it in the value lexer mode.
            //
            mode (lexer_mode::value);

            // Obtain and validate the depdb builtin command name.
            //
            next (t, tt);

            if (tt != type::word ||
                (v != "clear"  &&
                 v != "hash"   &&
                 v != "string" &&
                 v != "env"    &&
                 v != "dyndep"))
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

              if (!depdb_preamble_.empty ())
              {
                diag_record dr (fail (l));
                dr << "'depdb clear' should be the first 'depdb' builtin call";

                // Print the first depdb call location.
                //
                for (const line& l: depdb_preamble_)
                {
                  const replay_tokens& rt (l.tokens);
                  assert (!rt.empty ());

                  const token& t (rt[0].token);
                  if (t.type == type::word && t.value == "depdb")
                  {
                    dr << info (rt[0].location ())
                       << "first 'depdb' call is here";
                    break;
                  }
                }
              }

              // Save the builtin location, cancel the line saving, and clear
              // the referenced variable list, since it won't be used.
              //
              depdb_clear_ = l;
              save_line_ = nullptr;

              script_->vars.clear ();
            }
            else
            {
              // Verify depdb-dyndep is last.
              //
              if (v == "dyndep")
              {
                // Note that for now we do not allow multiple dyndep calls.
                // But we may wan to relax this later (though alternating
                // targets with prerequisites in depdb may be tricky -- maybe
                // still only allow additional targets in the first call).
                //
                if (!depdb_dyndep_)
                  depdb_dyndep_ = make_pair (l, depdb_preamble_.size ());
                else
                  fail (l) << "multiple 'depdb dyndep' calls" <<
                    info (depdb_dyndep_->first) << "previous call is here";

#if 0
                if (peek () == type::word && peeked ().value == "--byproduct")
                  ;
#endif
              }
              else
              {
                if (depdb_dyndep_)
                  fail (l) << "'depdb " << v << "' after 'depdb dyndep'" <<
                    info (depdb_dyndep_->first) << "'depdb dyndep' call is here";
              }

              // Move the script body to the end of the depdb preamble.
              //
              // Note that at this (pre-parsing) stage we cannot evaluate if
              // all the script body lines are allowed for depdb preamble.
              // That, in particular, would require to analyze pipelines to
              // see if they are terminated with the set builtin, but this
              // information is only available at the execution stage. Thus,
              // we move into the preamble whatever is there and delay the
              // check until the execution.
              //
              lines& ls (script_->body);
              depdb_preamble_.insert (depdb_preamble_.end (),
                                      make_move_iterator (ls.begin ()),
                                      make_move_iterator (ls.end ()));
              ls.clear ();

              // Also move the body_temp_dir flag, if it is true.
              //
              if (script_->body_temp_dir)
              {
                script_->depdb_preamble_temp_dir = true;
                script_->body_temp_dir = false;
              }

              // Reset the impure function call info since it's valid for the
              // depdb preamble.
              //
              impure_func_ = nullopt;

              // Instruct the parser to save the depdb builtin line separately
              // from the script lines, when it is fully parsed. Note that the
              // builtin command arguments will be validated during execution,
              // when expanded.
              //
              depdb_preamble_.push_back (line ());
              save_line_ = &depdb_preamble_.back ();
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

        {
          // During pre-parse, if the script name is not set manually we
          // suspend pre-parse, parse the command names for real and try to
          // deduce the script name from the result. Otherwise, we continue
          // to pre-parse and bail out after parsing the names.
          //
          // Note that the latter is not just an optimization since expansion
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
                dr << info (l)
                   << "while deducing low-verbosity script diagnostics name";

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
        // /usr/bin/g++ name@c++ checksum@... env-checksum@... --version
        //
        // Which is a bit harder to recognize syntactically. So what we are
        // going to do is have a separate first pass which reduces the
        // syntactic cases to the typed ones.
        //
        names pp_ns;
        const value_type* pp_vt (nullptr);
        if (pr.type == &value_traits<process_path>::value_type ||
            pr.type == &value_traits<process_path_ex>::value_type)
        {
          pp_ns = move (ns);
          pp_vt = pr.type;
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

            pp_vt = (i != b + 1
                     ? &value_traits<process_path_ex>::value_type
                     : &value_traits<process_path>::value_type);
          }
        }

        // Handle process_path[_ex], for example:
        //
        // {{
        //    $cxx.path ...
        // }}
        //
        if (pp_vt == &value_traits<process_path>::value_type)
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
        else if (pp_vt == &value_traits<process_path_ex>::value_type)
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
      execute_body (const scope& rs, const scope& bs,
                    environment& e, const script& s, runner& r,
                    bool enter, bool leave)
      {
        pre_exec (rs, bs, e, &s, &r);

        if (enter)
          runner_->enter (e, s.start_loc);

        // Note that we rely on "small function object" optimization here.
        //
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

        exec_lines (s.body, exec_cmd);

        if (leave)
          runner_->leave (e, s.end_loc);
      }

      void parser::
      exec_depdb_preamble (action a, const scope& bs, const file& t,
                           environment& e, const script& s, runner& r,
                           lines_iterator begin, lines_iterator end,
                           depdb& dd,
                           bool* update,
                           bool* deferred_failure,
                           optional<timestamp> mt)
      {
        tracer trace ("exec_depdb_preamble");

        // The only valid lines in the depdb preamble are the depdb builtin
        // itself as well as the variable assignments, including via the set
        // builtin.

        pre_exec (*bs.root_scope (), bs, e, &s, &r);

        // Let's "wrap up" the objects we operate upon into the single object
        // to rely on "small function object" optimization.
        //
        struct
        {
          tracer& trace;

          action a;
          const scope& bs;
          const file& t;

          environment& env;
          const script& scr;

          depdb& dd;
          bool* update;
          bool* deferred_failure;
          optional<timestamp> mt;

        } data {trace, a, bs, t, e, s, dd, update, deferred_failure, mt};

        auto exec_cmd = [this, &data] (token& t,
                                       build2::script::token_type& tt,
                                       size_t li,
                                       bool /* single */,
                                       const location& ll)
        {
          // Note that we never reset the line index to zero (as we do in
          // execute_body()) assuming that there are some script body
          // commands to follow.
          //
          if (tt == type::word && t.value == "depdb")
          {
            next (t, tt);

            // This should have been enforced during pre-parsing.
            //
            assert (tt == type::word); // <cmd> ... <newline>

            string cmd (move (t.value));

            if (cmd == "dyndep")
            {
              // Note: cast is safe since this is always executed in apply().
              //
              exec_depdb_dyndep (t, tt,
                                 li, ll,
                                 data.a, data.bs, const_cast<file&> (data.t),
                                 data.dd,
                                 *data.update,
                                 *data.deferred_failure,
                                 *data.mt);
            }
            else
            {
              names ns (exec_special (t, tt, true /* skip <cmd> */));

              if (cmd == "hash")
              {
                sha256 cs;
                for (const name& n: ns)
                  to_checksum (cs, n);

                if (data.dd.expect (cs.string ()) != nullptr)
                  l4 ([&] {
                      data.trace (ll)
                        << "'depdb hash' argument change forcing update of "
                        << data.t;});
              }
              else if (cmd == "string")
              {
                string s;
                try
                {
                  s = convert<string> (move (ns));
                }
                catch (const invalid_argument& e)
                {
                  fail (ll) << "invalid 'depdb string' argument: " << e;
                }

                if (data.dd.expect (s) != nullptr)
                  l4 ([&] {
                      data.trace (ll)
                        << "'depdb string' argument change forcing update of "
                        << data.t;});
              }
              else if (cmd == "env")
              {
                sha256 cs;
                const char* pf ("invalid 'depdb env' argument: ");

                try
                {
                  for (name& n: ns)
                  {
                    string vn (convert<string> (move (n)));
                    build2::script::verify_environment_var_name (vn, pf, ll);
                    hash_environment (cs, vn);
                  }
                }
                catch (const invalid_argument& e)
                {
                  fail (ll) << pf << e;
                }

                if (data.dd.expect (cs.string ()) != nullptr)
                  l4 ([&] {
                      data.trace (ll)
                        << "'depdb env' environment change forcing update of "
                        << data.t;});
              }
              else
                assert (false);
            }
          }
          else
          {
            command_expr ce (
              parse_command_line (t, static_cast<token_type&> (tt)));

            // Verify that this expression executes the set builtin.
            //
            if (find_if (ce.begin (), ce.end (),
                         [] (const expr_term& et)
                         {
                           const process_path& p (et.pipe.back ().program);
                           return p.initial == nullptr &&
                                  p.recall.string () == "set";
                         }) == ce.end ())
            {
              const replay_tokens& rt (data.scr.depdb_preamble.back ().tokens);
              assert (!rt.empty ());

              fail (ll) << "disallowed command in depdb preamble" <<
                info << "only variable assignments are allowed in "
                     << "depdb preamble" <<
                info (rt[0].location ()) << "depdb preamble ends here";
            }

            runner_->run (*environment_, ce, li, ll);
          }
        };

        exec_lines (begin, end, exec_cmd);
      }

      void parser::
      pre_exec (const scope& rs, const scope& bs,
                environment& e, const script* s, runner* r)
      {
        path_ = nullptr; // Set by replays.

        pre_parse_ = false;

        set_lexer (nullptr);

        actions_ = nullptr;

        // The script shouldn't be able to modify the scopes.
        //
        // Note that for now we don't set target_ since it's not clear what
        // it could be used for (we need scope_ for calling functions such as
        // $target.path()).
        //
        target_ = nullptr;
        root_ = const_cast<scope*> (&rs);
        scope_ = const_cast<scope*> (&bs);
        pbase_ = scope_->src_path_;

        script_ = const_cast<script*> (s);
        runner_ = r;
        environment_ = &e;
      }

      void parser::
      exec_lines (lines_iterator begin, lines_iterator end,
                  const function<exec_cmd_function>& exec_cmd)
      {
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

        build2::script::parser::exec_lines (begin, end,
                                            exec_set, exec_cmd, exec_if,
                                            environment_->exec_line,
                                            &environment_->var_pool);
      }

      names parser::
      exec_special (token& t, build2::script::token_type& tt, bool skip_first)
      {
        if (skip_first)
        {
          assert (tt != type::newline && tt != type::eos);
          next (t, tt);
        }

        return tt != type::newline && tt != type::eos
               ? parse_names (t, tt, pattern_mode::ignore)
               : names ();
      }

      names parser::
      execute_special (const scope& rs, const scope& bs,
                       environment& e,
                       const line& ln,
                       bool omit_builtin)
      {
        pre_exec (rs, bs, e, nullptr /* script */, nullptr /* runner */);

        // Copy the tokens and start playing.
        //
        replay_data (replay_tokens (ln.tokens));

        token t;
        build2::script::token_type tt;
        next (t, tt);

        names r (exec_special (t, tt, omit_builtin));

        replay_stop ();
        return r;
      }

      void parser::
      exec_depdb_dyndep (token& lt, build2::script::token_type& ltt,
                         size_t li, const location& ll,
                         action a, const scope& bs, file& t,
                         depdb& dd,
                         bool& update,
                         bool& deferred_failure,
                         timestamp mt)
      {
        tracer trace ("exec_depdb_dyndep");

        context& ctx (t.ctx);

        // Similar approach to parse_env_builtin().
        //
        depdb_dep_options ops;
        bool prog (false);
        {
          auto& t (lt);
          auto& tt (ltt);

          next (t, tt); // Skip 'dep' command.

          // Note that an option name and value can belong to different name
          // chunks. That's why we parse the arguments in the chunking mode
          // into the list up to the `--` separator and parse this list into
          // options afterwards. Note that the `--` separator should be
          // omitted if there is no program (i.e., additional dependency info
          // is being read from one of the prerequisites).
          //
          strings args;

          names ns; // Reuse to reduce allocations.
          while (tt != type::newline && tt != type::eos)
          {
            if (tt == type::word && t.value == "--")
            {
              prog = true;
              break;
            }

            location l (get_location (t));

            if (!start_names (tt))
              fail (l) << "depdb dyndep: expected option or '--' separator "
                       << "instead of " << t;

            parse_names (t, tt,
                         ns,
                         pattern_mode::ignore,
                         true /* chunk */,
                         "depdb dyndep builtin argument",
                         nullptr);

            for (name& n: ns)
            {
              try
              {
                args.push_back (convert<string> (move (n)));
              }
              catch (const invalid_argument&)
              {
                diag_record dr (fail (l));
                dr << "invalid string value ";
                to_stream (dr.os, n, true /* quote */);
              }
            }

            ns.clear ();
          }

          if (prog)
          {
            next (t, tt); // Skip '--'.

            if (tt == type::newline || tt == type::eos)
              fail (t) << "depdb dyndep: expected program name instead of "
                       << t;
          }

          // Parse the options.
          //
          // We would like to support both -I <dir> as well as -I<dir> forms
          // for better compatibility. The latter requires manual parsing.
          //
          try
          {
            for (cli::vector_scanner scan (args); scan.more (); )
            {
              if (ops.parse (scan, cli::unknown_mode::stop) && !scan.more ())
                break;

              const char* a (scan.peek ());

              // Handle -I<dir>
              //
              if (a[0] == '-' && a[1] == 'I')
              {
                try
                {
                  ops.include_path ().push_back (dir_path (a + 2));
                }
                catch (const invalid_path&)
                {
                  throw cli::invalid_value ("-I", a + 2);
                }

                scan.next ();
                continue;
              }

#if 0
              // Handle --byproduct in the wrong place.
              //
              if (strcmp (a, "--byproduct") == 0)
                fail (ll) << "depdb dyndep: --byproduct must be first option";
#endif

              // Handle unknown option.
              //
              if (a[0] == '-')
                throw cli::unknown_option (a);

              // Handle unexpected argument.
              //
              fail (ll) << "depdb dyndep: unexpected argument '" << a << "'";
            }
          }
          catch (const cli::exception& e)
          {
            fail (ll) << "depdb dyndep: " << e;
          }
        }

        // Get the default prerequisite type falling back to file{} if not
        // specified.
        //
        // The reason one would want to specify it is to make sure different
        // rules "resolve" the same dynamic prerequisites to the same targets.
        // For example, a rule that implements custom C compilation for some
        // translation unit would want to make sure it resolves extracted
        // system headers to h{} targets analogous to the c module's rule.
        //
        const target_type* def_pt;
        if (ops.default_prereq_type_specified ())
        {
          const string& t (ops.default_prereq_type ());

          def_pt = bs.find_target_type (t);
          if (def_pt == nullptr)
            fail (ll) << "unknown target type '" << t << "'";
        }
        else
          def_pt = &file::static_type;

        // This code is based on the prior work in the cc module (specifically
        // extract_headers()) where you can often find more detailed rationale
        // for some of the steps performed.

        using dyndep = dyndep_rule;

        // Build the maps lazily, only if/when needed.
        //
        using prefix_map = dyndep::prefix_map;
        using srcout_map = dyndep::srcout_map;

        function<dyndep::map_extension_func> map_ext (
          [] (const scope& bs, const string& n, const string& e)
          {
            // @@ TODO: allow specifying base target types.
            //
            // Feels like the only reason one would want to specify base types
            // is to tighten things up (as opposed to making some setup work)
            // since it essentially restricts the set of registered target
            // types that we will consider.
            //
            // Note also that these would be this project's target types while
            // the file could be from another project.
            //
            return dyndep::map_extension (bs, n, e, nullptr);

            // @@ TODO: should we return something as fallback (file{},
            //    def_pt)? Note: not the same semantics as enter_file()'s
            //    fallback. Feels like it could conceivably be different
            //    (e.g., h{} for fallback and hxx{} for some "unmappable" gen
            //    header). It looks like the "best" way currently is to define
            //    a custom target types for it (see moc{} in libQt5Core).
            //
            //    Note also that we should only do this if bs is in our
            //    project.
          });

        // Don't we want to insert a "local"/prefixless mapping in case the
        // user did not specify any -I's?  But then will also need src-out
        // remapping. So it will be equivalent to -I$out_base -I$src_base? But
        // then it's not hard to add explicitly...
        //
        function<dyndep::prefix_map_func> pfx_map;

        struct
        {
          tracer& trace;
          const location& ll;
          const depdb_dep_options& ops;
          optional<prefix_map> map;
        } pfx_data {trace, ll, ops, nullopt};

        if (!ops.include_path ().empty ())
        {
          pfx_map = [this, &pfx_data] (action,
                                       const scope& bs,
                                       const target& t) -> const prefix_map&
          {
            if (!pfx_data.map)
            {
              pfx_data.map = prefix_map ();

              const scope& rs (*bs.root_scope ());

              for (dir_path d: pfx_data.ops.include_path ())
              {
                if (d.relative ())
                  fail (pfx_data.ll) << "depdb dyndep: relative include "
                                     << "search path " << d;

                if (!d.normalized (false /* canonical dir seperators */))
                  d.normalize ();

                // If we are not inside our project root, then ignore.
                //
                if (d.sub (rs.out_path ()))
                  dyndep::append_prefix (
                    pfx_data.trace, *pfx_data.map, t, move (d));
              }
            }

            return *pfx_data.map;
          };
        }

        optional<path> file;
        enum class format {make} fmt (format::make);
        command_expr cmd;
        srcout_map so_map;

        // Parse the remainder of the command line as a program (which can be
        // a pipe). If file is absent, then we save the command's stdout to a
        // pipe. Otherwise, assume the command writes to file and add it to
        // the cleanups.
        //
        // Note that MSVC /showInclude sends its output to stderr (and so
        // could do other broken tools). However, the user can always merge
        // stderr to stdout (2>&1).
        //
        auto init_run = [this, &ctx,
                         &lt, &ltt, &ll,
                         &ops, prog, &file, &cmd, &so_map] ()
        {
          // --format
          //
          if (ops.format_specified ())
          {
            const string& f (ops.format ());

            if (f != "make")
              fail (ll) << "depdb dyndep: invalid --format option value '"
                        << f << "'";
          }

          // --file
          //
          if (ops.file_specified ())
          {
            file = move (ops.file ());

            if (file->relative ())
              fail (ll) << "depdb dyndep: relative path specified with --file";
          }

          // Populate the srcout map with the -I$out_base -I$src_base pairs.
          //
          {
            dyndep::srcout_builder builder (ctx, so_map);

            for (dir_path d: ops.include_path ())
              builder.next (move (d));
          }

          if (prog)
          {
            cmd = parse_command_line (lt, static_cast<token_type&> (ltt));

            // If the output goes to stdout, then this should be a single
            // pipeline without any logical operators (&& or ||).
            //
            if (!file && cmd.size () != 1)
              fail (ll) << "depdb dyndep: command with stdout output cannot "
                        << "contain logical operators";

            // Note that we may need to run this command multiple times. The
            // two potential issues here are the re-registration of the
            // clenups and re-use of the special files (stdin, stdout, etc;
            // they include the line index in their names to avoid clashes
            // between lines).
            //
            // Cleanups are not an issue, they will simply replaced. And
            // overriding the contents of the special files seems harmless and
            // consistent with what would happen if the command redirects its
            // output to a non-special file.
            //
            if (file)
              environment_->clean (
                {build2::script::cleanup_type::always, *file},
                true /* implicit */);
          }
          else
          {
            // Assume file is one of the prerequisites.
            //
            if (!file)
              fail (ll) << "depdb dyndep: program or --file expected";
          }
        };

        // Enter as a target, update, and add to the list of prerequisite
        // targets a file.
        //
        const char* what (ops.what_specified ()
                          ? ops.what ().c_str ()
                          : "file");

        size_t skip_count (0);
        auto add = [this, &trace, what,
                    a, &bs, &t,
                    &map_ext, def_pt, &pfx_map, &so_map,
                    &dd, &skip_count] (path fp,
                                       bool cache,
                                       timestamp mt) -> optional<bool>
        {
          context& ctx (t.ctx);

          // We can only defer the failure if we will be running the recipe
          // body.
          //
          auto fail = [this, what, &ctx] (const auto& f) -> optional<bool>
          {
            bool df (!ctx.match_only && !ctx.dry_run_option);

            diag_record dr;
            dr << error << what << ' ' << f << " not found and no rule to "
               << "generate it";

            if (df)
              dr << info << "failure deferred to recipe body diagnostics";

            if (verb < 4)
              dr << info << "re-run with --verbose=4 for more information";

            if (df)
              return nullopt;
            else
              dr << endf;
          };

          if (const build2::file* ft = dyndep::enter_file (
                trace, what,
                a, bs, t,
                move (fp), cache, false /* normalize */,
                map_ext, *def_pt, pfx_map, so_map).first)
          {
            if (optional<bool> u = dyndep::inject_file (
                  trace, what,
                  a, t,
                  *ft, mt, false /* fail */))
            {
              if (!cache)
                dd.expect (ft->path ());

              skip_count++;
              return *u;
            }
            else if (cache)
            {
              dd.write (); // Invalidate this line.
              return true;
            }
            else
              return fail (*ft);
          }
          else
            return fail (fp);
        };

        // If things go wrong (and they often do in this area), give the user
        // a bit extra context.
        //
        auto df = make_diag_frame (
          [this, &ll, &t] (const diag_record& dr)
          {
            if (verb != 0)
              dr << info (ll) << "while extracting dynamic dependencies for "
                 << t;
          });

        // If nothing so far has invalidated the dependency database, then try
        // the cached data before running the program.
        //
        bool cache (!update);

        for (bool restart (true), first_run (true); restart; cache = false)
        {
          restart = false;

          if (cache)
          {
            // If any, this is always the first run.
            //
            assert (skip_count == 0);

            // We should always end with a blank line.
            //
            for (;;)
            {
              string* l (dd.read ());

              // If the line is invalid, run the compiler.
              //
              if (l == nullptr)
              {
                restart = true;
                break;
              }

              if (l->empty ()) // Done, nothing changed.
                return;

              if (optional<bool> r = add (path (move (*l)), true /*cache*/, mt))
              {
                restart = *r;

                if (restart)
                {
                  update = true;
                  l6 ([&]{trace << "restarting (cache)";});
                  break;
                }
              }
              else
              {
                // Trigger rebuild and mark as expected to fail.
                //
                update = true;
                deferred_failure = true;
                return;
              }
            }
          }
          else
          {
            if (first_run)
            {
              init_run ();
              first_run = false;
            }
            else if (!prog)
            {
              fail (ll) << "generated " << what << " without program to retry";
            }

            // Save the timestamp just before we run the command. If we depend
            // on any file that has been updated since, then we should assume
            // we have "seen" the old copy and restart.
            //
            timestamp rmt (prog ? system_clock::now () : mt);

            // Run the command if any and reduce outputs to common istream.
            //
            // Note that the resulting stream should tolerate partial read.
            //
            // While reading the entire stdout into a string is not the most
            // efficient way to do it, this does simplify things quite a bit,
            // not least of which is not having to parse the output before
            // knowing the program exist status.
            //
            istringstream iss;
            if (prog)
            {
              string s;
              build2::script::run (*environment_,
                                   cmd,
                                   li,
                                   ll,
                                   !file ? &s : nullptr);

              if (!file)
              {
                iss.str (move (s));
                iss.exceptions (istream::badbit);
              }
            }

            ifdstream ifs (ifdstream::badbit);
            if (file)
            try
            {
              ifs.open (*file);
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to open file " << *file << ": " << e;
            }

            istream& is (file
                         ? static_cast<istream&> (ifs)
                         : static_cast<istream&> (iss));

            const path_name& in (file
                                 ? path_name (*file)
                                 : path_name ("<stdin>"));

            location il (in, 1);

            // The way we parse things is format-specific.
            //
            size_t skip (skip_count);

            switch (fmt)
            {
            case format::make:
              {
                using make_state = make_parser;
                using make_type = make_parser::type;

                make_parser make;

                for (string l; !restart; ++il.line) // Reuse the buffer.
                {
                  if (eof (getline (is, l)))
                  {
                    if (make.state != make_state::end)
                      fail (il) << "incomplete make dependency declaration";

                    break;
                  }

                  size_t pos (0);
                  do
                  {
                    pair<make_type, string> r;
                    {
                      auto df = make_diag_frame (
                        [this, &l] (const diag_record& dr)
                        {
                          if (verb != 0)
                            dr << info << "while parsing make dependency "
                               << "declaration line '" << l << "'";
                        });

                      r = make.next (l, pos, il, false /* strict */);
                    }

                    if (r.second.empty ())
                      continue;

                    // @@ TODO: what should we do about targets?
                    //
                    // Note that if we take GCC as an example, things are
                    // quite messed up: by default it ignores -o and just
                    // takes the source file name and replaces the extension
                    // with a platform-appropriate object file extension. One
                    // can specify a custom target (or even multiple targets)
                    // with -MT or with -MQ (quoting). Though MinGW GCC still
                    // does not quote `:` with -MQ. So in this case it's
                    // definitely easier for the user to ignore the targets
                    // and just specify everything in the buildfile.
                    //
                    // On the other hand, other tools are likely to produce
                    // more sensible output (except perhaps for quoting).
                    //
                    // @@ Maybe in the lax mode we should only recognize `:`
                    //    if it's separated on at least one side?
                    //
                    //    Alternatively, we could detect Windows drives in
                    //    paths and "handle" them (I believe this is what GNU
                    //    make does). Maybe we should have three formats:
                    //    make-lax, make, make-strict?
                    //
                    if (r.first == make_type::target)
                      continue;

                    // Skip until where we left off.
                    //
                    if (skip != 0)
                    {
                      skip--;
                      continue;
                    }

                    if (optional<bool> u = add (path (move (r.second)),
                                                false /* cache */,
                                                rmt))
                    {
                      restart = *u;

                      if (restart)
                      {
                        update = true;
                        l6 ([&]{trace << "restarting";});
                        break;
                      }
                    }
                    else
                    {
                      // Trigger recompilation, mark as expected to fail, and
                      // bail out.
                      //
                      update = true;
                      deferred_failure = true;
                      break;
                    }
                  }
                  while (pos != l.size ());

                  if (make.state == make_state::end || deferred_failure)
                    break;
                }

                break;
              }
            }

            // Bail out early if we have deferred a failure.
            //
            if (deferred_failure)
              return;
          }
        }

        // Add the terminating blank line (we are updating depdb).
        //
        dd.expect ("");
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
              script_->body_temp_dir = true;
          }
          else if (!name.empty ())
          {
            if (pre_parse_suspended_)
            {
              const variable* pvar (scope_->ctx.var_pool.find (name));

              if (pvar != nullptr)
                r = (*scope_)[*pvar];
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
            script_->depdb_preamble.empty ())
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

      void parser::
      lookup_function (string&& name, const location& loc)
      {
        if (perform_update_ && file_based_ && !impure_func_)
        {
          const function_overloads* f (ctx.functions.find (name));

          if (f != nullptr && !f->pure)
            impure_func_ = make_pair (move (name), loc);
        }
      }
    }
  }
}
