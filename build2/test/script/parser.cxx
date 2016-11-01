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

        id_map idm;

        script_ = &s;
        runner_ = nullptr;
        group_ = script_;
        id_map_ = &idm;
        scope_ = nullptr;

        // Start location of the implied script group is the beginning of the
        // file. End location -- end of the file.
        //
        group_->start_loc_ = location (path_, 1, 1);

        token t (pre_parse_scope_body ());

        if (t.type != type::eos)
          fail (t) << "stray " << t;

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
        id_map_ = nullptr;
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
          // Start lexing each line recognizing leading ':+-{}'.
          //
          mode (lexer_mode::first_token);
          tt = peek ();

          // Handle description.
          //
          optional<description> d;
          if (tt == type::colon)
            d = pre_parse_description (t, tt);

          // Determine the line type by peeking at the first token.
          //
          switch (tt)
          {
          case type::eos:
          case type::rcbrace:
            {
              next (t, tt);

              if (d)
                fail (t) << "description before " << t;

              return t;
            }
          case type::lcbrace:
            {
              // Nested scope.
              //
              next (t, tt); // Get '{'.
              const location sl (get_location (t));

              if (next (t, tt) != type::newline)
                fail (t) << "expected newline after '{'";

              // Push group. If there is no user-supplied id, use the line
              // number as the scope id.
              //
              const string& id (d && !d->id.empty ()
                                ? d->id
                                : insert_id (to_string (sl.line), sl));
              id_map idm;
              unique_ptr<group> g (new group (id, *group_));

              id_map* om (id_map_);
              id_map_ = &idm;

              group* og (group_);
              group_ = g.get ();

              group_->desc = move (d);

              group_->start_loc_ = sl;
              token e (pre_parse_scope_body ());
              group_->end_loc_ = get_location (e);

              // Pop group.
              //
              group_ = og;
              id_map_ = om;

              // Drop empty scopes.
              //
              if (!g->empty ())
              {
                // See if this turned out to be an explicit test scope. An
                // explicit test scope contains a single test, only variable
                // assignments in setup and nothing in teardown. Plus only
                // test or scope (but not both) can have a description.
                //
                auto& sc (g->scopes);
                auto& su (g->setup_);
                auto& td (g->tdown_);

                test* t;
                if (sc.size () == 1 &&
                    (t = dynamic_cast<test*> (sc.back ().get ())) != nullptr &&
                    find_if (
                      su.begin (), su.end (),
                      [] (const line& l)
                      {
                        return l.type != line_type::variable;
                      }) == su.end () &&
                    td.empty () &&
                    (!g->desc || !t->desc))
                {
                  // It would have been nice to reuse the test object and only
                  // throw aways the group. However, the merged scope may have
                  // to use id_path/wd_path of the group. So to keep things
                  // simple we are going to throw away both and create a new
                  // test object.
                  //
                  // Decide whose id to use. We use the group's unless there
                  // is a user-provided one for the test (note that they
                  // cannot be both user-provided since only one can have a
                  // description). If we are using the test's then we also
                  // have to insert it into the outer scope. Good luck getting
                  // its location.
                  //
                  string id;
                  if (t->desc && !t->desc->id.empty ())
                  {
                    // In the id map of the group we should have exactly one
                    // entry -- the one for the test id. That's where we will
                    // get the location.
                    //
                    assert (idm.size () == 1);
                    id = insert_id (t->desc->id, idm.begin ()->second);
                  }
                  else
                    id = g->id_path.leaf ().string ();

                  unique_ptr<test> m (new test (id, *group_));

                  // Move the description (again cannot be both).
                  //
                  if      (g->desc) m->desc = move (g->desc);
                  else if (t->desc) m->desc = move (t->desc);

                  // Merge the lines of the group and the test.
                  //
                  if (su.empty ())
                    m->tests_ = move (t->tests_);
                  else
                  {
                    m->tests_ = move (su); // Should come first.
                    m->tests_.insert (m->tests_.end (),
                                      make_move_iterator (t->tests_.begin ()),
                                      make_move_iterator (t->tests_.end ()));
                  }

                  // Use start/end locations of the outer scope.
                  //
                  m->start_loc_ = g->start_loc_;
                  m->end_loc_ = g->end_loc_;

                  group_->scopes.push_back (move (m));
                }
                else
                  group_->scopes.push_back (move (g));
              }

              if (e.type != type::rcbrace)
                fail (e) << "expected '}' at the end of the scope";

              if (next (t, tt) != type::newline)
                fail (t) << "expected newline after '}'";

              continue;
            }
          default:
            {
              pre_parse_line (t, tt, move (d));
              assert (tt == type::newline);
              break;
            }
          }
        }
      }

      void parser::
      parse_scope_body ()
      {
        size_t li (0);

        auto play = [&li, this] (lines& ls) // Note: destructive to lines.
        {
          token t;
          type tt;

          for (size_t i (0), n (ls.size ()); i != n; ++i)
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
              {
                parse_command_line (t, tt, l.type, ++li);
                break;
              }
            case line_type::test:
              {
                // We use the 0 index to signal that this is the only command.
                // Note that we only do this for test commands.
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

      description parser::
      pre_parse_description (token& t, token_type& tt)
      {
        // Note: token is only peeked at. On return tt is also only peeked at
        // and in the first_token mode.
        //
        assert (tt == type::colon);

        description r;
        location loc (get_location (peeked ()));

        string sp;     // Strip prefix.
        size_t sn (0); // Strip prefix length.

        for (size_t ln (1); tt == type::colon; ++ln)
        {
          next (t, tt); // Get ':'.

          mode (lexer_mode::description_line);
          next (t, tt);

          // If it is empty, then we get newline right away.
          //
          const string& l (tt == type::word ? t.value : string ());

          if (tt == type::word)
            next (t, tt); // Get newline.

          assert (tt == type::newline);

          // If this is the first line, then get the "strip prefix", i.e.,
          // the beginning of the line that contains only whitespaces. If
          // the subsequent lines start with the same prefix, then we strip
          // it.
          //
          if (ln == 1)
          {
            sn = l.find_first_not_of (" \t");
            sp.assign (l, 0, sn == string::npos ? (sn = 0) : sn);
          }

          // Apply strip prefix.
          //
          size_t i (l.compare (0, sn, sp) == 0 ? sn : 0);

          // Strip trailing whitespaces, as a courtesy to the user.
          //
          size_t j (l.find_last_not_of (" \t"));
          j = j != string::npos ? j + 1 : i;

          size_t n (j - i); // [i, j) is our data.

          if (ln == 1)
          {
            // First line. Ignore if it's blank.
            //
            if (n == 0)
              --ln; // Stay as if on the first line.
            else
            {
              // Otherwise, see if it is the id. Failed that we assume it is
              // the summary until we see the next line.
              //
              (l.find_first_of (" \t", i) >= j ? r.id : r.summary).
                assign (l, i, n);
            }
          }
          else if (ln == 2)
          {
            // If this is a blank then whatever we have in id/summary is good.
            // Otherwise, if we have id, then assume this is summary until we
            // see the next line. And if not, then move what we (wrongly)
            // assumed to be the summary to details.
            //
            if (n != 0)
            {
              if (!r.id.empty ())
                r.summary.assign (l, i, n);
              else
              {
                r.details = move (r.summary);
                r.details += '\n';
                r.details.append (l, i, n);

                r.summary.clear ();
              }
            }
          }
          // Don't treat line 3 as special if we have given up on id/summary.
          //
          else if (ln == 3 && r.details.empty ())
          {
            // If this is a blank and we have id and/or summary, then we are
            // good. Otherwise, if we have both, then move what we (wrongly)
            // assumed to be id and summary to details.
            //
            if (n != 0)
            {
              if (!r.id.empty () && !r.summary.empty ())
              {
                r.details = move (r.id);
                r.details += '\n';
                r.details += r.summary;
                r.details += '\n';

                r.id.clear ();
                r.summary.clear ();
              }

              r.details.append (l, i, n);
            }
          }
          else
          {
            if (!r.details.empty ())
              r.details += '\n';

            r.details.append (l, i, n);
          }

          mode (lexer_mode::first_token);
          tt = peek ();
        }

        // Zap trailing newlines in the details.
        //
        size_t p (r.details.find_last_not_of ('\n'));
        if (p != string::npos && ++p != r.details.size ())
          r.details.resize (p);

        // Insert id into the id map if we have one.
        //
        if (!r.id.empty ())
          insert_id (r.id, loc);

        if (r.empty ())
          fail (loc) << "empty description";

        return r;
      }

      optional<description> parser::
      pre_parse_line (token& t, type& tt, optional<description>&& d, lines* ls)
      {
        // Note: token is only peeked at.
        //
        const location ll (get_location (peeked ()));

        // Determine the line type.
        //
        line_type lt;
        switch (tt)
        {
        case type::plus:
        case type::minus:
          {
            // Setup/teardown command.
            //
            lt = (tt == type::plus ? line_type::setup : line_type::tdown);

            next (t, tt);   // Start saving tokens from the next one.
            replay_save ();
            next (t, tt);
            break;
          }
        default:
          {
            // Either test command or variable assignment.
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

        // Pre-parse the line keeping track of whether it ends with a
        // semicolon or contains description.
        //
        pair<bool, optional<description>> lr;

        switch (lt)
        {
        case line_type::variable:
          lr.first = parse_variable_line (t, tt);
          break;
        case line_type::setup:
        case line_type::tdown:
        case line_type::test:
          lr = parse_command_line (t, tt, lt, 0);
          break;
        }

        assert (tt == type::newline);

        // Stop saving and get the tokens.
        //
        line l {lt, replay_data ()};

        // Decide where it goes.
        //
        lines tests;
        if (ls == nullptr)
        {
          switch (lt)
          {
          case line_type::setup:
            {
              if (d)
                fail (ll) << "description before setup command";

              if (!group_->scopes.empty ())
                fail (ll) << "setup command after tests";

              if (!group_->tdown_.empty ())
                fail (ll) << "setup command after teardown";

              ls = &group_->setup_;
              break;
            }
          case line_type::tdown:
            {
              if (d)
                fail (ll) << "description before teardown command";

              ls = &group_->tdown_;
              break;
            }
          case line_type::variable:
            {
              // If there is a semicolon after the variable then we assume
              // it is part of a test (there is no reason to use semicolons
              // after variables in the group scope). Otherwise -- setup or
              // teardown.
              //
              if (!lr.first)
              {
                if (d)
                  fail (ll) << "description before setup/teardown variable";

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

              ls = &tests;
            }
          }
        }

        ls->push_back (move (l));

        // If this command ended with a semicolon, then the next one should
        // go to the same place.
        //
        if (lr.first)
        {
          mode (lexer_mode::first_token);
          tt = peek ();
          const location ll (get_location (peeked ()));

          switch (tt)
          {
          case type::colon:
            fail (ll) << "description inside test";
          case type::eos:
          case type::rcbrace:
          case type::lcbrace:
            fail (ll) << "expected another line after semicolon";
          case type::plus:
            fail (ll) << "setup command in test";
          case type::minus:
            fail (ll) << "teardown command in test";
          default:
            lr.second = pre_parse_line (t, tt, nullopt, ls);
            assert (tt == type::newline); // End of last test line.
          }
        }

        if (lr.second)
        {
          if (d)
            fail (ll) << "both leading and trailing description";

          d = lr.second;
        }


        // Create implicit test scope.
        //
        if (ls == &tests)
        {
          // If there is no user-supplied id, use the line number as the scope
          // id.
          //
          const string& id (d && !d->id.empty ()
                            ? d->id
                            : insert_id (to_string (ll.line), ll));

          unique_ptr<test> p (new test (id, *group_));

          p->desc = move (d);

          p->start_loc_ = ll;
          p->tests_ = move (tests);
          p->end_loc_ = get_location (t);

          group_->scopes.push_back (move (p));
          return nullopt;
        }
        else
          return d;
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

      pair<bool, optional<description>> parser::
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
          out_merge,
          out_string,
          out_document,
          out_file,
          err_merge,
          err_string,
          err_document,
          err_file,
          clean
        };
        pending p (pending::program);
        bool nn (false);  // True if pending here-{str,doc} is "no-newline".
        bool app (false); // True if to append to pending file.
        cleanup_type ct;  // Pending cleanup type.

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
          [&c, &p, &nn, &app, &ct, &hd, this] (string&& w, const location& l)
        {
          auto add_merge = [&l, this] (redirect& r, const string& w, int fd)
          {
            try
            {
              size_t n;
              if (stoi (w, &n) == fd && n == w.size ())
              {
                r.fd = fd;
                return;
              }
            }
            catch (const exception&) {} // Fall through.

            fail (l) << (fd == 1 ? "stderr" : "stdout") << " merge redirect "
                     << "file descriptor must be " << fd;
          };

          auto add_here_str = [&nn] (redirect& r, string&& w)
          {
            if (!nn) w += '\n';
            r.str = move (w);
          };

          auto add_here_end = [&hd, &nn] (redirect& r, string&& w)
          {
            hd.push_back (here_doc {&r, move (w), nn});
          };

          auto parse_path = [&l, this] (string&& w, const char* what) -> path
          {
            try
            {
              path p (move (w));

              if (!p.empty ())
                return p;

              error (l) << "empty " << what;
            }
            catch (const invalid_path& e)
            {
              error (l) << "invalid " << what << " '" << e.path << "'";
            }

            throw failed ();
          };

          auto add_file = [&app, &parse_path] (redirect& r, int fd, string&& w)
          {
            const char* what (nullptr);
            switch (fd)
            {
            case 0: what = "stdin redirect path";  break;
            case 1: what = "stdout redirect path"; break;
            case 2: what = "stderr redirect path"; break;
            }

            r.file.path = parse_path (move (w), what);
            r.file.append = app;
          };

          switch (p)
          {
          case pending::none: c.arguments.push_back (move (w)); break;
          case pending::program:
          {
            c.program = parse_path (move (w), "program path");
            break;
          }

          case pending::out_merge: add_merge (c.out, w, 2); break;
          case pending::err_merge: add_merge (c.err, w, 1); break;

          case pending::in_string:  add_here_str (c.in,  move (w)); break;
          case pending::out_string: add_here_str (c.out, move (w)); break;
          case pending::err_string: add_here_str (c.err, move (w)); break;

          case pending::in_document:  add_here_end (c.in,  move (w)); break;
          case pending::out_document: add_here_end (c.out, move (w)); break;
          case pending::err_document: add_here_end (c.err, move (w)); break;

          case pending::in_file:  add_file (c.in,  0, move (w)); break;
          case pending::out_file: add_file (c.out, 1, move (w)); break;
          case pending::err_file: add_file (c.err, 2, move (w)); break;

          case pending::clean:
            {
              c.cleanups.push_back (
                {ct, parse_path (move (w), "cleanup path")});

              break;
            }
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
          case pending::out_merge:    what = "stdout file descriptor";   break;
          case pending::out_string:   what = "stdout here-string";       break;
          case pending::out_document: what = "stdout here-document end"; break;
          case pending::out_file:     what = "stdout file";              break;
          case pending::err_merge:    what = "stderr file descriptor";   break;
          case pending::err_string:   what = "stderr here-string";       break;
          case pending::err_document: what = "stderr here-document end"; break;
          case pending::err_file:     what = "stderr file";              break;
          case pending::clean:        what = "cleanup path";             break;
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
          case type::out_merge:
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

          case type::out_merge:    rt = redirect_type::merge;         break;

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
          case redirect_type::merge:
            switch (fd)
            {
            case 0: assert (false);         break;
            case 1: p = pending::out_merge; break;
            case 2: p = pending::err_merge; break;
            }
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

        // Set pending cleanup type.
        //
        auto parse_clean = [&p, &ct] (type tt)
        {
          switch (tt)
          {
          case type::clean_always: ct = cleanup_type::always; break;
          case type::clean_maybe:  ct = cleanup_type::maybe;  break;
          case type::clean_never:  ct = cleanup_type::never;  break;
          }

          p = pending::clean;
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
          case type::colon:
          case type::newline:
            {
              done = true;
              break;
            }

          case type::in_pass:
          case type::out_pass:

          case type::in_null:
          case type::out_null:

          case type::out_merge:

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

          case type::clean_always:
          case type::clean_maybe:
          case type::clean_never:
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
                    fail (l) << "expected here-document end marker";

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

              case type::out_merge:

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
                  parse_redirect (t, l);
                  break;
                }

              case type::clean_always:
              case type::clean_maybe:
              case type::clean_never:
                {
                  parse_clean (tt);
                  break;
                }

              default: assert (false); break;
              }

              next (t, tt);
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

                    if (!ops.no_line ())
                    {
                      n += to_string (l.line);
                      n += ':';

                      if (!ops.no_column ())
                      {
                        n += to_string (l.column);
                        n += ':';
                      }
                    }

                    n += " (";
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

                    case type::out_merge:

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

                    case type::clean_always:
                    case type::clean_maybe:
                    case type::clean_never:
                      {
                        parse_clean (tt);
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

        if (!pre_parse_)
        {
          // Verify we don't have anything pending to be filled.
          //
          check_pending (l);

          if (c.out.type == redirect_type::merge &&
              c.err.type == redirect_type::merge)
            fail (l) << "stdout and stderr redirected to each other";
        }

        // While we no longer need to recognize command line operators, we
        // also don't expect a valid test trailer to contain them. So we are
        // going to continue lexing in the script_line mode.
        //
        if (tt == type::equal || tt == type::not_equal)
          c.exit = parse_command_exit (t, tt);

        // Colon and semicolon are only valid in test command lines. Note that
        // we still recognize them lexically, they are just not a valid tokens
        // per the grammar.
        //
        if (tt == type::colon || tt == type::semi)
        {
          switch (lt)
          {
          case line_type::setup: fail (t) << t << " after setup command";
          case line_type::tdown: fail (t) << t << " after teardown command";
          default: break;
          }
        }

        pair<bool, optional<description>> r (false, nullopt);
        if (tt == type::colon)
        {
          // Parse one-line trailing description.
          //
          //@@ Would be nice to omit trailing description from replay.
          //
          const location loc (get_location (t));

          mode (lexer_mode::description_line);
          next (t, tt);

          // If it is empty, then we get newline right away.
          //
          if (tt == type::word)
          {
            string l (move (t.value));
            trim (l); // Strip leading/trailing whitespaces.

            // Decide whether this is id or summary.
            //
            auto& d (*(r.second = description ()));
            (l.find_first_of (" \t") == string::npos ? d.id : d.summary) =
              move (l);

            next (t, tt); // Get newline.
          }

          assert (tt == type::newline);

          if (r.second->empty ())
            fail (loc) << "empty description";
        }
        else
        {
          if (tt == type::semi)
          {
            r.first = true;
            next (t, tt); // Get newline.
          }

          if (tt != type::newline)
            fail (t) << "expected newline instead of " << t;
        }

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

        return r;
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
            fail (t) << "expected exit status instead of '" << ns << "'" <<
              info << "exit status is an unsigned integer less than 256";
        }

        return command_exit {comp, static_cast<uint8_t> (es)};
      }

      string parser::
      parse_here_document (token& t, type& tt, const string& em, bool nn)
      {
        string r;

        // Here-documents can be indented. The leading whitespaces of the end
        // marker line (called strip prefix) determine the indentation. Every
        // other line in the here-document should start with this prefix which
        // is automatically stripped. The only exception is a blank line.
        //
        // The fact that the strip prefix is only known at the end, after
        // seeing all the lines, is rather inconvenient. As a result, the way
        // we implement this is a bit hackish (though there is also something
        // elegant about it): at the end of the pre-parse stage we are going
        // re-examine the sequence of tokens that comprise this here-document
        // and "fix up" the first token of each line by stripping the prefix.
        //
        string sp;

        // Remember the position of the first token in this here-document.
        //
        size_t ri (pre_parse_ ? replay_data_.size () - 1 : 0);

        while (tt != type::eos)
        {
          // Check if this is the end marker. For starters, it should be a
          // single, unquoted word followed by a newline.
          //
          if (tt == type::word && !t.quoted && peek () == type::newline)
          {
            const string& v (t.value);

            size_t vn (v.size ());
            size_t en (em.size ());

            // Then check that it ends with the end marker.
            //
            if (vn >= en && v.compare (vn - en, en, em) == 0)
            {
              // Now check that the prefix only contains whitespaces.
              //
              size_t n (vn - en);

              if (v.find_first_not_of (" \t") >= n)
              {
                assert (pre_parse_ || n == 0); // Should have been stripped.

                if (n != 0)
                  sp.assign (v, 0, n); // Save the strip prefix.

                next (t, tt); // Get the newline.
                break;
              }
            }
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

        if (pre_parse_)
        {
          // Strip the indentation prefix if there is one.
          //
          assert (replay_ == replay::save);

          if (!sp.empty ())
          {
            size_t sn (sp.size ());

            for (; ri != replay_data_.size (); ++ri)
            {
              token& rt (replay_data_[ri].token);

              if (rt.type == type::newline) // Blank
                continue;

              if (rt.type != type::word || rt.value.compare (0, sn, sp) != 0)
                fail (rt) << "unindented here-document line";

              // If the word is equal to the strip prefix then we have to drop
              // the token. Note that simply making it an empty word won't
              // have the same semantics. For instance, it would trigger
              // concatenated expansion.
              //
              if (rt.value.size () == sn)
                replay_data_.erase (replay_data_.begin () + ri);
              else
              {
                rt.value.erase (0, sn);
                rt.column += sn;
                ++ri;
              }

              // Skip until next newline.
              //
              for (; replay_data_[ri].token.type != type::newline; ++ri) ;
            }
          }
        }
        else
        {
          // Add final newline if requested.
          //
          if (!nn)
            r += '\n';
        }

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

      const string& parser::
      insert_id (string id, location l)
      {
        auto p (id_map_->emplace (move (id), move (l)));

        if (!p.second)
          fail (l) << "duplicate id " << p.first->first <<
            info (p.first->second) << "previously used here";

        return p.first->first;
      }
    }
  }
}
