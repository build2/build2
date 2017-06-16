// file      : build2/test/script/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/test/script/parser.hxx>

#include <sstream>

#include <build2/context.hxx> // sched, keep_going

#include <build2/test/script/lexer.hxx>
#include <build2/test/script/runner.hxx>

using namespace std;

namespace build2
{
  namespace test
  {
    namespace script
    {
      using type = token_type;

      // Return true if the string contains only a single digit characters
      // (used to detect the special $N variables).
      //
      static inline bool
      digit (const string& s)
      {
        return s.size () == 1 && butl::digit (s[0]);
      }

      //
      // Pre-parse.
      //

      void parser::
      pre_parse (script& s)
      {
        const path& p (s.script_target.path ());
        assert (!p.empty ()); // Should have been assigned.

        try
        {
          ifdstream ifs (p);
          pre_parse (ifs, s);
        }
        catch (const io_error& e)
        {
          fail << "unable to read testscript " << p << ": " << e << endf;
        }
      }

      void parser::
      pre_parse (istream& is, script& s)
      {
        path_ = &*s.paths_.insert (s.script_target.path ()).first;

        pre_parse_ = true;

        lexer l (is, *path_, lexer_mode::command_line);
        set_lexer (&l);

        id_prefix_.clear ();

        id_map idm;
        include_set ins;

        script_ = &s;
        runner_ = nullptr;
        group_ = script_;
        id_map_ = &idm;
        include_set_ = &ins;
        scope_ = nullptr;

        //@@ PAT TODO: set pbase_?

        // Start location of the implied script group is the beginning of
        // the file. End location -- end of the file.
        //
        group_->start_loc_ = location (path_, 1, 1);

        token t (pre_parse_scope_body ());

        if (t.type != type::eos)
          fail (t) << "stray " << t;

        group_->end_loc_ = get_location (t);
      }

      bool parser::
      pre_parse_demote_group_scope (unique_ptr<scope>& s)
      {
        // See if this turned out to be an explicit test scope. An explicit
        // test scope contains a single test, only variable assignments in
        // setup and nothing in teardown. Plus only the group can have the
        // description. Because we apply this recursively, also disqualify
        // a test scope that has an if-condition.
        //
        // If we have a chain, then all the scopes must be demotable. So we
        // first check if this scope is demotable and if so then recurse for
        // the next in chain.
        //
        group& g (static_cast<group&> (*s));

        auto& sc (g.scopes);
        auto& su (g.setup_);
        auto& td (g.tdown_);

        test* t;
        if (sc.size () == 1                                          &&
            (t = dynamic_cast<test*> (sc.back ().get ())) != nullptr &&
            find_if (
              su.begin (), su.end (),
              [] (const line& l) {
                return l.type != line_type::var;
              }) == su.end ()                                        &&

            td.empty ()                                              &&
            !t->desc                                                 &&
            !t->if_cond_)
        {
          if (g.if_chain != nullptr &&
              !pre_parse_demote_group_scope (g.if_chain))
            return false;

          // It would have been nice to reuse the test object and only throw
          // away the group. However, the merged scope has to use id_path and
          // wd_path of the group. So to keep things simple we are going to
          // throw away both and create a new test object.
          //
          // We always use the group's id since the test cannot have a
          // user-provided one.
          //
          unique_ptr<test> m (new test (g.id_path.leaf ().string (), *group_));

          // Move the description, if-condition, and if-chain.
          //
          m->desc = move (g.desc);
          m->if_cond_ = move (g.if_cond_);
          m->if_chain = move (g.if_chain);

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
          m->start_loc_ = g.start_loc_;
          m->end_loc_ = g.end_loc_;

          s = move (m);
          return true;
        }

        return false;
      }

      token parser::
      pre_parse_scope_body ()
      {
        // enter: next token is first token of scope body
        // leave: rcbrace or eos (returned)

        token t;
        type tt;

        // Parse lines (including nested scopes) until we see '}' or eos.
        //
        for (;;)
        {
          // Start lexing each line recognizing leading '.+-{}'.
          //
          tt = peek (lexer_mode::first_token);

          // Handle description.
          //
          optional<description> d;
          if (tt == type::colon)
            d = pre_parse_leading_description (t, tt);

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

              // First check that we don't have any teardown commands yet.
              // This will detect things like variable assignments between
              // scopes.
              //
              if (!group_->tdown_.empty ())
              {
                location tl (
                  group_->tdown_.back ().tokens.front ().location ());

                fail (sl) << "scope after teardown" <<
                  info (tl) << "last teardown line appears here";
              }

              // If there is no user-supplied id, use the line number
              // (prefixed with include id) as the scope id.
              //
              const string& id (
                d && !d->id.empty ()
                ? d->id
                : insert_id (id_prefix_ + to_string (sl.line), sl));

              unique_ptr<scope> g (pre_parse_scope_block (t, tt, id));
              g->desc = move (d);

              pre_parse_demote_group_scope (g);
              group_->scopes.push_back (move (g));
              continue;
            }
          default:
            {
              pre_parse_line (t, tt, d);
              assert (tt == type::newline);
              break;
            }
          }
        }
      }

      unique_ptr<group> parser::
      pre_parse_scope_block (token& t, type& tt, const string& id)
      {
        // enter: lcbrace
        // leave: newline after rcbrace

        const location sl (get_location (t));

        if (next (t, tt) != type::newline)
          fail (t) << "expected newline after '{'";

        // Push group.
        //
        id_map idm;
        include_set ins;

        unique_ptr<group> g (new group (id, *group_));

        id_map* om (id_map_);
        id_map_ = &idm;

        include_set* os (include_set_);
        include_set_ = &ins;

        group* og (group_);
        group_ = g.get ();

        // Parse body.
        //
        group_->start_loc_ = sl;
        token e (pre_parse_scope_body ());
        group_->end_loc_ = get_location (e);

        // Pop group.
        //
        group_ = og;
        include_set_ = os;
        id_map_ = om;

        if (e.type != type::rcbrace)
          fail (e) << "expected '}' at the end of the scope";

        if (next (t, tt) != type::newline)
          fail (t) << "expected newline after '}'";

        return g;
      }

      // Parse a logical line (as well as scope-if since the only way to
      // recognize it is to parse the if line).
      //
      // If one is true then only parse one line returning an indication of
      // whether the line ended with a semicolon.
      //
      bool parser::
      pre_parse_line (token& t, type& tt,
                      optional<description>& d,
                      lines* ls,
                      bool one)
      {
        // enter: next token is peeked at (type in tt)
        // leave: newline

        // Note: token is only peeked at.
        //
        const location ll (get_location (peeked ()));

        // Determine the line type/start token.
        //
        line_type lt;
        type st (type::eos);

        switch (tt)
        {
        case type::dot:
          {
            // Directive.
            //
            next (t, tt); // Skip dot.
            next (t, tt); // Get the directive name.

            if (tt != type::word || t.qtype != quote_type::unquoted)
              fail (t) << "expected directive name instead of " << t;

            // Make sure we are not inside a test (i.e., after semi).
            //
            if (ls != nullptr)
              fail (ll) << "directive after ';'";

            const string& n (t.value);

            if (n == "include")
              pre_parse_directive (t, tt);
            else
              fail (t) << "unknown directive '" << n << "'";

            assert (tt == type::newline);
            return false;
          }
        case type::plus:
        case type::minus:
          {
            // Setup/teardown command.
            //
            st = tt;

            next (t, tt);   // Start saving tokens from the next one.
            replay_save ();
            next (t, tt);

            // See if this is a special command.
            //
            lt = line_type::cmd; // Default.

            if (tt == type::word && t.qtype == quote_type::unquoted)
            {
              const string& n (t.value);

              if      (n == "if")  lt = line_type::cmd_if;
              else if (n == "if!") lt = line_type::cmd_ifn;
            }

            break;
          }
        default:
          {
            // Either variable assignment or test command.
            //
            replay_save (); // Start saving tokens from the current one.
            next (t, tt);

            // Decide whether this is a variable assignment or a command.
            //
            // It is an assignment if the first token is an unquoted name and
            // the next token is an assign/append/prepend operator. Assignment
            // to a computed variable name must use the set builtin.
            //
            // Note also thatspecial commands take precedence over variable
            // assignments.
            //
            lt = line_type::cmd; // Default.

            if (tt == type::word && t.qtype == quote_type::unquoted)
            {
              const string& n (t.value);

              if      (n == "if")    lt = line_type::cmd_if;
              else if (n == "if!")   lt = line_type::cmd_ifn;
              else if (n == "elif")  lt = line_type::cmd_elif;
              else if (n == "elif!") lt = line_type::cmd_elifn;
              else if (n == "else")  lt = line_type::cmd_else;
              else if (n == "end")   lt = line_type::cmd_end;
              else
              {
                // Switch the recognition of leading variable assignments for
                // the next token. This is safe to do because we know we
                // cannot be in the quoted mode (since the current token is
                // not quoted).
                //
                type p (peek (lexer_mode::second_token));

                if (p == type::assign  ||
                    p == type::prepend ||
                    p == type::append)
                {
                  lt = line_type::var;
                  st = p;
                }
              }
            }

            break;
          }
        }

        // Pre-parse the line keeping track of whether it ends with a semi.
        //
        bool semi (false);

        line ln;
        switch (lt)
        {
        case line_type::var:
          {
            // Check if we are trying to modify any of the special aliases
            // ($*, $N, $~, $@).
            //
            string& n (t.value);

            if (n == "*" || n == "~" || n == "@" || digit (n))
              fail (t) << "attempt to set '" << n << "' variable directly";

            // Pre-enter the variables now while we are executing serially.
            // Once parallel, it becomes a lot harder to do.
            //
            ln.var = &script_->var_pool.insert (move (n));

            next (t, tt); // Assignment kind.
            parse_variable_line (t, tt);

            semi = (tt == type::semi);

            if (tt == type::semi)
              next (t, tt);

            if (tt != type::newline)
              fail (t) << "expected newline instead of " << t;

            break;
          }
        case line_type::cmd_if:
        case line_type::cmd_ifn:
        case line_type::cmd_elif:
        case line_type::cmd_elifn:
        case line_type::cmd_else:
        case line_type::cmd_end:
          {
            next (t, tt); // Skip to start of command.
            // Fall through.
          }
        case line_type::cmd:
          {
            pair<command_expr, here_docs> p;

            if (lt != line_type::cmd_else && lt != line_type::cmd_end)
              p = parse_command_expr (t, tt);

            // Colon and semicolon are only valid in test command lines and
            // after 'end' in if-else. Note that we still recognize them
            // lexically, they are just not valid tokens per the grammar.
            //
            if (tt != type::newline)
            {
              if (lt != line_type::cmd && lt != line_type::cmd_end)
                fail (t) << "expected newline instead of " << t;

              switch (st)
              {
              case type::plus:  fail (t) << t << " after setup command";
              case type::minus: fail (t) << t << " after teardown command";
              }
            }

            switch (tt)
            {
            case type::colon:
              {
                if (d)
                  fail (ll) << "both leading and trailing descriptions";

                d = parse_trailing_description (t, tt);
                break;
              }
            case type::semi:
              {
                semi = true;
                next (t, tt); // Get newline.
                break;
              }
            }

            if (tt != type::newline)
              fail (t) << "expected newline instead of " << t;

            parse_here_documents (t, tt, p);
            break;
          }
        }

        assert (tt == type::newline);

        // Stop saving and get the tokens.
        //
        lines ls_data;

        if (ls == nullptr)
          ls = &ls_data;

        ln.type = lt;
        ln.tokens = replay_data ();
        ls->push_back (move (ln));

        if (lt == line_type::cmd_if || lt == line_type::cmd_ifn)
        {
          semi = pre_parse_if_else (t, tt, d, *ls);

          // If this turned out to be scope-if, then ls is empty, semi is
          // false, and none of the below logic applies.
          //
          if (ls->empty ())
            return semi;
        }

        // Unless we were told where to put it, decide where it actually goes.
        //
        if (ls == &ls_data)
        {
          // First pre-check variable and variable-if: by themselves (i.e.,
          // without a trailing semicolon) they are treated as either setup or
          // teardown without plus/minus. Also handle illegal line types.
          //
          switch (lt)
          {
          case line_type::cmd_elif:
          case line_type::cmd_elifn:
          case line_type::cmd_else:
          case line_type::cmd_end:
            {
              fail (ll) << lt << " without preceding 'if'";
            }
          case line_type::cmd_if:
          case line_type::cmd_ifn:
            {
              // See if this is a variable-only command-if.
              //
              if (find_if (ls_data.begin (), ls_data.end (),
                           [] (const line& l) {
                             return l.type == line_type::cmd;
                           }) != ls_data.end ())
                break;

              // Fall through.
            }
          case line_type::var:
            {
              // If there is a semicolon after the variable then we assume
              // it is part of a test (there is no reason to use semicolons
              // after variables in the group scope). Otherwise -- setup or
              // teardown.
              //
              if (!semi)
              {
                if (d)
                {
                  if (lt == line_type::var)
                    fail (ll) << "description before setup/teardown variable";
                  else
                    fail (ll) << "description before/after setup/teardown "
                              << "variable-if";
                }

                // If we don't have any nested scopes or teardown commands,
                // then we assume this is a setup, otherwise -- teardown.
                //
                ls = group_->scopes.empty () && group_->tdown_.empty ()
                  ? &group_->setup_
                  : &group_->tdown_;
              }
              break;
            }
          default:
            break;
          }

          // If pre-check didn't change the destination, then it's a test.
          //
          if (ls == &ls_data)
          {
            switch (st)
            {
              // Setup.
              //
            case type::plus:
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

              // Teardown.
              //
            case type::minus:
              {
                if (d)
                  fail (ll) << "description before teardown command";

                ls = &group_->tdown_;
                break;
              }

              // Test command or variable.
              //
            default:
              {
                // First check that we don't have any teardown commands yet.
                // This will detect things like variable assignments between
                // tests.
                //
                if (!group_->tdown_.empty ())
                {
                  location tl (
                    group_->tdown_.back ().tokens.front ().location ());

                  fail (ll) << "test after teardown" <<
                    info (tl) << "last teardown line appears here";
                }
                break;
              }
            }
          }

          // If the destination changed, then move the data over.
          //
          if (ls != &ls_data)
            ls->insert (ls->end (),
                        make_move_iterator (ls_data.begin ()),
                        make_move_iterator (ls_data.end ()));
        }

        // If this command ended with a semicolon, then the next one should
        // go to the same place.
        //
        if (semi && !one)
        {
          tt = peek (lexer_mode::first_token);
          const location ll (get_location (peeked ()));

          switch (tt)
          {
          case type::colon:
            fail (ll) << "description inside test";
          case type::eos:
          case type::rcbrace:
          case type::lcbrace:
            fail (ll) << "expected another line after ';'";
          case type::plus:
            fail (ll) << "setup command in test";
          case type::minus:
            fail (ll) << "teardown command in test";
          default:
            semi = pre_parse_line (t, tt, d, ls);
            assert (tt == type::newline); // End of last test line.
          }
        }

        // If this is a test then create implicit test scope.
        //
        if (ls == &ls_data)
        {
          // If there is no user-supplied id, use the line number (prefixed
          // with include id) as the scope id.
          //
          const string& id (
            d && !d->id.empty ()
            ? d->id
            : insert_id (id_prefix_ + to_string (ll.line), ll));

          unique_ptr<test> p (new test (id, *group_));

          p->desc = move (d);

          p->start_loc_ = ll;
          p->tests_ = move (ls_data);
          p->end_loc_ = get_location (t);

          group_->scopes.push_back (move (p));
        }

        return semi;
      }

      bool parser::
      pre_parse_if_else (token& t, type& tt,
                         optional<description>& d,
                         lines& ls)
      {
        // enter: <newline> (previous line)
        // leave: <newline>

        tt = peek (lexer_mode::first_token);

        return tt == type::lcbrace
          ? pre_parse_if_else_scope (t, tt, d, ls)
          : pre_parse_if_else_command (t, tt, d, ls);
      }

      bool parser::
      pre_parse_if_else_scope (token& t, type& tt,
                               optional<description>& d,
                               lines& ls)
      {
        // enter: peeked token of next line (lcbrace)
        // leave: newline

        assert (ls.size () == 1); // The if/if! line.

        // Use if/if! as the entire scope chain location.
        //
        const location sl (ls.back ().tokens.front ().location ());

        // First check that we don't have any teardown commands yet. This
        // will detect things like variable assignments between scopes.
        //
        if (!group_->tdown_.empty ())
        {
          location tl (
            group_->tdown_.back ().tokens.front ().location ());

          fail (sl) << "scope after teardown" <<
            info (tl) << "last teardown line appears here";
        }

        // If there is no user-supplied id, use the line number (prefixed with
        // include id) as the scope id. Note that we use the same id for all
        // scopes in the chain.
        //
        const string& id (
          d && !d->id.empty ()
          ? d->id
          : insert_id (id_prefix_ + to_string (sl.line), sl));

        unique_ptr<scope> root;

        // Parse the if-else scope chain.
        //
        line_type bt (line_type::cmd_if); // Current block.

        for (unique_ptr<scope>* ps (&root);; ps = &(*ps)->if_chain)
        {
          next (t, tt); // Get '{'.

          {
            unique_ptr<group> g (pre_parse_scope_block (t, tt, id));

            // If-condition.
            //
            g->if_cond_ = move (ls.back ());
            ls.clear ();

            // Description. For now we just duplicate it through the entire
            // chain.
            //
            g->desc = (ps == &root ? move (d) : root->desc);

            *ps = move (g);
          }

          // See if what comes next is another chain element.
          //
          line_type lt (line_type::cmd_end);

          type pt (peek (lexer_mode::first_token));
          const token& p (peeked ());
          const location ll (get_location (p));

          if (pt == type::word && p.qtype == quote_type::unquoted)
          {
            if      (p.value == "elif")  lt = line_type::cmd_elif;
            else if (p.value == "elif!") lt = line_type::cmd_elifn;
            else if (p.value == "else")  lt = line_type::cmd_else;
          }

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

          // Parse just the condition line using pre_parse_line() in the "one"
          // mode and into ls so that it is naturally picked up as if_cond_ on
          // the next iteration.
          //
          optional<description> td;
          bool semi (pre_parse_line (t, (tt = pt), td, &ls, true));
          assert (ls.size () == 1 && ls.back ().type == lt);
          assert (tt == type::newline);

          // For any of these lines trailing semi or description is illegal.
          //
          // @@ Not the exact location of semi/colon.
          //
          if (semi)
            fail (ll) << "';' after " << lt;

          if (td)
            fail (ll) << "description after " << lt;

          // Make sure what comes next is another scope.
          //
          tt = peek (lexer_mode::first_token);

          if (tt != type::lcbrace)
            fail (ll) << "expected scope after " << lt;

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

        pre_parse_demote_group_scope (root);
        group_->scopes.push_back (move (root));
        return false; // We never end with a semi.
      }

      bool parser::
      pre_parse_if_else_command (token& t, type& tt,
                                 optional<description>& d,
                                 lines& ls)
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

          switch (tt)
          {
          case type::colon:
            fail (ll) << "description inside " << bt;
          case type::eos:
          case type::rcbrace:
          case type::lcbrace:
            fail (ll) << "expected closing 'end'";
          case type::plus:
            fail (ll) << "setup command inside " << bt;
          case type::minus:
            fail (ll) << "teardown command inside " << bt;
          }

          // Parse one line. Note that this one line can still be multiple
          // lines in case of if-else. In this case we want to view it as
          // cmd_if, not cmd_end. Thus remember the start position of the
          // next logical line.
          //
          size_t i (ls.size ());

          optional<description> td;
          bool semi (pre_parse_line (t, tt, td, &ls, true));
          assert (tt == type::newline);

          line_type lt (ls[i].type);

          // First take care of 'end'.
          //
          if (lt == line_type::cmd_end)
          {
            if (td)
            {
              if (d)
                fail (ll) << "both leading and trailing descriptions";

              d = move (td);
            }

            return semi;
          }

          // For any other line trailing semi or description is illegal.
          //
          // @@ Not the exact location of semi/colon.
          //
          if (semi)
            fail (ll) << "';' inside " << bt;

          if (td)
            fail (ll) << "description inside " << bt;

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
      pre_parse_directive (token& t, type& tt)
      {
        // enter: directive name
        // leave: newline

        string d (t.value);
        location l (get_location (t));
        next (t, tt);

        // Suspend pre-parsing since we want to really parse the line, with
        // expansion, etc. Also parse the whole line in one go.
        //
        names args;

        if (tt != type::newline)
        {
          pre_parse_ = false;
          args = parse_names (t, tt,
                              pattern_mode::expand,
                              false,
                              "directive argument",
                              nullptr);
          pre_parse_ = true;
        }

        if (tt != type::newline)
          fail (t) << t << " after directive";

        if (d == "include")
          pre_parse_include_line (move (args), move (l));
        else
          assert (false); // Unhandled directive.
      }

      void parser::
      pre_parse_include_line (names args, location dl)
      {
        auto i (args.begin ());

        // Process options.
        //
        bool once (false);
        for (; i != args.end () && i->simple (); ++i)
        {
          if (i->value == "--once")
            once = true;
          else
            break;
        }

        // Process arguments.
        //
        auto include = [&dl, once, this] (string n) // throw invalid_path
        {
          // It may be tempting to use relative paths in diagnostics but it
          // most likely will be misguided.
          //
          auto enter_path = [this] (string n) -> const path&
          {
            path p (move (n));

            if (p.relative ())
              p = path_->directory () / p;

            p.normalize ();

            return *script_->paths_.insert (move (p)).first;
          };

          const path& p (enter_path (move (n)));

          if (include_set_->insert (p).second || !once)
          {
            try
            {
              ifdstream ifs (p);
              lexer l (ifs, p, lexer_mode::command_line);

              const path* op (path_);
              path_ = &p;

              lexer* ol (lexer_);
              set_lexer (&l);

              string oip (id_prefix_);
              id_prefix_ += to_string (dl.line);
              id_prefix_ += '-';
              id_prefix_ += p.leaf ().base ().string ();
              id_prefix_ += '-';

              token t (pre_parse_scope_body ());

              if (t.type != type::eos)
                fail (t) << "stray " << t;

              id_prefix_ = oip;
              set_lexer (ol);
              path_ = op;
            }
            catch (const io_error& e)
            {
              fail (dl) << "unable to read testscript " << p << ": " << e;
            }
          }
        };

        for (; i != args.end (); ++i)
        {
          name& n (*i);

          try
          {
            if (n.simple () && !n.empty ())
            {
              include (move (n.value));
              continue;
            }
          }
          catch (const invalid_path&) {} // Fall through.

          diag_record dr (fail (dl));
          dr << "invalid testscript include path ";
          to_stream (dr.os, n, true); // Quote.
        }
      }

      description parser::
      pre_parse_leading_description (token& t, type& tt)
      {
        // enter: peeked at colon (type in tt)
        // leave: peeked at in the first_token mode (type in tt)

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

          tt = peek (lexer_mode::first_token);
        }

        // Zap trailing newlines in the details.
        //
        size_t p (r.details.find_last_not_of ('\n'));
        if (p != string::npos && ++p != r.details.size ())
          r.details.resize (p);

        if (r.empty ())
          fail (loc) << "empty description";

        // Insert id into the id map if we have one.
        //
        if (!r.id.empty ())
          insert_id (r.id, loc);

        return r;
      }

      description parser::
      parse_trailing_description (token& t, type& tt)
      {
        // enter: colon
        // leave: newline

        // Parse one-line trailing description.
        //
        description r;

        // @@ Would be nice to omit trailing description from replay.
        //
        const location loc (get_location (t));

        mode (lexer_mode::description_line);
        next (t, tt);

        // If it is empty, then we will get newline right away.
        //
        if (tt == type::word)
        {
          string l (move (t.value));
          trim (l); // Strip leading/trailing whitespaces.

          // Decide whether this is id or summary.
          //
          (l.find_first_of (" \t") == string::npos ? r.id : r.summary) =
            move (l);

          next (t, tt); // Get newline.
        }

        assert (tt == type::newline); // Lexer mode invariant.

        if (r.empty ())
          fail (loc) << "empty description";

        // Insert id into the id map if we have one.
        //
        if (pre_parse_ && !r.id.empty ())
          insert_id (r.id, loc);

        return r;
      }

      value parser::
      parse_variable_line (token& t, type& tt)
      {
        // enter: assignment
        // leave: newline or semi

        // We cannot reuse the value mode since it will recognize { which we
        // want to treat as a literal.
        //
        mode (lexer_mode::variable_line);
        next (t, tt);

        // Parse value attributes if any. Note that it's ok not to have
        // anything after the attributes (e.g., foo=[null]).
        //
        attributes_push (t, tt, true);

        // @@ PAT: Should we expand patterns? Note that it will only be
        // simple ones since we have disabled {}. Also, what would be the
        // pattern base directory?
        //
        return tt != type::newline && tt != type::semi
          ? parse_value (t, tt,
                         pattern_mode::ignore,
                         "variable value",
                         nullptr)
          : value (names ());
      }

      command_expr parser::
      parse_command_line (token& t, type& tt)
      {
        // enter: first token of the command line
        // leave: <newline>

        // Note: this one is only used during execution.

        pair<command_expr, here_docs> p (parse_command_expr (t, tt));

        switch (tt)
        {
        case type::colon: parse_trailing_description (t, tt); break;
        case type::semi: next (t, tt); break; // Get newline.
        }

        assert (tt == type::newline);

        parse_here_documents (t, tt, p);
        assert (tt == type::newline);

        return move (p.first);
      }

      // Parse the regular expression representation (non-empty string value
      // framed with introducer characters and optionally followed by flag
      // characters from the {di} set, for example '/foo/id') into
      // components. Also return end-of-parsing position if requested,
      // otherwise treat any unparsed characters left as an error.
      //
      struct regex_parts
      {
        string value;
        char   intro;
        string flags; // Combination of characters from {di} set.

        // Create a special empty object.
        //
        regex_parts (): intro ('\0') {}

        regex_parts (string v, char i, string f)
            : value (move (v)), intro (i), flags (move (f)) {}
      };

      static regex_parts
      parse_regex (const string& s,
                   const location& l,
                   const char* what,
                   size_t* end = nullptr)
      {
        if (s.empty ())
          fail (l) << "no introducer character in " << what;

        size_t p (s.find (s[0], 1)); // Find terminating introducer.

        if (p == string::npos)
          fail (l) << "no closing introducer character in " << what;

        size_t rn (p - 1); // Regex length.
        if (rn == 0)
          fail (l) << what << " is empty";

        // Find end-of-flags position.
        //
        size_t fp (++p); // Save flags starting position.
        for (char c; (c = s[p]) == 'd' || c == 'i'; ++p) ;

        // If string end is not reached then report invalid flags, unless
        // end-of-parsing position is requested (which means regex is just a
        // prefix).
        //
        if (s[p] != '\0' && end == nullptr)
          fail (l) << "junk at the end of " << what;

        if (end != nullptr)
          *end = p;

        return regex_parts (string (s, 1, rn), s[0], string (s, fp, p - fp));
      }

      pair<command_expr, parser::here_docs> parser::
      parse_command_expr (token& t, type& tt)
      {
        // enter: first token of the command line
        // leave: <newline>

        command_expr expr;

        // OR-ed to an implied false for the first term.
        //
        expr.push_back ({expr_operator::log_or, command_pipe ()});

        command c; // Command being assembled.

        // Make sure the command makes sense.
        //
        auto check_command = [&c, this] (const location& l, bool last)
        {
          if (c.out.type == redirect_type::merge &&
              c.err.type == redirect_type::merge)
            fail (l) << "stdout and stderr redirected to each other";

          if (!last && c.out.type != redirect_type::none)
            fail (l) << "stdout is both redirected and piped";
        };

        // Check that the introducer character differs from '/' if the
        // portable path modifier is specified. Must be called before
        // parse_regex() (see below) to make sure its diagnostics is
        // meaningful.
        //
        // Note that the portable path modifier assumes '/' to be a valid
        // regex character and so makes it indistinguishable from the
        // terminating introducer.
        //
        auto check_regex_mod = [this] (const string& mod,
                                       const string& re,
                                       const location& l,
                                       const char* what)
        {
          // Handles empty regex properly.
          //
          if (mod.find ('/') != string::npos && re[0] == '/')
            fail (l) << "portable path modifier and '/' introducer in "
                     << what;
        };

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
          out_str_regex,
          out_document,
          out_doc_regex,
          out_file,
          err_merge,
          err_string,
          err_str_regex,
          err_document,
          err_doc_regex,
          err_file,
          clean
        };
        pending p (pending::program);
        string mod;   // Modifiers for pending in_* and out_* positions.
        here_docs hd; // Expected here-documents.

        // Add the next word to either one of the pending positions or to
        // program arguments by default.
        //
        auto add_word = [&c, &p, &mod, &check_regex_mod, this] (
          string&& w, const location& l)
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

          auto add_here_str = [] (redirect& r, string&& w)
          {
            if (r.modifiers.find (':') == string::npos)
              w += '\n';
            r.str = move (w);
          };

          auto add_here_str_regex = [&l, &check_regex_mod] (
            redirect& r, int fd, string&& w)
          {
            const char* what (nullptr);
            switch (fd)
            {
            case 1: what = "stdout regex redirect"; break;
            case 2: what = "stderr regex redirect"; break;
            }

            check_regex_mod (r.modifiers, w, l, what);

            regex_parts rp (parse_regex (w, l, what));

            regex_lines& re (r.regex);
            re.intro = rp.intro;

            re.lines.emplace_back (
              l.line, l.column, move (rp.value), move (rp.flags));

            // Add final blank line unless suppressed.
            //
            // Note that the position is synthetic, but that's ok as we don't
            // expect any diagnostics to refer this line.
            //
            if (r.modifiers.find (':') == string::npos)
              re.lines.emplace_back (l.line, l.column, string (), false);
          };

          auto parse_path = [&l, this] (string&& w, const char* what) -> path
          {
            try
            {
              path p (move (w));

              if (!p.empty ())
              {
                p.normalize ();
                return p;
              }

              fail (l) << "empty " << what << endf;
            }
            catch (const invalid_path& e)
            {
              fail (l) << "invalid " << what << " '" << e.path << "'" << endf;
            }
          };

          auto add_file = [&parse_path] (redirect& r, int fd, string&& w)
          {
            const char* what (nullptr);
            switch (fd)
            {
            case 0: what = "stdin redirect path";  break;
            case 1: what = "stdout redirect path"; break;
            case 2: what = "stderr redirect path"; break;
            }

            r.file.path = parse_path (move (w), what);
          };

          switch (p)
          {
          case pending::none: c.arguments.push_back (move (w)); break;
          case pending::program:
            c.program = parse_path (move (w), "program path");
            break;

          case pending::out_merge: add_merge (c.out, w, 2); break;
          case pending::err_merge: add_merge (c.err, w, 1); break;

          case pending::in_string:  add_here_str (c.in,  move (w)); break;
          case pending::out_string: add_here_str (c.out, move (w)); break;
          case pending::err_string: add_here_str (c.err, move (w)); break;

          case pending::out_str_regex:
            {
              add_here_str_regex (c.out, 1, move (w));
              break;
            }
          case pending::err_str_regex:
            {
              add_here_str_regex (c.err, 2, move (w));
              break;
            }

            // These are handled specially below.
            //
          case pending::in_document:
          case pending::out_document:
          case pending::err_document:
          case pending::out_doc_regex:
          case pending::err_doc_regex: assert (false); break;

          case pending::in_file:  add_file (c.in,  0, move (w)); break;
          case pending::out_file: add_file (c.out, 1, move (w)); break;
          case pending::err_file: add_file (c.err, 2, move (w)); break;

          case pending::clean:
            {
              cleanup_type t;
              switch (mod[0]) // Ok, if empty
              {
              case '!': t = cleanup_type::never;  break;
              case '?': t = cleanup_type::maybe;  break;
              default:  t = cleanup_type::always; break;
              }

              c.cleanups.push_back (
                {t, parse_path (move (w), "cleanup path")});
              break;
            }
          }

          p = pending::none;
          mod.clear ();
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

          case pending::out_str_regex:
            {
              what = "stdout here-string regex";
              break;
            }
          case pending::err_str_regex:
            {
              what = "stderr here-string regex";
              break;
            }
          case pending::out_doc_regex:
            {
              what = "stdout here-document regex end";
              break;
            }
          case pending::err_doc_regex:
            {
              what = "stderr here-document regex end";
              break;
            }
          }

          if (what != nullptr)
            fail (l) << "missing " << what;
        };

        // Parse the redirect operator.
        //
        auto parse_redirect =
          [&c, &expr, &p, &mod, this] (token& t, const location& l)
        {
          // Our semantics is the last redirect seen takes effect.
          //
          assert (p == pending::none && mod.empty ());

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
          case type::in_doc:
          case type::in_file:
            {
              if ((fd = fd == 3 ? 0 : fd) != 0)
                fail (l) << "invalid in redirect file descriptor " << fd;

              if (!expr.back ().pipe.empty ())
                fail (l) << "stdin is both piped and redirected";

              break;
            }
          case type::out_pass:
          case type::out_null:
          case type::out_trace:
          case type::out_merge:
          case type::out_str:
          case type::out_doc:
          case type::out_file_cmp:
          case type::out_file_ovr:
          case type::out_file_app:
            {
              if ((fd = fd == 3 ? 1 : fd) == 0)
                fail (l) << "invalid out redirect file descriptor " << fd;

              break;
            }
          }

          mod = move (t.value);

          redirect_type rt (redirect_type::none);
          switch (tt)
          {
          case type::in_pass:
          case type::out_pass:  rt = redirect_type::pass;  break;

          case type::in_null:
          case type::out_null:  rt = redirect_type::null;  break;

          case type::out_trace: rt = redirect_type::trace; break;

          case type::out_merge: rt = redirect_type::merge; break;

          case type::in_str:
          case type::out_str:
            {
              bool re (mod.find ('~') != string::npos);
              assert (tt == type::out_str || !re);

              rt = re
                ? redirect_type::here_str_regex
                : redirect_type::here_str_literal;

              break;
            }

          case type::in_doc:
          case type::out_doc:
            {
              bool re (mod.find ('~') != string::npos);
              assert (tt == type::out_doc || !re);

              rt = re
                ? redirect_type::here_doc_regex
                : redirect_type::here_doc_literal;

              break;
            }

          case type::in_file:
          case type::out_file_cmp:
          case type::out_file_ovr:
          case type::out_file_app: rt = redirect_type::file; break;
          }

          redirect& r (fd == 0 ? c.in : fd == 1 ? c.out : c.err);
          r = redirect (rt);

          // Don't move as still may be used for pending here-document end
          // marker processing.
          //
          r.modifiers = mod;

          switch (rt)
          {
          case redirect_type::none:
          case redirect_type::pass:
          case redirect_type::null:
          case redirect_type::trace:
            break;
          case redirect_type::merge:
            switch (fd)
            {
            case 0: assert (false);         break;
            case 1: p = pending::out_merge; break;
            case 2: p = pending::err_merge; break;
            }
            break;
          case redirect_type::here_str_literal:
            switch (fd)
            {
            case 0: p = pending::in_string;  break;
            case 1: p = pending::out_string; break;
            case 2: p = pending::err_string; break;
            }
            break;
          case redirect_type::here_str_regex:
            switch (fd)
            {
            case 0: assert (false);             break;
            case 1: p = pending::out_str_regex; break;
            case 2: p = pending::err_str_regex; break;
            }
            break;
          case redirect_type::here_doc_literal:
            switch (fd)
            {
            case 0: p = pending::in_document;  break;
            case 1: p = pending::out_document; break;
            case 2: p = pending::err_document; break;
            }
            break;
          case redirect_type::here_doc_regex:
            switch (fd)
            {
            case 0: assert (false);             break;
            case 1: p = pending::out_doc_regex; break;
            case 2: p = pending::err_doc_regex; break;
            }
            break;
          case redirect_type::file:
            switch (fd)
            {
            case 0: p = pending::in_file;  break;
            case 1: p = pending::out_file; break;
            case 2: p = pending::err_file; break;
            }

            // Also sets for stdin, but this is harmless.
            //
            r.file.mode = tt == type::out_file_ovr
              ? redirect_fmode::overwrite
              : (tt == type::out_file_app
                 ? redirect_fmode::append
                 : redirect_fmode::compare);

            break;

          case redirect_type::here_doc_ref: assert (false); break;
          }
        };

        // Set pending cleanup type.
        //
        auto parse_clean = [&p, &mod] (token& t)
        {
          p = pending::clean;
          mod = move (t.value);
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
          case type::semi:
          case type::colon:
          case type::newline:
            {
              done = true;
              break;
            }

          case type::equal:
          case type::not_equal:
            {
              if (!pre_parse_)
                check_pending (l);

              c.exit = parse_command_exit (t, tt);

              // Only a limited set of things can appear after the exit status
              // so we check this here.
              //
              switch (tt)
              {
              case type::semi:
              case type::colon:
              case type::newline:

              case type::pipe:
              case type::log_or:
              case type::log_and:
                break;
              default:
                fail (t) << "unexpected " << t << " after command exit status";
              }

              break;
            }

          case type::pipe:
          case type::log_or:
          case type::log_and:

          case type::in_pass:
          case type::out_pass:

          case type::in_null:
          case type::out_null:

          case type::out_trace:

          case type::out_merge:

          case type::in_str:
          case type::in_doc:
          case type::out_str:
          case type::out_doc:

          case type::in_file:
          case type::out_file_cmp:
          case type::out_file_ovr:
          case type::out_file_app:

          case type::clean:
            {
              if (pre_parse_)
              {
                // The only things we need to handle here are the here-document
                // and here-document regex end markers since we need to know
                // how many of them to pre-parse after the command.
                //
                switch (tt)
                {
                case type::in_doc:
                case type::out_doc:
                  mod = move (t.value);

                  bool re (mod.find ('~') != string::npos);
                  const char* what (re
                                    ? "here-document regex end marker"
                                    : "here-document end marker");

                  // We require the end marker to be a literal, unquoted word.
                  // In particularm, we don't allow quoted because of cases
                  // like foo"$bar" (where we will see word 'foo').
                  //
                  next (t, tt);

                  // We require the end marker to be an unquoted or completely
                  // quoted word. The complete quoting becomes important for
                  // cases like foo"$bar" (where we will see word 'foo').
                  //
                  // For good measure we could have also required it to be
                  // separated from the following token, but out grammar
                  // allows one to write >>EOO;. The problematic sequence
                  // would be >>FOO$bar -- on reparse it will be expanded
                  // as a single word.
                  //
                  if (tt != type::word || t.value.empty ())
                    fail (t) << "expected " << what;

                  peek ();
                  const token& p (peeked ());
                  if (!p.separated)
                  {
                    switch (p.type)
                    {
                    case type::dollar:
                    case type::lparen:
                      fail (p) << what << " must be literal";
                    }
                  }

                  quote_type qt (t.qtype);
                  switch (qt)
                  {
                  case quote_type::unquoted:
                    qt = quote_type::single; // Treat as single-quoted.
                    break;
                  case quote_type::single:
                  case quote_type::double_:
                    if (t.qcomp)
                      break;
                    // Fall through.
                  case quote_type::mixed:
                    fail (t) << "partially-quoted " << what;
                  }

                  regex_parts r;
                  string end (move (t.value));

                  if (re)
                  {
                    check_regex_mod (mod, end, l, what);

                    r = parse_regex (end, l, what);
                    end = move (r.value); // The "cleared" end marker.
                  }

                  bool literal (qt == quote_type::single);
                  bool shared (false);

                  for (const auto& d: hd)
                  {
                    if (d.end == end)
                    {
                      auto check = [&t, &end, &re, this] (bool c,
                                                          const char* what)
                      {
                        if (!c)
                          fail (t) << "different " << what
                                   << " for shared here-document "
                                   << (re ? "regex '" : "'") << end << "'";
                      };

                      check (d.modifiers == mod, "modifiers");
                      check (d.literal == literal, "quoting");

                      if (re)
                      {
                        check (d.regex == r.intro, "introducers");
                        check (d.regex_flags == r.flags, "global flags");
                      }

                      shared = true;
                      break;
                    }
                  }

                  if (!shared)
                    hd.push_back (
                      here_doc {
                        {},
                        move (end),
                        literal,
                        move (mod),
                        r.intro, move (r.flags)});

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
              case type::pipe:
              case type::log_or:
              case type::log_and:
                {
                  // Check that the previous command makes sense.
                  //
                  check_command (l, tt != type::pipe);
                  expr.back ().pipe.push_back (move (c));

                  c = command ();
                  p = pending::program;

                  if (tt != type::pipe)
                  {
                    expr_operator o (tt == type::log_or
                                     ? expr_operator::log_or
                                     : expr_operator::log_and);
                    expr.push_back ({o, command_pipe ()});
                  }

                  break;
                }

              case type::in_pass:
              case type::out_pass:

              case type::in_null:
              case type::out_null:

              case type::out_trace:

              case type::out_merge:

              case type::in_str:
              case type::in_doc:
              case type::out_str:
              case type::out_doc:

              case type::in_file:
              case type::out_file_cmp:
              case type::out_file_ovr:
              case type::out_file_app:
                {
                  parse_redirect (t, l);
                  break;
                }

              case type::clean:
                {
                  parse_clean (t);
                  break;
                }

              default: assert (false); break;
              }

              next (t, tt);
              break;
            }
          default:
            {
              // Here-document end markers are literal (we verified that above
              // during pre-parsing) and we need to know whether they were
              // quoted. So handle this case specially.
              //
              {
                int fd;
                switch (p)
                {
                case pending::in_document:   fd =  0; break;
                case pending::out_document:
                case pending::out_doc_regex: fd =  1; break;
                case pending::err_document:
                case pending::err_doc_regex: fd =  2; break;
                default:                     fd = -1; break;
                }

                if (fd != -1)
                {
                  here_redirect rd {
                    expr.size () - 1, expr.back ().pipe.size (), fd};

                  string end (move (t.value));

                  regex_parts r;

                  if (p == pending::out_doc_regex ||
                      p == pending::err_doc_regex)
                  {
                    // We can't fail here as we already parsed all the end
                    // markers during pre-parsing stage, and so no need in the
                    // description.
                    //
                    r = parse_regex (end, l, "");
                    end = move (r.value); // The "cleared" end marker.
                  }

                  bool shared (false);
                  for (auto& d: hd)
                  {
                    // No need to check that redirects that share here-document
                    // have the same modifiers, etc. That have been done during
                    // pre-parsing.
                    //
                    if (d.end == end)
                    {
                      d.redirects.emplace_back (rd);
                      shared = true;
                      break;
                    }
                  }

                  if (!shared)
                    hd.push_back (
                      here_doc {
                        {rd},
                        move (end),
                        (t.qtype == quote_type::unquoted ||
                         t.qtype == quote_type::single),
                        move (mod),
                        r.intro, move (r.flags)});

                  p = pending::none;
                  mod.clear ();

                  next (t, tt);
                  break;
                }
              }

              // Parse the next chunk as simple names to get expansion, etc.
              // Note that we do it in the chunking mode to detect whether
              // anything in each chunk is quoted.
              //
              // @@ PAT: should we support pattern expansion? This is even
              // fuzzier than the variable case above. Though this is the
              // shell semantics. Think what happens when we do rm *.txt?
              //
              reset_quoted (t);
              parse_names (t, tt,
                           ns,
                           pattern_mode::ignore,
                           true,
                           "command line",
                           nullptr);

              if (pre_parse_) // Nothing else to do if we are pre-parsing.
                break;

              // Process what we got. Determine whether anything inside was
              // quoted (note that the current token is "next" and is not part
              // of this).
              //
              bool q ((quoted () -
                       (t.qtype != quote_type::unquoted ? 1 : 0)) != 0);

              for (name& n: ns)
              {
                string s;

                try
                {
                  s = value_traits<string>::convert (move (n), nullptr);
                }
                catch (const invalid_argument&)
                {
                  diag_record dr (fail (l));
                  dr << "invalid string value ";
                  to_stream (dr.os, n, true); // Quote.
                }

                // If it is a quoted chunk, then we add the word as is.
                // Otherwise we re-lex it. But if the word doesn't contain any
                // interesting characters (operators plus quotes/escapes),
                // then no need to re-lex.
                //
                // NOTE: update quoting (script.cxx:to_stream_q()) if adding
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

                  // When re-lexing we do "effective escaping" and only for
                  // ['"\] (quotes plus the backslash itself). In particular,
                  // there is no way to escape redirects, operators, etc. The
                  // idea is to prefer quoting except for passing literal
                  // quotes, for example:
                  //
                  // args = \"&foo\"
                  // cmd $args               # cmd &foo
                  //
                  // args = 'x=\"foo bar\"'
                  // cmd $args               # cmd x="foo bar"
                  //
                  istringstream is (s);
                  lexer lex (is, name,
                             lexer_mode::command_expansion,
                             "\'\"\\");

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
                    case type::pipe:
                    case type::log_or:
                    case type::log_and:
                      {
                        // Check that the previous command makes sense.
                        //
                        check_command (l, tt != type::pipe);
                        expr.back ().pipe.push_back (move (c));

                        c = command ();
                        p = pending::program;

                        if (tt != type::pipe)
                        {
                          expr_operator o (tt == type::log_or
                                           ? expr_operator::log_or
                                           : expr_operator::log_and);
                          expr.push_back ({o, command_pipe ()});
                        }

                        break;
                      }

                    case type::in_pass:
                    case type::out_pass:

                    case type::in_null:
                    case type::out_null:

                    case type::out_trace:

                    case type::out_merge:

                    case type::in_str:
                    case type::out_str:

                    case type::in_file:
                    case type::out_file_cmp:
                    case type::out_file_ovr:
                    case type::out_file_app:
                      {
                        parse_redirect (t, l);
                        break;
                      }

                    case type::clean:
                      {
                        parse_clean (t);
                        break;
                      }

                    case type::in_doc:
                    case type::out_doc:
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
          // Verify we don't have anything pending to be filled and the
          // command makes sense.
          //
          check_pending (l);
          check_command (l, true);

          expr.back ().pipe.push_back (move (c));
        }

        return make_pair (move (expr), move (hd));
      }

      command_exit parser::
      parse_command_exit (token& t, type& tt)
      {
        // enter: equal/not_equal
        // leave: token after exit status (one parse_names() chunk)

        exit_comparison comp (tt == type::equal
                              ? exit_comparison::eq
                              : exit_comparison::ne);

        // The next chunk should be the exit status.
        //
        next (t, tt);
        location l (get_location (t));
        names ns (parse_names (t, tt,
                               pattern_mode::ignore,
                               true,
                               "exit status",
                               nullptr));
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
          {
            diag_record dr;

            dr << fail (l) << "expected exit status instead of ";
            to_stream (dr.os, ns, true); // Quote.

            dr << info << "exit status is an unsigned integer less than 256";
          }
        }

        return command_exit {comp, static_cast<uint8_t> (es)};
      }

      void parser::
      parse_here_documents (token& t, type& tt,
                            pair<command_expr, here_docs>& p)
      {
        // enter: newline
        // leave: newline

        // Parse here-document fragments in the order they were mentioned on
        // the command line.
        //
        for (here_doc& h: p.second)
        {
          // Switch to the here-line mode which is like single/double-quoted
          // string but recognized the newline as a separator.
          //
          mode (h.literal
                ? lexer_mode::here_line_single
                : lexer_mode::here_line_double);
          next (t, tt);

          parsed_doc v (
            parse_here_document (t, tt, h.end, h.modifiers, h.regex));

          if (!pre_parse_)
          {
            assert (!h.redirects.empty ());
            auto i (h.redirects.cbegin ());

            command& c (p.first[i->expr].pipe[i->pipe]);
            redirect& r (i->fd == 0 ? c.in : i->fd == 1 ? c.out : c.err);

            if (v.re)
            {
              r.regex = move (v.regex);
              r.regex.flags = move (h.regex_flags);
            }
            else
              r.str = move (v.str);

            r.end        = move (h.end);
            r.end_line   = v.end_line;
            r.end_column = v.end_column;

            // Note that our references cannot be invalidated because the
            // command_expr/command-pipe vectors already contain all their
            // elements.
            //
            for (++i; i != h.redirects.cend (); ++i)
            {
              command& c (p.first[i->expr].pipe[i->pipe]);

              (i->fd == 0 ? c.in : i->fd == 1 ? c.out : c.err) =
                redirect (redirect_type::here_doc_ref, r);
            }
          }

          expire_mode ();
        }
      }

      parser::parsed_doc parser::
      parse_here_document (token& t, type& tt,
                           const string& em,
                           const string& mod,
                           char re)
      {
        // enter: first token on first line
        // leave: newline (after end marker)

        // String literal. Note that when decide if to terminate the previously
        // added line with a newline, we need to distinguish a yet empty result
        // and the one that has a single blank line added.
        //
        optional<string> rs;

        regex_lines rre;

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

        // We will use the location of the first token on the line for the
        // regex diagnostics. At the end of the loop it will point to the
        // beginning of the end marker.
        //
        location l;

        while (tt != type::eos)
        {
          l = get_location (t);

          // Check if this is the end marker. For starters, it should be a
          // single, unquoted word followed by a newline.
          //
          if (tt == type::word &&
              t.qtype == quote_type::unquoted &&
              peek () == type::newline)
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
          // @@ PAT: one could argue that if we do it in variables, then we
          // should do it here as well. Though feels bizarre.
          //
          names ns (tt != type::newline
                    ? parse_names (t, tt,
                                   pattern_mode::ignore,
                                   false,
                                   "here-document line",
                                   nullptr)
                    : names ());

          if (!pre_parse_)
          {
            // What shall we do if the expansion results in multiple names?
            // For, example if the line contains just the variable expansion
            // and it is of type strings. Adding all the elements space-
            // separated seems like the natural thing to do.
            //
            string s;
            for (auto b (ns.begin ()), i (b); i != ns.end (); ++i)
            {
              string n;

              try
              {
                n = value_traits<string>::convert (move (*i), nullptr);
              }
              catch (const invalid_argument&)
              {
                fail (l) << "invalid string value '" << *i << "'";
              }

              if (i == b)
                s = move (n);
              else
              {
                s += ' ';
                s += n;
              }
            }

            if (!re)
            {
              // Add newline after previous line.
              //
              if (rs)
              {
                *rs += '\n';
                *rs += s;
              }
              else
                rs = move (s);
            }
            else
            {
              // Due to expansion we can end up with multiple lines. If empty
              // then will add a blank textual literal.
              //
              for (size_t p (0); p != string::npos; )
              {
                string ln;
                size_t np (s.find ('\n', p));

                if (np != string::npos)
                {
                  ln = string (s, p, np - p);
                  p = np + 1;
                }
                else
                {
                  ln = string (s, p);
                  p = np;
                }

                if (ln[0] != re) // Line doesn't start with regex introducer.
                {
                  // This is a line-char literal (covers blank lines as well).
                  //
                  // Append textual literal.
                  //
                  rre.lines.emplace_back (l.line, l.column, move (ln), false);
                }
                else // Line starts with the regex introducer.
                {
                  // This is a char-regex, or a sequence of line-regex syntax
                  // characters or both (in this specific order). So we will
                  // add regex (with optional special characters) or special
                  // literal.
                  //
                  size_t p (ln.find (re, 1));
                  if (p == string::npos)
                  {
                    // No regex, just a sequence of syntax characters.
                    //
                    string spec (ln, 1);
                    if (spec.empty ())
                      fail (l) << "no syntax line characters";

                    // Append special literal.
                    //
                    rre.lines.emplace_back (
                      l.line, l.column, move (spec), true);
                  }
                  else
                  {
                    // Regex (probably with syntax characters).
                    //
                    regex_parts re;

                    // Empty regex is a special case repesenting a blank line.
                    //
                    if (p == 1)
                      // Position to optional specal characters of an empty
                      // regex.
                      //
                      ++p;
                    else
                      // Can't fail as all the pre-conditions verified
                      // (non-empty with both introducers in place), so no
                      // description required.
                      //
                      re = parse_regex (ln, l, "", &p);

                    // Append regex with optional special characters.
                    //
                    rre.lines.emplace_back (l.line, l.column,
                                            move (re.value), move (re.flags),
                                            string (ln, p));
                  }
                }
              }
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
          // Add final newline unless suppressed.
          //
          if (mod.find (':') == string::npos)
          {
            if (re)
              // Note that the position is synthetic, but that's ok as we don't
              // expect any diagnostics to refer this line.
              //
              rre.lines.emplace_back (l.line, l.column, string (), false);
            else if (rs)
              *rs += '\n';
            else
              rs = "\n";
          }

          // Finalize regex lines.
          //
          if (re)
          {
            // Empty regex matches nothing, so not of much use.
            //
            if (rre.lines.empty ())
              fail (l) << "empty here-document regex";

            rre.intro  = re;
          }
        }

        return re
          ? parsed_doc (move (rre), l.line, l.column)
          : parsed_doc (rs ? move (*rs) : string (), l.line, l.column);
      }

      //
      // Execute.
      //

      void parser::
      execute (script& s, runner& r)
      {
        assert (s.state == scope_state::unknown);

        auto g (
          make_exception_guard (
            [&s] () {s.state = scope_state::failed;}));

        if (!s.empty ())
          execute (s, s, r);
        else
          s.state = scope_state::passed;
      }

      void parser::
      execute (scope& sc, script& s, runner& r)
      {
        path_ = nullptr; // Set by replays.

        pre_parse_ = false;

        set_lexer (nullptr);

        script_ = &s;
        runner_ = &r;
        group_ = nullptr;
        id_map_ = nullptr;
        include_set_ = nullptr;
        scope_ = &sc;

        //@@ PAT TODO: set pbase_?

        exec_scope_body ();
      }

      static void
      execute_impl (scope& s, script& scr, runner& r)
      {
        try
        {
          parser p;
          p.execute (s, scr, r);
        }
        catch (const failed&)
        {
          s.state = scope_state::failed;
        }
      }

      void parser::
      exec_scope_body ()
      {
        size_t li (0);

        runner_->enter (*scope_, scope_->start_loc_);

        if (test* t = dynamic_cast<test*> (scope_))
        {
          exec_lines (
            t->tests_.begin (), t->tests_.end (), li, command_type::test);
        }
        else if (group* g = dynamic_cast<group*> (scope_))
        {
          bool exec_scope (
            exec_lines (
              g->setup_.begin (), g->setup_.end (), li, command_type::setup));

          if (exec_scope)
          {
            atomic_count task_count (0);
            wait_guard wg (task_count);

            // Start asynchronous execution of inner scopes keeping track of
            // how many we have handled.
            //
            for (unique_ptr<scope>& chain: g->scopes)
            {
              // Check if this scope is ignored (e.g., via config.test).
              //
              if (!runner_->test (*chain) || !exec_scope)
              {
                chain = nullptr;
                continue;
              }

              // Pick a scope from the if-else chain.
              //
              // In fact, we are going to drop all but the selected (if any)
              // scope. This way we can re-examine the scope states later. It
              // will also free some memory.
              //
              unique_ptr<scope>* ps;
              for (ps = &chain; *ps != nullptr; ps = &ps->get ()->if_chain)
              {
                scope& s (**ps);

                if (!s.if_cond_) // Unconditional.
                {
                  assert (s.if_chain == nullptr);
                  break;
                }

                line l (move (*s.if_cond_));
                line_type lt (l.type);

                replay_data (move (l.tokens));

                token t;
                type tt;

                next (t, tt);
                const location ll (get_location (t));
                next (t, tt); // Skip to start of command.

                bool take;
                if (lt != line_type::cmd_else)
                {
                  // Note: the line index count continues from setup.
                  //
                  command_expr ce (parse_command_line (t, tt));

                  try
                  {
                    take = runner_->run_if (*scope_, ce, ++li, ll);
                  }
                  catch (const exit_scope& e)
                  {
                    // Bail out if the scope is exited with the failure status.
                    // Otherwise leave the scope normally.
                    //
                    if (!e.status)
                      throw failed ();

                    // Stop iterating through if conditions, and stop executing
                    // inner scopes.
                    //
                    exec_scope = false;
                    replay_stop ();
                    break;
                  }

                  if (lt == line_type::cmd_ifn || lt == line_type::cmd_elifn)
                    take = !take;
                }
                else
                {
                  assert (tt == type::newline);
                  take = true;
                }

                replay_stop ();

                if (take)
                {
                  // Count the remaining conditions for the line index.
                  //
                  for (scope* r (s.if_chain.get ());
                       r != nullptr &&
                         r->if_cond_->type != line_type::cmd_else;
                       r = r->if_chain.get ())
                    ++li;

                  s.if_chain.reset (); // Drop remaining scopes.
                  break;
                }
              }

              chain.reset (*ps == nullptr || (*ps)->empty () || !exec_scope
                           ? nullptr
                           : ps->release ());

              if (chain != nullptr)
              {
                // Hand it off to a sub-parser potentially in another thread.
                // But we could also have handled it serially in this parser:
                //
                // scope* os (scope_);
                // scope_ = chain.get ();
                // exec_scope_body ();
                // scope_ = os;

                // Pass our diagnostics stack (this is safe since we are going
                // to wait for completion before unwinding the diag stack).
                //
                // If the scope was executed synchronously, check the status
                // and bail out if we weren't asked to keep going.
                //
                if (!sched.async (task_count,
                                  [] (scope& s,
                                      script& scr,
                                      runner& r,
                                      const diag_frame* ds)
                                  {
                                    diag_frame df (ds);
                                    execute_impl (s, scr, r);
                                  },
                                  ref (*chain),
                                  ref (*script_),
                                  ref (*runner_),
                                  diag_frame::stack))
                {
                  // Bail out if the scope has failed and we weren't instructed
                  // to keep going.
                  //
                  if (chain->state == scope_state::failed && !keep_going)
                    throw failed ();
                }
              }
            }

            wg.wait ();

            // Re-examine the scopes we have executed collecting their state.
            //
            for (const unique_ptr<scope>& chain: g->scopes)
            {
              if (chain == nullptr)
                continue;

              switch (chain->state)
              {
              case scope_state::passed: break;
              case scope_state::failed: throw failed ();
              default:                  assert (false);
              }
            }
          }

          exec_lines (
            g->tdown_.begin (), g->tdown_.end (), li, command_type::teardown);
        }
        else
          assert (false);

        runner_->leave (*scope_, scope_->end_loc_);

        scope_->state = scope_state::passed;
      }

      bool parser::
      exec_lines (lines::iterator i, lines::iterator e,
                  size_t& li,
                  command_type ct)
      {
        try
        {
          token t;
          type tt;

          for (; i != e; ++i)
          {
            line& ln (*i);
            line_type lt (ln.type);

            assert (path_ == nullptr);

            // Set the tokens and start playing.
            //
            replay_data (move (ln.tokens));

            // We don't really need to change the mode since we already know
            // the line type.
            //
            next (t, tt);
            const location ll (get_location (t));

            switch (lt)
            {
            case line_type::var:
              {
                // Parse.
                //
                string name (move (t.value));

                next (t, tt);
                type kind (tt); // Assignment kind.

                value rhs (parse_variable_line (t, tt));

                if (tt == type::semi)
                  next (t, tt);

                assert (tt == type::newline);

                // Assign.
                //
                const variable& var (*ln.var);

                value& lhs (kind == type::assign
                            ? scope_->assign (var)
                            : scope_->append (var));

                build2::parser::apply_value_attributes (
                  &var, lhs, move (rhs), kind);

                // If we changes any of the test.* values, then reset the $*,
                // $N special aliases.
                //
                if (var.name == script_->test_var.name      ||
                    var.name == script_->options_var.name   ||
                    var.name == script_->arguments_var.name ||
                    var.name == script_->redirects_var.name ||
                    var.name == script_->cleanups_var.name)
                {
                  scope_->reset_special ();
                }

                replay_stop ();
                break;
              }
            case line_type::cmd:
              {
                // We use the 0 index to signal that this is the only command.
                // Note that we only do this for test commands.
                //
                if (ct == command_type::test && li == 0)
                {
                  lines::iterator j (i);
                  for (++j; j != e && j->type == line_type::var; ++j) ;

                  if (j != e) // We have another command.
                    ++li;
                }
                else
                  ++li;

                command_expr ce (parse_command_line (t, tt));
                runner_->run (*scope_, ce, ct, li, ll);

                replay_stop ();
                break;
              }
            case line_type::cmd_if:
            case line_type::cmd_ifn:
            case line_type::cmd_elif:
            case line_type::cmd_elifn:
            case line_type::cmd_else:
              {
                next (t, tt); // Skip to start of command.

                bool take;
                if (lt != line_type::cmd_else)
                {
                  // Assume if-else always involves multiple commands.
                  //
                  command_expr ce (parse_command_line (t, tt));
                  take = runner_->run_if (*scope_, ce, ++li, ll);

                  if (lt == line_type::cmd_ifn || lt == line_type::cmd_elifn)
                    take = !take;
                }
                else
                {
                  assert (tt == type::newline);
                  take = true;
                }

                replay_stop ();

                // If end is true, then find the 'end' line. Otherwise, find
                // the next if-else line. If skip is true then increment the
                // command line index.
                //
                auto next = [e, &li]
                  (lines::iterator j, bool end, bool skip) -> lines::iterator
                  {
                    // We need to be aware of nested if-else chains.
                    //
                    size_t n (0);

                    for (++j; j != e; ++j)
                    {
                      line_type lt (j->type);

                      if (lt == line_type::cmd_if ||
                          lt == line_type::cmd_ifn)
                        ++n;

                      // If we are nested then we just wait until we get back
                      // to the surface.
                      //
                      if (n == 0)
                      {
                        switch (lt)
                        {
                        case line_type::cmd_elif:
                        case line_type::cmd_elifn:
                        case line_type::cmd_else:
                          {
                            if (end) break;

                            // Fall through.
                          }
                        case line_type::cmd_end:  return j;
                        default: break;
                        }
                      }

                      if (lt == line_type::cmd_end)
                        --n;

                      if (skip)
                      {
                        // Note that we don't count else and end as commands.
                        //
                        switch (lt)
                        {
                        case line_type::cmd:
                        case line_type::cmd_if:
                        case line_type::cmd_ifn:
                        case line_type::cmd_elif:
                        case line_type::cmd_elifn: ++li; break;
                        default:                         break;
                        }
                      }
                    }

                    assert (false); // Missing end.
                    return e;
                  };

                // If we are taking this branch then we need to parse all the
                // lines until the next if-else line and then skip all the
                // lines until the end (unless next is already end).
                //
                // Otherwise, we need to skip all the lines until the next
                // if-else line and then continue parsing.
                //
                if (take)
                {
                  lines::iterator j (next (i, false, false)); // Next if-else.
                  if (!exec_lines (i + 1, j, li, ct))
                    return false;

                  i = j->type == line_type::cmd_end ? j : next (j, true, true);
                }
                else
                {
                  i = next (i, false, true);
                  if (i->type != line_type::cmd_end)
                    --i; // Continue with this line (e.g., elif or else).
                }

                break;
              }
            case line_type::cmd_end:
              {
                assert (false);
              }
            }
          }

          return true;
        }
        catch (const exit_scope& e)
        {
          // Bail out if the scope is exited with the failure status. Otherwise
          // leave the scope normally.
          //
          if (!e.status)
            throw failed ();

          replay_stop ();
          return false;
        }
      }

      //
      // The rest.
      //

      lookup parser::
      lookup_variable (name&& qual, string&& name, const location& loc)
      {
        assert (!pre_parse_);

        if (!qual.empty ())
          fail (loc) << "qualified variable name";

        // If we have no scope (happens when pre-parsing directives), then we
        // only look for buildfile variables.
        //
        // Otherwise, every variable that is ever set in a script has been
        // pre-entered during pre-parse or introduced with the set builtin
        // during test execution. Which means that if one is not found in the
        // script pool then it can only possibly be set in the buildfile.
        //
        // Note that we need to acquire the variable pool lock. The pool can
        // be changed from multiple threads by the set builtin. The obtained
        // variable pointer can safelly be used with no locking as the variable
        // pool is an associative container (underneath) and we are only adding
        // new variables into it.
        //
        const variable* pvar (nullptr);

        if (scope_ != nullptr)
        {
          slock sl (script_->var_pool_mutex);
          pvar = script_->var_pool.find (name);
        }

        return pvar != nullptr
          ? scope_->find (*pvar)
          : script_->find_in_buildfile (name);
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
            if (replay_data_[i].token.qtype != quote_type::unquoted)
              ++r;
        }

        return r;
      }

      void parser::
      reset_quoted (token& cur)
      {
        if (replay_ != replay::play)
          lexer_->reset_quoted (cur.qtype != quote_type::unquoted ? 1 : 0);
        else
        {
          replay_quoted_ = replay_i_ - 1;

          // Must be the same token.
          //
          assert (replay_data_[replay_quoted_].token.qtype == cur.qtype);
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

      void parser::
      set_lexer (lexer* l)
      {
        lexer_ = l;
        base_parser::lexer_ = l;
      }

      void parser::
      apply_value_attributes (const variable* var,
                              value& lhs,
                              value&& rhs,
                              const string& attributes,
                              token_type kind,
                              const path& name)
      {
        path_ = &name;

        istringstream is (attributes);
        lexer l (is, name, lexer_mode::attribute);
        set_lexer (&l);

        token t;
        type tt;
        next (t, tt);

        if (tt != type::lsbrace && tt != type::eos)
          fail (t) << "expected '[' instead of " << t;

        attributes_push (t, tt, true);

        if (tt != type::eos)
          fail (t) << "trailing junk after ']'";

        build2::parser::apply_value_attributes (var, lhs, move (rhs), kind);
      }

      // parser::parsed_doc
      //
      parser::parsed_doc::
      parsed_doc (string s, uint64_t l, uint64_t c)
          : str (move (s)), re (false), end_line (l), end_column (c)
      {
      }

      parser::parsed_doc::
      parsed_doc (regex_lines&& r, uint64_t l, uint64_t c)
          : regex (move (r)), re (true), end_line (l), end_column (c)
      {
      }

      parser::parsed_doc::
      parsed_doc (parsed_doc&& d)
          : re (d.re), end_line (d.end_line), end_column (d.end_column)
      {
        if (re)
          new (&regex) regex_lines (move (d.regex));
        else
          new (&str) string (move (d.str));
      }

      parser::parsed_doc::
      ~parsed_doc ()
      {
        if (re)
          regex.~regex_lines ();
        else
          str.~string ();
      }
    }
  }
}
