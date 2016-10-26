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
      pre_parse (istream& is, const path& p, script& s)
      {
        path_ = &p;

        pre_parse_ = true;

        lexer l (is, *path_, lexer_mode::script_line);
        lexer_ = &l;
        base_parser::lexer_ = &l;

        script_ = &s;
        runner_ = nullptr;

        group_ = script_;
        lines_ = nullptr;

        scope_ = nullptr;

        // Start location of the implied script group is the beginning of the
        // file. End location -- end of the file.
        //
        group_->start_loc_ = location (path_, 1, 1);

        token t (pre_parse_scope_body ());

        if (t.type != type::eos)
          fail (t) << "stray " << t;

        // Check that we don't expect more lines.
        //
        if (lines_ != nullptr)
          fail (t) << "expected another line after semicolon";

        group_->end_loc_ = get_location (t);
      }

      void parser::
      parse (scope& sc, const path& p, script& s, runner& r)
      {
        path_ = &p;

        pre_parse_ = false;

        lexer_ = nullptr;
        base_parser::lexer_ = nullptr;

        script_ = &s;
        runner_ = &r;

        group_ = nullptr;
        lines_ = nullptr;

        scope_ = &sc;

        parse_scope_body ();
      }

      token parser::
      pre_parse_scope_body ()
      {
        token t;
        type tt;

        // Parse lines (including nested scopes) until we see '}' or eos.
        //
        for (;;)
        {
          // Start lexing each line recognizing leading '+-{}'.
          //
          mode (lexer_mode::first_token);

          // Determine the line type by peeking at the first token.
          //
          tt = peek ();
          const location ll (get_location (peeked ()));

          line_type lt;
          switch (tt)
          {
          case type::eos:
          case type::rcbrace:
            {
              next (t, tt);
              return t;
            }
          case type::lcbrace:
            {
              // @@ Nested scope. Get newlines after open/close.

              assert (false);

              /*
              group_->start_loc_ = get_location (t);

              // Check that we don't expect more lines.
              //
              if (lines_ != nullptr)
                fail (t) << "expected another line after semicolon";

              group_->end_loc_ = get_location (t);
              */

              continue;
            }
          case type::plus:
          case type::minus:
            {
              // This is a setup/teardown command.
              //
              lt = (tt == type::plus ? line_type::setup : line_type::tdown);

              next (t, tt);   // Start saving tokens from the next one.
              replay_save ();
              next (t, tt);
              break;
            }
          default:
            {
              // This is either a test command or a variable assignment.
              //
              replay_save (); // Start saving tokens from the current one.
              next (t, tt);

              // Decide whether this is a variable assignment or a command. It
              // is an assignment if the first token is an unquoted word and
              // the next is an assign/append/prepend operator. Assignment to
              // a computed variable name must use the set builtin.
              //
              if (tt == type::word && !t.quoted)
              {
                // Switch the recognition of leading variable assignments for
                // the next token. This is safe to do because we know we
                // cannot be in the quoted mode (since the current token is
                // not quoted).
                //
                mode (lexer_mode::second_token);
                type p (peek ());

                if (p == type::assign  ||
                    p == type::prepend ||
                    p == type::append)
                {
                  lt = line_type::variable;
                  break;
                }
              }

              lt = line_type::test;
              break;
            }
          }

          // Being here means we know the line type and token saving is on.
          // First we pre-parse the line keeping track of whether it ends with
          // a semicolon.
          //
          bool semi;

          switch (lt)
          {
          case line_type::variable:
            semi = parse_variable_line (t, tt);
            break;
          case line_type::setup:
          case line_type::tdown:
          case line_type::test:
            semi = parse_command_line (t, tt, lt, 0);
            break;
          }

          assert (tt == type::newline);

          // Stop saving and get the tokens.
          //
          line l {lt, replay_data ()};

          // Decide where it goes.
          //
          lines* ls (nullptr);

          // If lines_ is not NULL then the previous command ended with a
          // semicolon and we should add this one to the same place.
          //
          if (lines_ != nullptr)
          {
            switch (lt)
            {
            case line_type::setup: fail (ll) << "setup command in test";
            case line_type::tdown: fail (ll) << "teardown command in test";
            default: break;
            }

            ls = lines_;
          }
          else
          {
            switch (lt)
            {
            case line_type::setup:
              {
                if (!group_->scopes.empty ())
                  fail (ll) << "setup command after tests";

                if (!group_->tdown_.empty ())
                  fail (ll) << "setup command after teardown";

                ls = &group_->setup_;
                break;
              }
            case line_type::tdown:
              {
                ls = &group_->tdown_;
                break;
              }
            case line_type::variable:
              {
                // If there is a semicolon after the variable then we assume
                // it is part of a test (there is no reason to use semicolons
                // after variables in the group scope).
                //
                if (!semi)
                {
                  // If we don't have any nested scopes or teardown commands,
                  // then we assume this is a setup, otherwise -- teardown.
                  //
                  ls = group_->scopes.empty () && group_->tdown_.empty ()
                    ? &group_->setup_
                    : &group_->tdown_;

                  break;
                }

                // Fall through.
              }
            case line_type::test:
              {
                // First check that we don't have any teardown commands yet.
                // This will detect things like variable assignments between
                // tests.
                //
                if (!group_->tdown_.empty ())
                {
                  // @@ Can the teardown line be from a different file?
                  //
                  location tl (
                    get_location (
                      group_->tdown_.back ().tokens.front ().token));

                  fail (ll) << "test after teardown" <<
                    info (tl) << "last teardown line appears here";
                }

                // Create implicit test scope. Use line number as the scope id.
                //
                unique_ptr<test> p (new test (to_string (ll.line), *group_));

                p->start_loc_ = ll;
                p->end_loc_ = get_location (t);

                ls = &p->tests_;

                group_->scopes.push_back (move (p));
                break;
              }
            }
          }

          ls->push_back (move (l));

          // If this command ended with a semicolon, then the next one should
          // go to the same place.
          //
          lines_ = semi ? ls : nullptr;
        }
      }

      void parser::
      parse_scope_body ()
      {
        auto play = [this] (lines& ls) // Note: destructive to lines.
        {
          token t;
          type tt;

          for (size_t i (0), li (0), n (ls.size ()); i != n; ++i)
          {
            line& l (ls[i]);

            replay_data (move (l.tokens)); // Set the tokens and start playing.

            // We don't really need to change the mode since we already know
            // the line type.
            //
            next (t, tt);

            switch (l.type)
            {
            case line_type::variable:
              {
                parse_variable_line (t, tt);
                break;
              }
            case line_type::setup:
            case line_type::tdown:
            case line_type::test:
              {
                // We use the 0 index to signal that this is the only command.
                //
                if (li == 0)
                {
                  size_t j (i + 1);
                  for (; j != n && ls[j].type == line_type::variable; ++j) ;

                  if (j != n) // We have another command.
                    ++li;
                }
                else
                  ++li;

                parse_command_line (t, tt, l.type, li);
                break;
              }
            }

            assert (tt == type::newline);
            replay_stop (); // Stop playing.
          }
        };

        runner_->enter (*scope_, scope_->start_loc_);

        if (test* t = dynamic_cast<test*> (scope_))
        {
          play (t->tests_);
        }
        else if (group* g = dynamic_cast<group*> (scope_))
        {
          play (g->setup_);

          for (const unique_ptr<scope>& s: g->scopes)
          {
            // Hand it off to a sub-parser potentially in another thread. But
            // we could also have handled it serially in this parser:
            //
            // scope* os (scope_);
            // scope_ = s.get ();
            // parse_scope_body ();
            // scope_ = os;
            //
            parser p;
            p.parse (*s, *path_, *script_, *runner_);
          }

          play (g->tdown_);
        }
        else
          assert (false);

        runner_->leave (*scope_, scope_->end_loc_);
      }

      // Return true if the string contains only digit characters (used to
      // detect the special $NN variables).
      //
      static inline bool
      digits (const string& s)
      {
        for (char c: s)
          if (!digit (c))
            return false;

        return !s.empty ();
      }

      bool parser::
      parse_variable_line (token& t, type& tt)
      {
        string name (move (t.value));

        // Check if we are trying to modify any of the special aliases ($*,
        // $~, $N).
        //
        if (pre_parse_)
        {
          if (name == "*" || name == "~" || digits (name))
            fail (t) << "attempt to set '" << name << "' variable directly";
        }

        next (t, tt);
        type kind (tt); // Assignment kind.

        // We cannot reuse the value mode since it will recognize { which we
        // want to treat as a literal.
        //
        mode (lexer_mode::variable_line);
        next (t, tt);

        // Parse value attributes if any. Note that it's ok not to have
        // anything after the attributes (e.g., foo=[null]).
        //
        attributes_push (t, tt, true);

        value rhs (tt != type::newline && tt != type::semi
                   ? parse_names_value (t, tt, "variable value", nullptr)
                   : value (names ()));

        bool semi (tt == type::semi);

        if (semi)
          next (t, tt); // Get newline.

        if (tt != type::newline)
          fail (t) << "expected newline instead of " << t;

        if (!pre_parse_)
        {
          const variable& var (script_->var_pool.insert (move (name)));

          value& lhs (kind == type::assign
                      ? scope_->assign (var)
                      : scope_->append (var));

          // @@ Need to adjust to make strings the default type.
          //
          apply_value_attributes (&var, lhs, move (rhs), kind);

          // Handle the $*, $NN special aliases.
          //
          // The plan is as follows: in this function we detect modification
          // of the source variables (test*), and (re)set $* to NULL on this
          // scope (this is important to both invalidate any old values but
          // also to "stake" the lookup position). This signals to the
          // variable lookup function below that the $* and $NN values need to
          // be recalculated from their sources. Note that we don't need to
          // invalidate $NN since their lookup always checks $* first.
          //
          if (var.name == script_->test_var.name ||
              var.name == script_->opts_var.name ||
              var.name == script_->args_var.name)
          {
            scope_->assign (script_->cmd_var) = nullptr;
          }
        }

        return semi;
      }

      bool parser::
      parse_command_line (token& t, type& tt, line_type lt, size_t li)
      {
        command c;

        // Pending positions where the next word should go.
        //
        enum class pending
        {
          none,
          program,
          in_string,
          in_document,
          in_file,
          out_string,
          out_document,
          out_file,
          err_string,
          err_document,
          err_file
        };
        pending p (pending::program);
        bool nn (false);  // True if pending here-{str,doc} is "no-newline".
        bool app (false); // True if to append to pending file.

        // Ordered sequence of here-document redirects that we can expect to
        // see after the command line.
        //
        struct here_doc
        {
          redirect* redir;
          string end;
          bool no_newline;
        };
        vector<here_doc> hd;

        // Add the next word to either one of the pending positions or
        // to program arguments by default.
        //
        auto add_word =
          [&c, &p, &nn, &app, &hd, this] (string&& w, const location& l)
        {
          auto add_here_str = [&nn] (redirect& r, string&& w)
          {
            if (!nn) w += '\n';
            r.str = move (w);
          };

          auto add_here_end = [&hd, &nn] (redirect& r, string&& w)
          {
            hd.push_back (here_doc {&r, move (w), nn});
          };

          auto add_file =
            [&app, &l, this] (redirect& r, const char* n, string&& w)
          {
            try
            {
              r.file.path = path (move (w));

              if (r.file.path.empty ())
                fail (l) << "empty " << n << " redirect file path";

            }
            catch (const invalid_path& e)
            {
              fail (l) << "invalid " << n << "redirect file path '" << e.path
                       << "'";
            }

            r.file.append = app;
          };

          switch (p)
          {
          case pending::none:    c.arguments.push_back (move (w)); break;
          case pending::program:
          {
            try
            {
              c.program = path (move (w));

              if (c.program.empty ())
                fail (l) << "empty program path";
            }
            catch (const invalid_path& e)
            {
              fail (l) << "invalid program path '" << e.path << "'";
            }
            break;
          }

          case pending::in_document:  add_here_end (c.in,  move (w)); break;
          case pending::out_document: add_here_end (c.out, move (w)); break;
          case pending::err_document: add_here_end (c.err, move (w)); break;

          case pending::in_string:  add_here_str (c.in,  move (w)); break;
          case pending::out_string: add_here_str (c.out, move (w)); break;
          case pending::err_string: add_here_str (c.err, move (w)); break;

          case pending::in_file:  add_file (c.in,  "stdin",  move (w)); break;
          case pending::out_file: add_file (c.out, "stdout", move (w)); break;
          case pending::err_file: add_file (c.err, "stderr", move (w)); break;
          }

          p = pending::none;
          nn = false;
          app = false;
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
          case pending::in_file:      what = "stdin file";               break;
          case pending::out_string:   what = "stdout here-string";       break;
          case pending::out_document: what = "stdout here-document end"; break;
          case pending::out_file:     what = "stdout file";              break;
          case pending::err_string:   what = "stderr here-string";       break;
          case pending::err_document: what = "stderr here-document end"; break;
          case pending::err_file:     what = "stderr file";              break;
          }

          if (what != nullptr)
            fail (l) << "missing " << what;
        };

        // Parse the redirect operator.
        //
        auto parse_redirect =
          [&c, &p, &nn, &app, this] (const token& t, const location& l)
        {
          // Our semantics is the last redirect seen takes effect.
          //
          assert (p == pending::none && !nn && !app);

          // See if we have the file descriptor.
          //
          unsigned long fd (3);
          if (!t.separated)
          {
            if (c.arguments.empty ())
              fail (l) << "missing redirect file descriptor";

            const string& s (c.arguments.back ());

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

            c.arguments.pop_back ();
          }

          type tt (t.type);

          // Validate/set default file descriptor.
          //
          switch (tt)
          {
          case type::in_pass:
          case type::in_null:
          case type::in_str:
          case type::in_str_nn:
          case type::in_doc:
          case type::in_doc_nn:
          case type::in_file:
            {
              if ((fd = fd == 3 ? 0 : fd) != 0)
                fail (l) << "invalid in redirect file descriptor " << fd;

              break;
            }
          case type::out_pass:
          case type::out_null:
          case type::out_str:
          case type::out_str_nn:
          case type::out_doc:
          case type::out_doc_nn:
          case type::out_file:
          case type::out_file_app:
            {
              if ((fd = fd == 3 ? 1 : fd) == 0)
                fail (l) << "invalid out redirect file descriptor " << fd;

              break;
            }
          }

          redirect_type rt;
          switch (tt)
          {
          case type::in_pass:
          case type::out_pass:     rt = redirect_type::pass;          break;

          case type::in_null:
          case type::out_null:     rt = redirect_type::null;          break;

          case type::in_str_nn:
          case type::out_str_nn:   nn = true; // Fall through.
          case type::in_str:
          case type::out_str:      rt = redirect_type::here_string;   break;

          case type::in_doc_nn:
          case type::out_doc_nn:   nn = true; // Fall through.
          case type::in_doc:
          case type::out_doc:      rt = redirect_type::here_document; break;

          case type::out_file_app: app = true; // Fall through.
          case type::in_file:
          case type::out_file:     rt = redirect_type::file; break;
          }

          redirect& r (fd == 0 ? c.in : fd == 1 ? c.out : c.err);
          r = redirect (rt);

          switch (rt)
          {
          case redirect_type::none:
          case redirect_type::pass:
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
          case redirect_type::file:
            switch (fd)
            {
            case 0: p = pending::in_file;  break;
            case 1: p = pending::out_file; break;
            case 2: p = pending::err_file; break;
            }
            break;
          }
        };

        const location ll (get_location (t)); // Line location.

        // Keep parsing chunks of the command line until we see one of the
        // "terminators" (newline, semicolon, exit status comparison, etc).
        //
        location l (ll);
        names ns; // Reuse to reduce allocations.

        for (bool done (false); !done; l = get_location (t))
        {
          switch (tt)
          {
          case type::equal:
          case type::not_equal:
          case type::semi:
          case type::newline:
            {
              done = true;
              break;
            }

          case type::in_pass:
          case type::out_pass:

          case type::in_null:
          case type::out_null:

          case type::in_str:
          case type::in_doc:
          case type::out_str:
          case type::out_doc:

          case type::in_str_nn:
          case type::in_doc_nn:
          case type::out_str_nn:
          case type::out_doc_nn:

          case type::in_file:
          case type::out_file:
          case type::out_file_app:
            {
              if (pre_parse_)
              {
                // The only thing we need to handle here are the here-document
                // end markers since we need to know how many of them to pre-
                // parse after the command.
                //
                nn = false;

                switch (tt)
                {
                case type::in_doc_nn:
                case type::out_doc_nn:
                  nn = true;
                  // Fall through.
                case type::in_doc:
                case type::out_doc:
                  // We require the end marker to be a literal, unquoted word.
                  // In particularm, we don't allow quoted because of cases
                  // like foo"$bar" (where we will see word 'foo').
                  //
                  next (t, tt);

                  if (tt != type::word || t.quoted)
                    fail (l) << "here-document end marker expected";

                  hd.push_back (here_doc {nullptr, move (t.value), nn});
                  break;
                }

                next (t, tt);
                break;
              }

              // If this is one of the operators/separators, check that we
              // don't have any pending locations to be filled.
              //
              check_pending (l);

              // Note: there is another one in the inner loop below.
              //
              switch (tt)
              {
              case type::in_pass:
              case type::out_pass:

              case type::in_null:
              case type::out_null:

              case type::in_str:
              case type::in_doc:
              case type::out_str:
              case type::out_doc:

              case type::in_str_nn:
              case type::in_doc_nn:
              case type::out_str_nn:
              case type::out_doc_nn:

              case type::in_file:
              case type::out_file:
              case type::out_file_app:

                parse_redirect (t, l);
                next (t, tt);
                break;
              }

              break;
            }
          default:
            {
              // Parse the next chunk as simple names to get expansion, etc.
              // Note that we do it in the chunking mode to detect whether
              // anything in each chunk is quoted.
              //
              reset_quoted (t);
              parse_names (t, tt, ns, true, "command line", nullptr);

              if (pre_parse_) // Nothing else to do if we are pre-parsing.
                break;

              // Process what we got. Determine whether anything inside was
              // quoted (note that the current token is "next" and is not part
              // of this).
              //
              bool q ((quoted () - (t.quoted ? 1 : 0)) != 0);

              for (name& n: ns)
              {
                string s;

                try
                {
                  s = value_traits<string>::convert (move (n), nullptr);
                }
                catch (const invalid_argument&)
                {
                  fail (l) << "invalid string value '" << n << "'";
                }

                // If it is a quoted chunk, then we add the word as is.
                // Otherwise we re-lex it. But if the word doesn't contain any
                // interesting characters (operators plus quotes/escapes),
                // then no need to re-lex.
                //
                // NOTE: updated quoting (script.cxx:to_stream_q()) if adding
                // any new characters.
                //
                if (q || s.find_first_of ("|&<>\'\"\\") == string::npos)
                  add_word (move (s), l);
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
                    string n (l.file->string ());
                    n += ':';
                    n += to_string (l.line);
                    n += ':';
                    n += to_string (l.column);
                    n += ": (";
                    n += s;
                    n += ')';
                    name = path (move (n));
                  }

                  istringstream is (s);
                  lexer lex (is, name, lexer_mode::command_line);

                  // Treat the first "sub-token" as always separated from what
                  // we saw earlier.
                  //
                  // Note that this is not "our" token so we cannot do
                  // fail(t). Rather we should do fail(l).
                  //
                  token t (lex.next ());
                  location l (build2::get_location (t, name));
                  t.separated = true;

                  string w;
                  bool f (t.type == type::eos); // If the whole thing is empty.

                  for (; t.type != type::eos; t = lex.next ())
                  {
                    type tt (t.type);
                    l = build2::get_location (t, name);

                    // Re-lexing double-quotes will recognize $, ( inside as
                    // tokens so we have to reverse them back. Since we don't
                    // treat spaces as separators we can be sure we will get
                    // it right.
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

                    if (tt == type::word)
                    {
                      w = move (t.value);
                      f = true;
                      continue;
                    }

                    // If this is one of the operators/separators, check that
                    // we don't have any pending locations to be filled.
                    //
                    check_pending (l);

                    // Note: there is another one in the outer loop above.
                    //
                    switch (tt)
                    {
                    case type::in_pass:
                    case type::out_pass:

                    case type::in_null:
                    case type::out_null:

                    case type::in_str:
                    case type::out_str:

                    case type::in_str_nn:
                    case type::out_str_nn:

                    case type::in_file:
                    case type::out_file:
                    case type::out_file_app:
                      {
                        parse_redirect (t, l);
                        break;
                      }

                    case type::in_doc:
                    case type::out_doc:

                    case type::in_doc_nn:
                    case type::out_doc_nn:
                      {
                        fail (l) << "here-document redirect in expansion";
                        break;
                      }
                    }
                  }

                  // Don't forget the last word.
                  //
                  if (!w.empty () || f)
                    add_word (move (w), l);
                }
              }

              ns.clear ();
              break;
            }
          }
        }

        // Verify we don't have anything pending to be filled.
        //
        if (!pre_parse_)
          check_pending (l);

        // While we no longer need to recognize command line operators, we
        // also don't expect a valid test trailer to contain them. So we are
        // going to continue lexing in the script_line mode.
        //
        if (tt == type::equal || tt == type::not_equal)
          c.exit = parse_command_exit (t, tt);

        // Semicolon is only valid in test command lines. Note that we still
        // recognize it lexically, it's just not a valid token per the
        // grammar.
        //
        bool semi (tt == type::semi && lt == line_type::test);

        if (semi)
          next (t, tt); // Get newline.

        if (tt != type::newline)
          fail (t) << "expected newline instead of " << t;

        // Parse here-document fragments in the order they were mentioned on
        // the command line.
        //
        for (here_doc& h: hd)
        {
          // Switch to the here-line mode which is like double-quoted but
          // recognized the newline as a separator.
          //
          mode (lexer_mode::here_line);
          next (t, tt);

          string v (parse_here_document (t, tt, h.end, h.no_newline));

          if (!pre_parse_)
          {
            redirect& r (*h.redir);
            r.doc.doc = move (v);
            r.doc.end = move (h.end);
          }

          expire_mode ();
        }

        // Now that we have all the pieces, run the command.
        //
        if (!pre_parse_)
          runner_->run (*scope_, c, li, ll);

        return semi;
      }

      command_exit parser::
      parse_command_exit (token& t, type& tt)
      {
        exit_comparison comp (tt == type::equal
                              ? exit_comparison::eq
                              : exit_comparison::ne);

        // The next chunk should be the exit status.
        //
        next (t, tt);
        names ns (parse_names (t, tt, true, "exit status", nullptr));
        unsigned long es (256);

        if (!pre_parse_)
        {
          try
          {
            if (ns.size () == 1 && ns[0].simple () && !ns[0].empty ())
              es = stoul (ns[0].value);
          }
          catch (const exception&) {} // Fall through.

          if (es > 255)
            fail (t) << "exit status expected instead of '" << ns << "'" <<
              info << "exit status is an unsigned integer less than 256";
        }

        return command_exit {comp, static_cast<uint8_t> (es)};
      }

      string parser::
      parse_here_document (token& t, type& tt, const string& em, bool nn)
      {
        string r;

        while (tt != type::eos)
        {
          // Check if this is the end marker.
          //
          if (tt == type::word &&
              !t.quoted        &&
              t.value == em    &&
              peek () == type::newline)
          {
            next (t, tt); // Get the newline.
            break;
          }

          // Expand the line (can be blank).
          //
          names ns (tt != type::newline
                    ? parse_names (t, tt, false, "here-document line", nullptr)
                    : names ());

          if (!pre_parse_)
          {
            if (!r.empty ()) // Add newline after previous line.
              r += '\n';

            // What shall we do if the expansion results in multiple names?
            // For, example if the line contains just the variable expansion
            // and it is of type strings. Adding all the elements space-
            // separated seems like the natural thing to do.
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
            }
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

        // Add final newline if requested.
        //
        if (!pre_parse_ && !nn)
          r += '\n';

        return r;
      }

      lookup parser::
      lookup_variable (name&& qual, string&& name, const location& loc)
      {
        assert (!pre_parse_);

        if (!qual.empty ())
          fail (loc) << "qualified variable name";

        // @@ MT: will need RW mutex on var_pool. Or maybe if it's not there
        // then it can't possibly be found? Still will be setting variables.
        //
        if (name != "*" && !digits (name))
          return scope_->find (script_->var_pool.insert (move (name)));

        // Handle the $*, $NN special aliases.
        //
        // See the parse_variable_line() for the overall plan.
        //
        // @@ MT: we are potentially changing outer scopes. Could force
        //    lookup before executing tests in each group scope. Poblem is
        //    we don't know which $NN vars will be looked up from inside.
        //    Could we collect all the variable names during the pre-parse
        //    stage? They could be computed.
        //
        //    Or we could set all the non-NULL $NN (i.e., based on the number
        //    of elements in $*).
        //

        // In both cases first thing we do is lookup $*. It should always be
        // defined since we set it on the script's root scope.
        //
        lookup l (scope_->find (script_->cmd_var));
        assert (l.defined ());

        // $* NULL value means it needs to be (re)calculated.
        //
        value& v (const_cast<value&> (*l));
        bool recalc (v.null);

        if (recalc)
        {
          strings s;

          auto append = [&s] (const strings& v)
          {
            s.insert (s.end (), v.begin (), v.end ());
          };

          if (lookup l = scope_->find (script_->test_var))
            s.push_back (cast<path> (l).string ());

          if (lookup l = scope_->find (script_->opts_var))
            append (cast<strings> (l));

          if (lookup l = scope_->find (script_->args_var))
            append (cast<strings> (l));

          v = move (s);
        }

        if (name == "*")
          return l;

        // Use the string type for the $NN variables.
        //
        const variable& var (script_->var_pool.insert<string> (move (name)));

        // We need to look for $NN in the same scope as where we found $*.
        //
        variable_map& vars (const_cast<variable_map&> (*l.vars));

        // If there is already a value and no need to recalculate it, then we
        // are done.
        //
        if (!recalc && (l = vars[var]).defined ())
          return l;

        // Convert the variable name to index we can use on $*.
        //
        unsigned long i;

        try
        {
          i = stoul (var.name);
        }
        catch (const exception&)
        {
          fail (loc) << "invalid $* index " << var.name;
        }

        const strings& s (cast<strings> (v));
        value& nv (vars.assign (var));

        if (i < s.size ())
          nv = s[i];
        else
          nv = nullptr;

        return lookup (nv, vars);
      }

      size_t parser::
      quoted () const
      {
        size_t r (0);

        if (replay_ != replay::play)
          r = lexer_->quoted ();
        else
        {
          // Examine tokens we have replayed since last reset.
          //
          for (size_t i (replay_quoted_); i != replay_i_; ++i)
            if (replay_data_[i].token.quoted)
              ++r;
        }

        return r;
      }

      void parser::
      reset_quoted (token& cur)
      {
        if (replay_ != replay::play)
          lexer_->reset_quoted (cur.quoted ? 1 : 0);
        else
        {
          replay_quoted_ = replay_i_ - 1;

          // Must be the same token.
          //
          assert (replay_data_[replay_quoted_].token.quoted == cur.quoted);
        }
      }
    }
  }
}
