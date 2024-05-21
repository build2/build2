// file      : libbuild2/test/script/parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/script/parser.hxx>

#include <libbuild2/context.hxx> // sched, keep_going

#include <libbuild2/test/script/lexer.hxx>
#include <libbuild2/test/script/runner.hxx>

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
        path_ = &*s.paths_.insert (
          path_name_value (s.script_target.path ())).first;

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
        group_->start_loc_ = location (*path_, 1, 1);

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
      // recognize it is to parse the if line), handling the flow control
      // constructs recursively.
      //
      // If one is true then only parse one line returning an indication of
      // whether the line ended with a semicolon. If the flow control
      // construct type is specified, then this line is assumed to belong to
      // such construct.
      //
      bool parser::
      pre_parse_line (token& t, type& tt,
                      optional<description>& d,
                      lines* ls,
                      bool one,
                      optional<line_type> fct)
      {
        // enter: next token is peeked at (type in tt)
        // leave: newline

        assert (!fct                              ||
                *fct == line_type::cmd_if         ||
                *fct == line_type::cmd_while      ||
                *fct == line_type::cmd_for_stream ||
                *fct == line_type::cmd_for_args);

        // Note: token is only peeked at.
        //
        const location ll (get_location (peeked ()));

        // Determine the line type/start token.
        //
        line_type lt;
        type st (type::eos); // Later, can only be set to plus or minus.
        bool semi (false);

        // Parse the command line tail, starting from the newline or the
        // potential colon/semicolon token.
        //
        // Note that colon and semicolon are only valid in test command lines
        // and after 'end' in flow control constructs. Note that we always
        // recognize them lexically, even when they are not valid tokens per
        // the grammar.
        //
        auto parse_command_tail = [&t, &tt, &st, &lt, &d, &semi, &ll, this] ()
        {
          if (tt != type::newline)
          {
            if (lt != line_type::cmd && lt != line_type::cmd_end)
              fail (t) << "expected newline instead of " << t;

            switch (st)
            {
            case type::plus:  fail (t) << t << " after setup command" << endf;
            case type::minus: fail (t) << t << " after teardown command" << endf;
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
              replay_pop (); // See above for the reasoning.
              next (t, tt);  // Get newline.
              break;
            }
          }

          if (tt != type::newline)
            fail (t) << "expected newline instead of " << t;
        };

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

              // Handle the for-loop consistently with pre_parse_line_start().
              //
              if      (n == "if")    lt = line_type::cmd_if;
              else if (n == "if!")   lt = line_type::cmd_ifn;
              else if (n == "while") lt = line_type::cmd_while;
              else if (n == "for")   lt = line_type::cmd_for_stream;
            }

            break;
          }
        default:
          {
            lt = pre_parse_line_start (t, tt, lexer_mode::second_token);
            break;
          }
        }

        // Pre-parse the line keeping track of whether it ends with a semi.
        //
        line ln;
        switch (lt)
        {
        case line_type::var:
          {
            // Check if we are trying to modify any of the special aliases
            // ($*, $N, $~, $@).
            //
            string& n (t.value);

            if (special_variable (n))
              fail (t) << "attempt to set '" << n << "' variable directly";

            // Pre-enter the variables now while we are executing serially.
            // Once parallel, it becomes a lot harder to do.
            //
            ln.var = &script_->var_pool.insert (move (n));

            next (t, tt); // Assignment kind.

            // We cannot reuse the value mode since it will recognize `{`
            // which we want to treat as a literal.
            //
            mode (lexer_mode::variable_line);
            parse_variable_line (t, tt);

            // Note that the semicolon token is only required during
            // pre-parsing to decide which line list the current line should
            // go to and provides no additional semantics during the
            // execution. Moreover, build2::script::parser::exec_lines()
            // doesn't expect this token to be present. Thus, we just drop
            // this token from the saved tokens.
            //
            semi = (tt == type::semi);

            if (semi)
            {
              replay_pop ();
              next (t, tt);
            }

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

            string& n (t.value);

            if (tt == type::word && t.qtype == quote_type::unquoted &&
                (n[0] == '_' || alpha (n[0]) ||     // Variable.
                 n == "*" || n == "~" || n == "@")) // Special variable.
            {
              // Detect patterns analogous to parse_variable_name() (so we
              // diagnose `for x[string]: ...`).
              //
              if (n.find_first_of ("[*?") != string::npos)
                fail (t) << "expected variable name instead of " << n;

              if (special_variable (n))
                fail (t) << "attempt to set '" << n << "' variable directly";

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
              ln.var = nullptr;

              expire_mode ();

              parse_command_expr_result r (
                parse_command_expr (t, tt,
                                    lexer::redirect_aliases,
                                    move (pt)));

              assert (r.for_loop);

              parse_command_tail ();
              parse_here_documents (t, tt, r);
            }
            else                                 // for x: ...
            {
              ln.var = &script_->var_pool.insert (move (n));

              next (t, tt);

              assert (tt == type::colon);

              expire_mode ();

              // Parse the value similar to the var line type (see above),
              // except for the fact that we don't expect a trailing semicolon.
              //
              mode (lexer_mode::variable_line);
              parse_variable_line (t, tt);

              if (tt != type::newline)
                fail (t) << "expected newline instead of " << t << " after for";
            }

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

            parse_command_tail ();
            parse_here_documents (t, tt, r);

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

        switch (lt)
        {
        case line_type::cmd_if:
        case line_type::cmd_ifn:
          {
            semi = pre_parse_if_else (t, tt, d, *ls);

            // If this turned out to be scope-if, then ls is empty, semi is
            // false, and none of the below logic applies.
            //
            if (ls->empty ())
              return semi;

            break;
          }
        case line_type::cmd_while:
        case line_type::cmd_for_stream:
        case line_type::cmd_for_args:
          {
            semi = pre_parse_loop (t, tt, lt, d, *ls);
            break;
          }
        default: break;
        }

        // Unless we were told where to put it, decide where it actually goes.
        //
        if (ls == &ls_data)
        {
          // First pre-check variables and variable-only flow control
          // constructs: by themselves (i.e., without a trailing semicolon)
          // they are treated as either setup or teardown without
          // plus/minus. Also handle illegal line types.
          //
          switch (lt)
          {
          case line_type::cmd_elif:
          case line_type::cmd_elifn:
          case line_type::cmd_else:
          case line_type::cmd_end:
            {
              assert (false); // Should have been failed earlier.
            }
          case line_type::cmd_if:
          case line_type::cmd_ifn:
          case line_type::cmd_while:
          case line_type::cmd_for_stream:
          case line_type::cmd_for_args:
            {
              // See if this is a variable-only flow control construct.
              //
              if (find_if (ls_data.begin (), ls_data.end (),
                           [] (const line& l) {
                             return l.type == line_type::cmd;
                           }) != ls_data.end ())
                break;
            }
            // Fall through.
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
                              << "variable-only " << lt;
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
            fail (ll) << "description inside test" << endf;
          case type::eos:
          case type::rcbrace:
          case type::lcbrace:
            fail (ll) << "expected another line after ';'" << endf;
          case type::plus:
            fail (ll) << "setup command in test" << endf;
          case type::minus:
            fail (ll) << "teardown command in test" << endf;
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
            g->desc = (ps == &root ? d : root->desc);

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
          bool semi (pre_parse_line (t, (tt = pt),
                                     td,
                                     &ls,
                                     true /* one */,
                                     line_type::cmd_if));

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

      // Pre-parse the flow control construct block line. Fail if the line is
      // unexpectedly followed with a semicolon or test description.
      //
      bool parser::
      pre_parse_block_line (token& t, type& tt,
                            line_type bt,
                            optional<description>& d,
                            lines& ls)
      {
        // enter: peeked first token of the line (type in tt)
        // leave: newline

        const location ll (get_location (peeked ()));

        switch (tt)
        {
        case type::colon:
          fail (ll) << "description inside " << bt << endf;
        case type::eos:
        case type::rcbrace:
        case type::lcbrace:
          fail (ll) << "expected closing 'end'" << endf;
        case type::plus:
          fail (ll) << "setup command inside " << bt << endf;
        case type::minus:
          fail (ll) << "teardown command inside " << bt << endf;
        }

        // Parse one line. Note that this one line can still be multiple lines
        // in case of a flow control construct. In this case we want to view
        // it as, for example, cmd_if, not cmd_end. Thus remember the start
        // position of the next logical line.
        //
        size_t i (ls.size ());

        line_type fct; // Flow control construct type the block type relates to.

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

        optional<description> td;
        bool semi (pre_parse_line (t, tt, td, &ls, true /* one */, fct));

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

        return false;
      }

      bool parser::
      pre_parse_if_else_command (token& t, type& tt,
                                 optional<description>& d,
                                 lines& ls)
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
          size_t i (ls.size ());

          bool semi (pre_parse_block_line (t, tt, bt, d, ls));

          line_type lt (ls[i].type);

          // First take care of 'end'.
          //
          if (lt == line_type::cmd_end)
            return semi;

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

        assert (false); // Can't be here.
        return false;
      }

      bool parser::
      pre_parse_loop (token& t, type& tt,
                      line_type lt,
                      optional<description>& d,
                      lines& ls)
      {
        // enter: <newline> (previous line)
        // leave: <newline>

        assert (lt == line_type::cmd_while      ||
                lt == line_type::cmd_for_stream ||
                lt == line_type::cmd_for_args);

        tt = peek (lexer_mode::first_token);

        // Parse lines until we see closing 'end'.
        //
        for (;; tt = peek (lexer_mode::first_token))
        {
          size_t i (ls.size ());

          bool semi (pre_parse_block_line (t, tt, lt, d, ls));

          if (ls[i].type == line_type::cmd_end)
            return semi;
        }

        assert (false); // Can't be here.
        return false;
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
                              pattern_mode::ignore,
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
          auto enter_path = [this] (string n) -> const path_name_value&
          {
            path p (move (n));

            if (p.relative ())
            {
              // There is always the testscript path (path_ refers to an
              // object in the script::paths_ set).
              //
              assert (path_->path != nullptr);

              p = path_->path->directory () / p;
            }

            p.normalize ();

            return *script_->paths_.insert (path_name_value (move (p))).first;
          };

          const path_name_value& pn (enter_path (move (n)));
          const path& p (pn.path);

          if (include_set_->insert (p).second || !once)
          {
            try
            {
              ifdstream ifs (p);
              lexer l (ifs, pn, lexer_mode::command_line);

              const path_name* op (path_);
              path_ = &pn;

              build2::script::lexer* ol (lexer_);
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
          to_stream (dr.os, n, quote_mode::normal);
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
              (l.find_first_of (" \t.", i) >= j ? r.id : r.summary).
                assign (l, i, n);

              // If this is an id then validate it.
              //
              if (!r.id.empty ())
              {
                for (char c: r.id)
                {
                  if (!(alnum (c) || c == '_' || c == '-' || c == '+'))
                    fail (loc) << "illegal character '" << c
                               << "' in test id '" << r.id << "'";
                }
              }
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

      command_expr parser::
      parse_command_line (token& t, type& tt)
      {
        // enter: first token of the command line
        // leave: <newline>

        // Note: this one is only used during execution.

        parse_command_expr_result pr (
          parse_command_expr (t, tt, lexer::redirect_aliases));

        if (tt == type::colon)
          parse_trailing_description (t, tt);

        assert (tt == type::newline);

        parse_here_documents (t, tt, pr);
        assert (tt == type::newline);

        command_expr r (move (pr.expr));

        // If the test program runner is specified, then adjust the
        // expressions to run test programs via this runner.
        //
        pair<const process_path*, const strings*> tr (
          runner_->test_runner ());

        if (tr.first != nullptr)
        {
          for (expr_term& t: r)
          {
            for (command& c: t.pipe)
            {
              if (scope_->test_program (c.program.recall))
              {
                // Append the runner options and the test program path to the
                // the arguments list and rotate the list to the left, so that
                // it starts from the runner options. This should probably be
                // not less efficient than inserting the program path and then
                // the runner options at the beginning of the list.
                //
                strings& args (c.arguments);
                size_t n (args.size ());

                args.insert (args.end (),
                             tr.second->begin (), tr.second->end ());

                args.push_back (c.program.recall.string ());

                rotate (args.begin (), args.begin () + n, args.end ());

                c.program = process_path (*tr.first, false /* init */);
              }
            }
          }
        }

        return r;
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

        // The script shouldn't be able to modify the scope.
        //
        // Note that we need it for calling functions which require the
        // current scope, such as $target.path().
        //
        build2::parser::scope_ = const_cast<build2::scope*> (&s.target_scope);

        //@@ PAT TODO: set pbase_?

        exec_scope_body ();
      }

      static void
      execute_impl (scope& s, script& scr, runner& r)
      {
        try
        {
          parser p (scr.test_target.ctx);
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
        runner_->enter (*scope_, scope_->start_loc_);

        // Set thread-specific current directory override. In particular, this
        // makes sure functions like $path.complete() work correctly.
        //
        auto wdg = make_guard (
          [old = path_traits::thread_current_directory ()] ()
          {
            path_traits::thread_current_directory (old);
          });

        path_traits::thread_current_directory (&scope_->work_dir.path->string ());

        // Note that we rely on "small function object" optimization for the
        // exec_*() lambdas.
        //
        auto exec_set = [this] (const variable& var,
                                token& t, build2::script::token_type& tt,
                                const location&)
        {
          next (t, tt);
          type kind (tt); // Assignment kind.

          // We cannot reuse the value mode (see above for details).
          //
          mode (lexer_mode::variable_line);
          value rhs (parse_variable_line (t, tt));

          assert (tt == type::newline);

          // Assign.
          //
          value& lhs (kind == type::assign
                      ? scope_->assign (var)
                      : scope_->append (var));

          apply_value_attributes (&var, lhs, move (rhs), kind);

          if (script_->test_command_var (var.name))
            scope_->reset_special ();
        };

        // Is set later, right before the exec_lines() call.
        //
        command_type ct;

        auto exec_cmd = [&ct, this] (token& t, build2::script::token_type& tt,
                                     const iteration_index* ii, size_t li,
                                     bool single,
                                     const function<command_function>& cf,
                                     const location& ll)
        {
          // We use the 0 index to signal that this is the only command.
          // Note that we only do this for test commands.
          //
          if (ct == command_type::test && single)
            li = 0;

          command_expr ce (
            parse_command_line (t, static_cast<token_type&> (tt)));

          runner_->run (*scope_, ce, ct, ii, li, cf, ll);
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
          return runner_->run_cond (*scope_, ce, ii, li, ll);
        };

        auto exec_for = [this] (const variable& var,
                                value&& val,
                                const attributes& val_attrs,
                                const location&)
        {
          value& lhs (scope_->assign (var));

          attributes_.push_back (val_attrs);

          apply_value_attributes (&var, lhs, move (val), type::assign);

          if (script_->test_command_var (var.name))
            scope_->reset_special ();
        };

        size_t li (1);

        if (test* t = dynamic_cast<test*> (scope_))
        {
          ct = command_type::test;

          exec_lines (t->tests_.begin (), t->tests_.end (),
                      exec_set, exec_cmd, exec_cond, exec_for,
                      nullptr /* iteration_index */, li);
        }
        else if (group* g = dynamic_cast<group*> (scope_))
        {
          ct = command_type::setup;

          bool exec_scope (
            exec_lines (g->setup_.begin (), g->setup_.end (),
                        exec_set, exec_cmd, exec_cond, exec_for,
                        nullptr /* iteration_index */, li));

          if (exec_scope)
          {
            atomic_count task_count (0);
            wait_guard wg (g->root.test_target.ctx, task_count);

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
                    take = runner_->run_cond (
                      *scope_, ce, nullptr /* iteration_index */, li++, ll);
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
                // UBSan workaround.
                //
                const diag_frame* df (diag_frame::stack ());
                if (!ctx->sched->async (task_count,
                                        [] (const diag_frame* ds,
                                            scope& s,
                                            script& scr,
                                            runner& r)
                                        {
                                          diag_frame::stack_guard dsg (ds);
                                          execute_impl (s, scr, r);
                                        },
                                        df,
                                        ref (*chain),
                                        ref (*script_),
                                        ref (*runner_)))
                {
                  // Bail out if the scope has failed and we weren't instructed
                  // to keep going.
                  //
                  if (chain->state == scope_state::failed && !ctx->keep_going)
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

          ct = command_type::teardown;

          exec_lines (g->tdown_.begin (), g->tdown_.end (),
                      exec_set, exec_cmd, exec_cond, exec_for,
                      nullptr /* iteration_index */, li);
        }
        else
          assert (false);

        runner_->leave (*scope_, scope_->end_loc_);

        scope_->state = scope_state::passed;
      }

      //
      // The rest.
      //

      // When add a special variable don't forget to update lexer::word() and
      // for-loop parsing in pre_parse_line().
      //
      bool parser::
      special_variable (const string& n) noexcept
      {
        return n == "*" || n == "~" || n == "@" || digit (n);
      }

      lookup parser::
      lookup_variable (names&& qual, string&& name, const location& loc)
      {
        if (pre_parse_)
          return lookup ();

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
          ? scope_->lookup (*pvar)
          : script_->lookup_in_buildfile (name);
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
