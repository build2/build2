// file      : libbuild2/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/parser.hxx>

#include <sstream>
#include <iostream> // cout

#include <libbutl/filesystem.mxx>   // path_search
#include <libbutl/path-pattern.mxx>

#include <libbuild2/dump.hxx>
#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/module.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/prerequisite.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  using type = token_type;

  class parser::enter_scope
  {
  public:
    enter_scope (): p_ (nullptr), r_ (nullptr), s_ (nullptr), b_ (nullptr) {}

    enter_scope (parser& p, dir_path&& d)
        : p_ (&p), r_ (p.root_), s_ (p.scope_), b_ (p.pbase_)
    {
      // Try hard not to call normalize(). Most of the time we will go just
      // one level deeper.
      //
      bool n (true);

      if (d.relative ())
      {
        // Relative scopes are opened relative to out, not src.
        //
        if (d.simple () && !d.current () && !d.parent ())
        {
          d = dir_path (p.scope_->out_path ()) /= d.string ();
          n = false;
        }
        else
          d = p.scope_->out_path () / d;
      }

      if (n)
        d.normalize ();

      p.switch_scope (d);
    }

    ~enter_scope ()
    {
      if (p_ != nullptr)
      {
        p_->scope_ = s_;
        p_->root_ = r_;
        p_->pbase_ = b_;
      }
    }

    explicit operator bool () const {return p_ != nullptr;}

    // Note: move-assignable to empty only.
    //
    enter_scope (enter_scope&& x) {*this = move (x);}
    enter_scope& operator= (enter_scope&& x)
    {
      if (this != &x)
      {
        p_ = x.p_;
        r_ = x.r_;
        s_ = x.s_;
        b_ = x.b_;
        x.p_ = nullptr;
      }
      return *this;
    }

    enter_scope (const enter_scope&) = delete;
    enter_scope& operator= (const enter_scope&) = delete;

  private:
    parser* p_;
    scope* r_;
    scope* s_;
    const dir_path* b_; // Pattern base.
  };

  class parser::enter_target
  {
  public:
    enter_target (): p_ (nullptr), t_ (nullptr) {}

    enter_target (parser& p, target& t)
        : p_ (&p), t_ (p.target_)
    {
      p.target_ = &t;
    }

    enter_target (parser& p,
                  name&& n,  // If n.pair, then o is out dir.
                  name&& o,
                  bool implied,
                  const location& loc,
                  tracer& tr)
        : p_ (&p), t_ (p.target_)
    {
      p.target_ = &insert_target (p, move (n), move (o), implied, loc, tr);
    }

    // Find or insert.
    //
    static target&
    insert_target (parser& p,
                   name&& n,  // If n.pair, then o is out dir.
                   name&& o,
                   bool implied,
                   const location& loc,
                   tracer& tr)
    {
      auto r (p.scope_->find_target_type (n, o, loc));
      return p.ctx.targets.insert (r.first,         // target type
                                   move (n.dir),
                                   move (o.dir),
                                   move (n.value),
                                   move (r.second), // extension
                                   implied,
                                   tr).first;
    }

    // Only find.
    //
    static const target*
    find_target (parser& p,
                 name& n,  // If n.pair, then o is out dir.
                 name& o,
                 const location& loc,
                 tracer& tr)
    {
      auto r (p.scope_->find_target_type (n, o, loc));
      return p.ctx.targets.find (r.first,  // target type
                                 n.dir,
                                 o.dir,
                                 n.value,
                                 r.second, // extension
                                 tr);
    }

    ~enter_target ()
    {
      if (p_ != nullptr)
        p_->target_ = t_;
    }

    // Note: move-assignable to empty only.
    //
    enter_target (enter_target&& x) {*this = move (x);}
    enter_target& operator= (enter_target&& x) {
      p_ = x.p_; t_ = x.t_; x.p_ = nullptr; return *this;}

    enter_target (const enter_target&) = delete;
    enter_target& operator= (const enter_target&) = delete;

  private:
    parser* p_;
    target* t_;
  };

  class parser::enter_prerequisite
  {
  public:
    enter_prerequisite (): p_ (nullptr), r_ (nullptr) {}

    enter_prerequisite (parser& p, prerequisite& r)
        : p_ (&p), r_ (p.prerequisite_)
    {
      assert (p.target_ != nullptr);
      p.prerequisite_ = &r;
    }

    ~enter_prerequisite ()
    {
      if (p_ != nullptr)
        p_->prerequisite_ = r_;
    }

    // Note: move-assignable to empty only.
    //
    enter_prerequisite (enter_prerequisite&& x) {*this = move (x);}
    enter_prerequisite& operator= (enter_prerequisite&& x) {
      p_ = x.p_; r_ = x.r_; x.p_ = nullptr; return *this;}

    enter_prerequisite (const enter_prerequisite&) = delete;
    enter_prerequisite& operator= (const enter_prerequisite&) = delete;

  private:
    parser* p_;
    prerequisite* r_;
  };

  void parser::
  parse_buildfile (istream& is, const path_name& in, scope& root, scope& base)
  {
    lexer l (is, in);
    parse_buildfile (l, root, base);
  }

  void parser::
  parse_buildfile (lexer& l, scope& root, scope& base)
  {
    path_ = &l.name ();
    lexer_ = &l;
    root_ = &root;
    scope_ = &base;
    pbase_ = scope_->src_path_;
    target_ = nullptr;
    prerequisite_ = nullptr;
    default_target_ = nullptr;

    enter_buildfile (*path_); // Needs scope_.

    token t;
    type tt;
    next (t, tt);

    parse_clause (t, tt);

    if (tt != type::eos)
      fail (t) << "unexpected " << t;

    process_default_target (t);
  }

  token parser::
  parse_variable (lexer& l, scope& s, const variable& var, type kind)
  {
    path_ = &l.name ();
    lexer_ = &l;
    scope_ = &s;
    pbase_ = scope_->src_path_; // Normally NULL.
    target_ = nullptr;
    prerequisite_ = nullptr;

    token t;
    type tt;
    parse_variable (t, tt, var, kind);
    return t;
  }

  pair<value, token> parser::
  parse_variable_value (lexer& l,
                        scope& s,
                        const dir_path* b,
                        const variable& var)
  {
    path_ = &l.name ();
    lexer_ = &l;
    scope_ = &s;
    pbase_ = b;
    target_ = nullptr;
    prerequisite_ = nullptr;

    token t;
    type tt;
    value rhs (parse_variable_value (t, tt));

    value lhs;
    apply_value_attributes (&var, lhs, move (rhs), type::assign);

    return make_pair (move (lhs), move (t));
  }

  bool parser::
  parse_clause (token& t, type& tt, bool one)
  {
    tracer trace ("parser::parse_clause", &path_);

    // This function should be called in the normal lexing mode with the first
    // token of a line or an alternative arrangements may have to be made to
    // recognize the attributes.
    //
    // It should also always stop at a token that is at the beginning of the
    // line (except for eof). That is, if something is called to parse a line,
    // it should parse it until newline (or fail). This is important for
    // if-else blocks, directory scopes, etc., that assume the '}' token they
    // see is on the new line.
    //
    bool parsed (false);

    while (tt != type::eos && !(one && parsed))
    {
      // Extract attributes if any.
      //
      assert (attributes_.empty ());
      auto at (attributes_push (t, tt));

      // We always start with one or more names, potentially <>-grouped.
      //
      if (!(start_names (tt) || tt == type::labrace))
      {
        // Something else. Let our caller handle that.
        //
        if (at.first)
          fail (at.second) << "attributes before " << t;
        else
          attributes_pop ();

        break;
      }

      // Now we will either parse something or fail.
      //
      if (!parsed)
        parsed = true;

      // See if this is one of the directives.
      //
      if (tt == type::word && keyword (t))
      {
        const string& n (t.value);
        void (parser::*f) (token&, type&) = nullptr;

        // @@ Is this the only place where some of these are valid? Probably
        // also in the var namespace?
        //
        if (n == "assert" ||
            n == "assert!")
        {
          f = &parser::parse_assert;
        }
        else if (n == "print") // Unlike text goes to stdout.
        {
          f = &parser::parse_print;
        }
        else if (n == "fail"  ||
                 n == "warn"  ||
                 n == "info"  ||
                 n == "text")
        {
          f = &parser::parse_diag;
        }
        else if (n == "dump")
        {
          f = &parser::parse_dump;
        }
        else if (n == "source")
        {
          f = &parser::parse_source;
        }
        else if (n == "include")
        {
          f = &parser::parse_include;
        }
        else if (n == "run")
        {
          f = &parser::parse_run;
        }
        else if (n == "import")
        {
          f = &parser::parse_import;
        }
        else if (n == "export")
        {
          f = &parser::parse_export;
        }
        else if (n == "using" ||
                 n == "using?")
        {
          f = &parser::parse_using;
        }
        else if (n == "define")
        {
          f = &parser::parse_define;
        }
        else if (n == "if" ||
                 n == "if!")
        {
          f = &parser::parse_if_else;
        }
        else if (n == "else" ||
                 n == "elif" ||
                 n == "elif!")
        {
          // Valid ones are handled in parse_if_else().
          //
          fail (t) << n << " without if";
        }
        else if (n == "switch")
        {
          f = &parser::parse_switch;
        }
        else if (n == "case"  ||
                 n == "default")
        {
          // Valid ones are handled in parse_switch().
          //
          fail (t) << n << " outside switch";
        }
        else if (n == "for")
        {
          f = &parser::parse_for;
        }

        if (f != nullptr)
        {
          if (at.first)
            fail (at.second) << "attributes before " << n;
          else
            attributes_pop ();

          (this->*f) (t, tt);
          continue;
        }
      }

      location nloc (get_location (t));
      names ns;

      if (tt != type::labrace)
      {
        ns = parse_names (t, tt, pattern_mode::ignore);

        // Allow things like function calls that don't result in anything.
        //
        if (tt == type::newline && ns.empty ())
        {
          if (at.first)
            fail (at.second) << "standalone attributes";
          else
            attributes_pop ();

          next (t, tt);
          continue;
        }
      }

      // Handle ad hoc target group specification (<...>).
      //
      // We keep an "optional" (empty) vector of names parallel to ns.
      //
      adhoc_names ans;
      if (tt == type::labrace)
      {
        while (tt == type::labrace)
        {
          // Parse target names inside < >.
          //
          // We "reserve" the right to have attributes inside <> though what
          // exactly that would mean is unclear. One potentially useful
          // semantics would be the ability to specify attributes for ad hoc
          // members though the fact that the primary target is listed first
          // would make it rather unintuitive.
          //
          next_with_attributes (t, tt);

          auto at (attributes_push (t, tt));

          if (at.first)
            fail (at.second) << "attributes before ad hoc target";
          else
            attributes_pop ();

          // Allow empty case (<>).
          //
          if (tt != type::rabrace)
          {
            location aloc (get_location (t));

            // The first name (or a pair) is the primary target which we need
            // to keep in ns. The rest, if any, are ad hoc members that we
            // should move to ans.
            //
            size_t m (ns.size ());
            parse_names (t, tt, ns, pattern_mode::ignore);
            size_t n (ns.size ());

            // Another empty case (<$empty>).
            //
            if (m != n)
            {
              m = n - m - (ns[m].pair ? 2 : 1); // Number of names to move.

              // Allow degenerate case with just the primary target.
              //
              if (m != 0)
              {
                n -= m; // Number of names in ns we should end up with.

                ans.resize (n); // Catch up with the names vector.
                adhoc_names_loc& a (ans.back ());

                a.loc = move (aloc);
                a.ns.insert (a.ns.end (),
                             make_move_iterator (ns.begin () + n),
                             make_move_iterator (ns.end ()));
                ns.resize (n);
              }
            }
          }

          if (tt != type::rabrace)
            fail (t) << "expected '>' instead of " << t;

          // Parse the next chunk of target names after >, if any.
          //
          next (t, tt);
          if (start_names (tt))
            parse_names (t, tt, ns, pattern_mode::ignore);
        }

        if (!ans.empty ())
          ans.resize (ns.size ()); // Catch up with the final chunk.

        if (tt != type::colon)
          fail (t) << "expected ':' instead of " << t;

        if (ns.empty ())
          fail (t) << "expected target before ':'";
      }

      // If we have a colon, then this is target-related.
      //
      if (tt == type::colon)
      {
        // While '{}:' means empty name, '{$x}:' where x is empty list
        // means empty list.
        //
        if (ns.empty ())
          fail (t) << "expected target before ':'";

        if (at.first)
          fail (at.second) << "attributes before target";
        else
          attributes_pop ();

        // Call the specified parsing function (either variable or block) for
        // each target. We handle multiple targets by replaying the tokens
        // since the value/block may contain variable expansions that would be
        // sensitive to the target context in which they are evaluated. The
        // function signature is:
        //
        // void (token& t, type& tt, const target_type* type, string pat)
        //
        auto for_each = [this, &trace,
                         &t, &tt,
                         &ns, &nloc, &ans] (auto&& f)
        {
          // Note: watch out for an out-qualified single target (two names).
          //
          replay_guard rg (*this,
                           ns.size () > 2 || (ns.size () == 2 && !ns[0].pair));

          for (size_t i (0), e (ns.size ()); i != e; )
          {
            name& n (ns[i]);

            if (n.qualified ())
              fail (nloc) << "project name in target " << n;

            // Figure out if this is a target or a target type/pattern (yeah,
            // it can be a mixture).
            //
            if (path_pattern (n.value))
            {
              if (n.pair)
                fail (nloc) << "out-qualified target type/pattern";

              if (!ans.empty () && !ans[i].ns.empty ())
                fail (ans[i].loc) << "ad hoc member in target type/pattern";

              // If we have the directory, then it is the scope.
              //
              enter_scope sg;
              if (!n.dir.empty ())
                sg = enter_scope (*this, move (n.dir));

              // Resolve target type. If none is specified or if it is '*',
              // use the root of the hierarchy. So these are all equivalent:
              //
              // *: foo = bar
              // {*}: foo = bar
              // *{*}: foo = bar
              //
              const target_type* ti (
                n.untyped () || n.type == "*"
                ? &target::static_type
                : scope_->find_target_type (n.type));

              if (ti == nullptr)
                fail (nloc) << "unknown target type " << n.type;

              f (t, tt, ti, move (n.value));
            }
            else
            {
              name o (n.pair ? move (ns[++i]) : name ());
              enter_target tg (*this,
                               move (n),
                               move (o),
                               true /* implied */,
                               nloc,
                               trace);

              // Enter ad hoc members.
              //
              if (!ans.empty ())
              {
                // Note: index after the pair increment.
                //
                enter_adhoc_members (move (ans[i]), true /* implied */);
              }

              f (t, tt, nullptr, string ());
            }

            if (++i != e)
              rg.play (); // Replay.
          }
        };

        next_with_attributes (t, tt); // Recognize attributes after `:`.

        if (tt == type::newline)
        {
          // See if this is a target block.
          //
          // Note that we cannot just let parse_dependency() handle this case
          // because we can have (a mixture of) target type/patterns.
          //
          if (next (t, tt) == type::lcbrace && peek () == type::newline)
          {
            next (t, tt); // Newline.

            // Parse the block for each target.
            //
            for_each ([this] (token& t, type& tt,
                              const target_type* type, string pat)
                      {
                        next (t, tt); // First token inside the block.

                        parse_variable_block (t, tt, type, move (pat));

                        if (tt != type::rcbrace)
                          fail (t) << "expected '}' instead of " << t;
                      });

            next (t, tt);                    // Presumably newline after '}'.
            next_after_newline (t, tt, '}'); // Should be on its own line.
          }
          else
          {
            // If not followed by a block, then it's a target without any
            // prerequisites. We, however, cannot just fall through to the
            // parse_dependency() call because we have already seen the next
            // token.
            //
            // Note also that we treat this as an explicit dependency
            // declaration (i.e., not implied).
            //
            enter_targets (move (ns), nloc, move (ans), 0);
          }

          continue;
        }

        // Target-specific variable assignment or dependency declaration,
        // including a dependency chain and/or prerequisite-specific variable
        // assignment.
        //
        auto at (attributes_push (t, tt));

        if (!start_names (tt))
          fail (t) << "unexpected " << t;

        // @@ PAT: currently we pattern-expand target-specific vars.
        //
        const location ploc (get_location (t));
        names pns (parse_names (t, tt, pattern_mode::expand));

        // Target-specific variable assignment.
        //
        if (tt == type::assign || tt == type::prepend || tt == type::append)
        {
          type akind (tt);
          const location aloc (get_location (t));

          const variable& var (parse_variable_name (move (pns), ploc));
          apply_variable_attributes (var);

          if (var.visibility > variable_visibility::target)
          {
            fail (nloc) << "variable " << var << " has " << var.visibility
                        << " visibility but is assigned on a target";
          }

          // Parse the assignment for each target.
          //
          for_each ([this, &var, akind, &aloc] (token& t, type& tt,
                                                const target_type* type,
                                                string pat)
                    {
                      if (type == nullptr)
                        parse_variable (t, tt, var, akind);
                      else
                        parse_type_pattern_variable (t, tt,
                                                     *type, move (pat),
                                                     var, akind, aloc);
                    });

          next_after_newline (t, tt);
        }
        // Dependency declaration potentially followed by a chain and/or a
        // prerequisite-specific variable assignment/block.
        //
        else
        {
          if (at.first)
            fail (at.second) << "attributes before prerequisites";
          else
            attributes_pop ();

          bool r (parse_dependency (t, tt,
                                    move (ns), nloc,
                                    move (ans),
                                    move (pns), ploc));
          assert (r); // Block must have been claimed.
        }

        continue;
      }

      // Variable assignment.
      //
      // This can take any of the following forms:
      //
      //        x = y
      //   foo/ x = y   (ns will have two elements)
      //
      // And in the future we may also want to support:
      //
      //   foo/ bar/ x = y
      //
      // Note that we don't support this:
      //
      //   foo/ [attrs] x = y
      //
      // Because the meaning of `[attrs]` would be ambiguous (it could also be
      // a name). Note that the above semantics can be easily achieved with an
      // explicit directory scope:
      //
      //   foo/
      //   {
      //     [attrs] x = y
      //   }
      //
      if (tt == type::assign || tt == type::prepend || tt == type::append)
      {
        // Detect and handle the directory scope. If things look off, then we
        // let parse_variable_name() complain.
        //
        dir_path d;
        if (ns.size () == 2 && ns[0].directory ())
        {
          if (at.first)
            fail (at.second) << "attributes before scope directory";

          d = move (ns[0].dir);
          ns.erase (ns.begin ());

          // Make sure it's not a pattern (see also the target case above and
          // scope below).
          //
          if (path_pattern (d))
            fail (nloc) << "pattern in directory " << d.representation ();
        }

        const variable& var (parse_variable_name (move (ns), nloc));
        apply_variable_attributes (var);

        if (var.visibility >= variable_visibility::target)
        {
          diag_record dr (fail (nloc));

          dr << "variable " << var << " has " << var.visibility
             << " visibility but is assigned on a scope";

          if (var.visibility == variable_visibility::target)
            dr << info << "consider changing it to '*: " << var << "'";
        }

        {
          enter_scope sg (d.empty ()
                          ? enter_scope ()
                          : enter_scope (*this, move (d)));
          parse_variable (t, tt, var, tt);
        }

        next_after_newline (t, tt);
        continue;
      }

      // See if this is a directory scope.
      //
      // Note: must be last since we are going to get the next token.
      //
      if (ns.size () == 1 && ns[0].directory () && tt == type::newline)
      {
        token ot (t);

        if (next (t, tt) == type::lcbrace && peek () == type::newline)
        {
          dir_path&& d (move (ns[0].dir));

          // Make sure not a pattern (see also the target and directory cases
          // above).
          //
          if (path_pattern (d))
            fail (nloc) << "pattern in directory " << d.representation ();

          next (t, tt); // Newline.
          next (t, tt); // First token inside the block.

          if (at.first)
            fail (at.second) << "attributes before scope directory";
          else
            attributes_pop ();

          // Can contain anything that a top level can.
          //
          {
            enter_scope sg (*this, move (d));
            parse_clause (t, tt);
          }

          if (tt != type::rcbrace)
            fail (t) << "expected name or '}' instead of " << t;

          next (t, tt);                    // Presumably newline after '}'.
          next_after_newline (t, tt, '}'); // Should be on its own line.
          continue;
        }

        t = ot;
        // Fall through to fail.
      }

      fail (t) << "unexpected " << t << " after " << ns;
    }

    return parsed;
  }

  void parser::
  parse_variable_block (token& t, type& tt,
                        const target_type* type, string pat)
  {
    // Parse a target or prerequisite-specific variable block. If type is not
    // NULL, then this is a target type/pattern-specific block.
    //
    // enter: first token of first line in the block (normal lexer mode)
    // leave: rcbrace
    //
    // This is a more restricted variant of parse_clause() that only allows
    // variable assignments.
    //
    tracer trace ("parser::parse_variable_block", &path_);

    while (tt != type::rcbrace && tt != type::eos)
    {
      attributes_push (t, tt);

      location nloc (get_location (t));
      names ns (parse_names (t, tt, pattern_mode::ignore, "variable name"));

      if (tt != type::assign  &&
          tt != type::prepend &&
          tt != type::append)
        fail (t) << "expected variable assignment instead of " << t;

      const variable& var (parse_variable_name (move (ns), nloc));
      apply_variable_attributes (var);

      if (prerequisite_ != nullptr                   &&
          var.visibility > variable_visibility::target)
      {
        fail (t) << "variable " << var << " has " << var.visibility
                 << " visibility but is assigned on a target";
      }

      if (type == nullptr)
        parse_variable (t, tt, var, tt);
      else
        parse_type_pattern_variable (t, tt,
                                     *type, pat, // Note: can't move.
                                     var, tt, get_location (t));

      if (tt != type::newline)
        fail (t) << "expected newline instead of " << t;

      next (t, tt);
    }
  }

  void parser::
  enter_adhoc_members (adhoc_names_loc&& ans, bool implied)
  {
    tracer trace ("parser::enter_adhoc_members", &path_);

    names& ns (ans.ns);
    const location& loc (ans.loc);

    for (size_t i (0); i != ns.size (); ++i)
    {
      name&& n (move (ns[i]));
      name&& o (n.pair ? move (ns[++i]) : name ());

      if (n.qualified ())
        fail (loc) << "project name in target " << n;

      // We derive the path unless the target name ends with the '...' escape
      // which here we treat as the "let the rule derive the path" indicator
      // (see target::split_name() for details). This will only be useful for
      // referring to ad hoc members that are managed by the group's matching
      // rule. Note also that omitting '...' for such a member could be used
      // to override the file name, provided the rule checks if the path has
      // already been derived before doing it itself.
      //
      bool escaped;
      {
        const string& v (n.value);
        size_t p (v.size ());

        escaped = (p > 3 &&
                   v[--p] == '.' && v[--p] == '.' && v[--p] == '.' &&
                   v[--p] != '.');
      }

      target& at (
        enter_target::insert_target (*this,
                                     move (n), move (o),
                                     implied,
                                     loc, trace));

      if (target_ == &at)
        fail (loc) << "ad hoc group member " << at << " is primary target";

      // Add as an ad hoc member at the end of the chain skipping duplicates.
      //
      {
        const_ptr<target>* mp (&target_->member);
        for (; *mp != nullptr; mp = &(*mp)->member)
        {
          if (*mp == &at)
          {
            mp = nullptr;
            break;
          }
        }

        if (mp != nullptr)
        {
          *mp = &at;
          at.group = target_;
        }
      }

      if (!escaped)
      {
        if (file* ft = at.is_a<file> ())
          ft->derive_path ();
      }
    }
  }

  small_vector<reference_wrapper<target>, 1> parser::
  enter_targets (names&& tns, const location& tloc, // Target names.
                 adhoc_names&& ans,                 // Ad hoc target names.
                 size_t prereq_size)
  {
    // Enter all the targets (normally we will have just one) and their ad hoc
    // groups.
    //
    tracer trace ("parser::enter_targets", &path_);

    small_vector<reference_wrapper<target>, 1> tgs;

    for (size_t i (0); i != tns.size (); ++i)
    {
      name&& n (move (tns[i]));
      name&& o (n.pair ? move (tns[++i]) : name ());

      if (n.qualified ())
        fail (tloc) << "project name in target " << n;

      // Make sure none of our targets are patterns (maybe we will allow
      // quoting later).
      //
      if (path_pattern (n.value))
        fail (tloc) << "pattern in target " << n;

      enter_target tg (*this,
                       move (n), move (o),
                       false /* implied */,
                       tloc, trace);

      // Enter ad hoc members.
      //
      if (!ans.empty ())
      {
        // Note: index after the pair increment.
        //
        enter_adhoc_members (move (ans[i]), false /* implied */);
      }

      if (default_target_ == nullptr)
        default_target_ = target_;

      target_->prerequisites_state_.store (2, memory_order_relaxed);
      target_->prerequisites_.reserve (prereq_size);
      tgs.push_back (*target_);
    }

    return tgs;
  }

  bool parser::
  parse_dependency (token& t, token_type& tt,
                    names&& tns, const location& tloc, // Target names.
                    adhoc_names&& ans,                 // Ad hoc target names.
                    names&& pns, const location& ploc, // Prereq names.
                    bool chain)
  {
    // Parse a dependency chain and/or a target/prerequisite-specific variable
    // assignment/block. Return true if the following block (if any) has been
    // "claimed" (the block "belongs" to targets/prerequisites before the last
    // colon).
    //
    // enter: colon (anything else is not handled)
    // leave: - first token on the next line           if returning true
    //        - newline (presumably, must be verified) if returning false
    //
    // Note that top-level call (with chain == false) is expected to always
    // return true.
    //
    // This dual-return "complication" is necessary to handle non-block cases
    // like this:
    //
    // foo: bar
    // {hxx ixx}: install = true
    //
    tracer trace ("parser::parse_dependency", &path_);

    // First enter all the targets.
    //
    small_vector<reference_wrapper<target>, 1> tgs (
      enter_targets (move (tns), tloc, move (ans), pns.size ()));

    // Now enter each prerequisite into each target.
    //
    for (name& pn: pns)
    {
      // We cannot reuse the names if we (potentially) may need to pass them
      // as targets in case of a chain (see below).
      //
      name n (tt != type::colon ? move (pn) : pn);

      auto rp (scope_->find_target_type (n, ploc));
      const target_type* tt (rp.first);
      optional<string>& e (rp.second);

      if (tt == nullptr)
        fail (ploc) << "unknown target type " << n.type;

      // Current dir collapses to an empty one.
      //
      if (!n.dir.empty ())
        n.dir.normalize (false, true);

      // @@ OUT: for now we assume the prerequisite's out is undetermined. The
      // only way to specify an src prerequisite will be with the explicit
      // @-syntax.
      //
      // Perhaps use @file{foo} as a way to specify it is in the out tree,
      // e.g., to suppress any src searches? The issue is what to use for such
      // a special indicator. Also, one can easily and natually suppress any
      // searches by specifying the absolute path.
      //
      prerequisite p (move (n.proj),
                      *tt,
                      move (n.dir),
                      dir_path (),
                      move (n.value),
                      move (e),
                      *scope_);

      for (auto i (tgs.begin ()), e (tgs.end ()); i != e; )
      {
        // Move last prerequisite (which will normally be the only one).
        //
        target& t (*i);
        t.prerequisites_.push_back (++i == e
                                    ? move (p)
                                    : prerequisite (p, memory_order_relaxed));
      }
    }

    // Call the specified parsing function (either variable or block) for each
    // target in tgs (for_each_t) or for the last pns.size() prerequisites of
    // each target (for_each_p).
    //
    // We handle multiple targets and/or prerequisites by replaying the tokens
    // (see the target-specific case for details). The function signature is:
    //
    // void (token& t, type& tt)
    //
    auto for_each_t = [this, &t, &tt, &tgs] (auto&& f)
    {
      replay_guard rg (*this, tgs.size () > 1);

      for (auto ti (tgs.begin ()), te (tgs.end ()); ti != te; )
      {
        target& tg (*ti);
        enter_target tgg (*this, tg);

        f (t, tt);

        if (++ti != te)
          rg.play (); // Replay.
      }
    };

    auto for_each_p = [this, &t, &tt, &tgs, &pns] (auto&& f)
    {
      replay_guard rg (*this, tgs.size () > 1 || pns.size () > 1);

      for (auto ti (tgs.begin ()), te (tgs.end ()); ti != te; )
      {
        target& tg (*ti);
        enter_target tgg (*this, tg);

        for (size_t pn (tg.prerequisites_.size ()), pi (pn - pns.size ());
             pi != pn; )
        {
          enter_prerequisite pg (*this, tg.prerequisites_[pi]);

          f (t, tt);

          if (++pi != pn)
            rg.play (); // Replay.
        }

        if (++ti != te)
          rg.play (); // Replay.
      }
    };

    // Do we have a dependency chain and/or prerequisite-specific variable
    // assignment? If not, check for the target-specific variable block unless
    // this is a chained call (in which case the block, if any, "belongs" to
    // prerequisites).
    //
    if (tt != type::colon)
    {
      if (chain)
        return false;

      next_after_newline (t, tt); // Must be a newline then.

      if (tt == type::lcbrace && peek () == type::newline)
      {
        next (t, tt); // Newline.

        // Parse the block for each target.
        //
        for_each_t ([this] (token& t, token_type& tt)
                    {
                      next (t, tt); // First token inside the block.

                      parse_variable_block (t, tt, nullptr, string ());

                      if (tt != type::rcbrace)
                        fail (t) << "expected '}' instead of " << t;
                    });

        next (t, tt);                    // Presumably newline after '}'.
        next_after_newline (t, tt, '}'); // Should be on its own line.
      }

      return true; // Claimed or isn't any.
    }

    // What should we do if there are no prerequisites (for example, because
    // of an empty wildcard result)? We can fail or we can ignore. In most
    // cases, however, this is probably an error (for example, forgetting to
    // checkout a git submodule) so let's not confuse the user and fail (one
    // can always handle the optional prerequisites case with a variable and
    // an if).
    //
    if (pns.empty ())
      fail (ploc) << "no prerequisites in dependency chain or prerequisite-"
                  << "specific variable assignment";

    next_with_attributes (t, tt); // Recognize attributes after `:`.

    auto at (attributes_push (t, tt));

    // @@ PAT: currently we pattern-expand prerequisite-specific vars.
    //
    const location loc (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::expand)
              : names ());

    // Prerequisite-specific variable assignment.
    //
    if (tt == type::assign || tt == type::prepend || tt == type::append)
    {
      type at (tt);

      const variable& var (parse_variable_name (move (ns), loc));
      apply_variable_attributes (var);

      // Parse the assignment for each prerequisites of each target.
      //
      for_each_p ([this, &var, at] (token& t, token_type& tt)
                {
                  parse_variable (t, tt, var, at);
                });

      // Pretend that we have claimed the block to cause an error if there is
      // one. Failed that, the following would result in a valid (target-
      // specific) block:
      //
      // foo: bar: x = y
      // {
      //   ...
      // }
      //
      next_after_newline (t, tt);
      return true;
    }
    //
    // Dependency chain.
    //
    else
    {
      if (at.first)
        fail (at.second) << "attributes before prerequisites";
      else
        attributes_pop ();

      // Note that we could have "pre-resolved" these prerequisites to actual
      // targets or, at least, made their directories absolute. We don't do it
      // for ease of documentation: with the current semantics we can just say
      // that the dependency chain is equivalent to specifying each dependency
      // separately.
      //
      // Also note that supporting ad hoc target group specification in chains
      // will be complicated. For example, what if prerequisites that have ad
      // hoc targets don't end up being chained? Do we just silently drop
      // them? Also, these are prerequsites first that happened to be reused
      // as target names so perhaps it is the right thing not to support,
      // conceptually.
      //
      if (parse_dependency (t, tt,
                            names (pns), ploc,           // Note: can't move.
                            {} /* ad hoc target name */,
                            move (ns), loc,
                            true /* chain */))
        return true;

      // Claim the block (if any) for these prerequisites if it hasn't been
      // claimed by the inner ones.
      //
      next_after_newline (t, tt); // Must be a newline.

      if (tt == type::lcbrace && peek () == type::newline)
      {
        next (t, tt); // Newline.

        // Parse the block for each prerequisites of each target.
        //
        for_each_p ([this] (token& t, token_type& tt)
                    {
                      next (t, tt); // First token inside the block.

                      parse_variable_block (t, tt, nullptr, string ());

                      if (tt != type::rcbrace)
                        fail (t) << "expected '}' instead of " << t;
                    });

        next (t, tt);                    // Presumably newline after '}'.
        next_after_newline (t, tt, '}'); // Should be on its own line.
      }

      return true; // Claimed or isn't any.
    }
  }

  void parser::
  source (istream& is,
          const path_name& in,
          const location& loc,
          bool enter,
          bool deft)
  {
    tracer trace ("parser::source", &path_);

    l5 ([&]{trace (loc) << "entering " << in;});

    if (enter)
      enter_buildfile (in);

    const path_name* op (path_);
    path_ = &in;

    lexer l (is, *path_);
    lexer* ol (lexer_);
    lexer_ = &l;

    target* odt;
    if (deft)
    {
      odt = default_target_;
      default_target_ = nullptr;
    }

    token t;
    type tt;
    next (t, tt);
    parse_clause (t, tt);

    if (tt != type::eos)
      fail (t) << "unexpected " << t;

    if (deft)
    {
      process_default_target (t);
      default_target_ = odt;
    }

    lexer_ = ol;
    path_ = op;

    l5 ([&]{trace (loc) << "leaving " << in;});
  }

  void parser::
  parse_source (token& t, type& tt)
  {
    // The rest should be a list of buildfiles. Parse them as names in the
    // value mode to get variable expansion and directory prefixes.
    //
    mode (lexer_mode::value, '@');
    next (t, tt);
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::expand, "path", nullptr)
              : names ());

    for (name& n: ns)
    {
      if (n.pair || n.qualified () || n.typed () || n.value.empty ())
        fail (l) << "expected buildfile instead of " << n;

      // Construct the buildfile path.
      //
      path p (move (n.dir));
      p /= path (move (n.value));

      // If the path is relative then use the src directory corresponding
      // to the current directory scope.
      //
      if (scope_->src_path_ != nullptr && p.relative ())
        p = scope_->src_path () / p;

      p.normalize ();

      try
      {
        ifdstream ifs (p);
        source (ifs,
                path_name (p),
                get_location (t),
                true  /* enter */,
                false /* default_target */);
      }
      catch (const io_error& e)
      {
        fail (l) << "unable to read buildfile " << p << ": " << e;
      }
    }

    next_after_newline (t, tt);
  }

  void parser::
  parse_include (token& t, type& tt)
  {
    tracer trace ("parser::parse_include", &path_);

    if (root_->src_path_ == nullptr)
      fail (t) << "inclusion during bootstrap";

    // The rest should be a list of buildfiles. Parse them as names in the
    // value mode to get variable expansion and directory prefixes.
    //
    mode (lexer_mode::value, '@');
    next (t, tt);
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::expand, "path", nullptr)
              : names ());

    for (name& n: ns)
    {
      if (n.pair || n.qualified () || n.typed () || n.empty ())
        fail (l) << "expected buildfile instead of " << n;

      // Construct the buildfile path. If it is a directory, then append
      // 'buildfile'.
      //
      path p (move (n.dir));

      bool a;
      if (n.value.empty ())
        a = true;
      else
      {
        a = path::traits_type::is_separator (n.value.back ());

        try
        {
          p /= path (move (n.value));
        }
        catch (const invalid_path& e)
        {
          fail (l) << "invalid include path '" << e.path << "'";
        }
      }

      if (a)
      {
        // This shouldn't happen but let's make sure.
        //
        if (root_->root_extra == nullptr)
          fail (l) << "buildfile naming scheme is not yet known";

        p /= root_->root_extra->buildfile_file;
      }

      l6 ([&]{trace (l) << "relative path " << p;});

      // Determine new out_base.
      //
      dir_path out_base;

      try
      {
        if (p.relative ())
        {
          out_base = scope_->out_path () / p.directory ();
          out_base.normalize ();
        }
        else
        {
          p.normalize ();

          // Make sure the path is in this project. Include is only meant
          // to be used for intra-project inclusion (plus amalgamation).
          //
          bool in_out (false);
          if (!p.sub (root_->src_path ()) &&
              !(in_out = p.sub (root_->out_path ())))
            fail (l) << "out of project include " << p;

          out_base = in_out
            ? p.directory ()
            : out_src (p.directory (), *root_);
        }
      }
      catch (const invalid_path&)
      {
        // The failure reason can only be the specified 'go past the root'
        // path. Let's print the original path.
        //
        fail (l) << "invalid include path '" << (a ? p.directory () : p)
                 << "'";
      }

      // Switch the scope. Note that we need to do this before figuring
      // out the absolute buildfile path since we may switch the project
      // root and src_root with it (i.e., include into a sub-project).
      //
      scope* ors (root_);
      scope* ocs (scope_);
      const dir_path* opb (pbase_);
      switch_scope (out_base);

      if (root_ == nullptr)
        fail (l) << "out of project include from " << out_base;

      // Use the new scope's src_base to get absolute buildfile path if it is
      // relative.
      //
      if (p.relative ())
        p = scope_->src_path () / p.leaf ();

      l6 ([&]{trace (l) << "absolute path " << p;});

      if (!root_->buildfiles.insert (p).second) // Note: may be "new" root.
      {
        l5 ([&]{trace (l) << "skipping already included " << p;});
        pbase_ = opb;
        scope_ = ocs;
        root_ = ors;
        continue;
      }

      try
      {
        ifdstream ifs (p);
        source (ifs,
                path_name (p),
                get_location (t),
                true  /* enter */,
                true  /* default_target */);
      }
      catch (const io_error& e)
      {
        fail (l) << "unable to read buildfile " << p << ": " << e;
      }

      pbase_ = opb;
      scope_ = ocs;
      root_ = ors;
    }

    next_after_newline (t, tt);
  }

  void parser::
  parse_run (token& t, type& tt)
  {
    // run <name> [<arg>...]
    //

    // Parse the command line as names in the value mode to get variable
    // expansion, etc.
    //
    mode (lexer_mode::value);
    next (t, tt);
    const location l (get_location (t));

    strings args;
    try
    {
      args = convert<strings> (
        tt != type::newline && tt != type::eos
        ? parse_names (t, tt, pattern_mode::ignore, "argument", nullptr)
        : names ());
    }
    catch (const invalid_argument& e)
    {
      fail (l) << "invalid run argument: " << e.what ();
    }

    if (args.empty () || args[0].empty ())
      fail (l) << "expected executable name after run";

    cstrings cargs;
    cargs.reserve (args.size () + 1);
    transform (args.begin (),
               args.end (),
               back_inserter (cargs),
               [] (const string& s) {return s.c_str ();});
    cargs.push_back (nullptr);

    process pr (run_start (3            /* verbosity */,
                           cargs,
                           0            /* stdin  */,
                           -1           /* stdout */,
                           true         /* error  */,
                           dir_path ()  /* cwd    */,
                           nullptr      /* env    */,
                           l));
    try
    {
      // While a failing process could write garbage to stdout, for simplicity
      // let's assume it is well behaved.
      //
      ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

      // If there is an error in the output, our diagnostics will look like
      // this:
      //
      // <stdout>:2:3 error: unterminated single quote
      //   buildfile:3:4 info: while parsing foo output
      //
      {
        auto df = make_diag_frame (
          [&args, &l](const diag_record& dr)
          {
            dr << info (l) << "while parsing " << args[0] << " output";
          });

        source (is,
                path_name ("<stdout>"),
                l,
                false /* enter */,
                false /* default_target */);
      }

      is.close (); // Detect errors.
    }
    catch (const io_error& e)
    {
      if (run_wait (cargs, pr, l))
        fail (l) << "io error reading " << cargs[0] << " output: " << e;

      // If the child process has failed then assume the io error was
      // caused by that and let run_finish() deal with it.
    }

    run_finish (cargs, pr, l);

    next_after_newline (t, tt);
  }

  void parser::
  parse_import (token& t, type& tt)
  {
    tracer trace ("parser::parse_import", &path_);

    if (root_->src_path_ == nullptr)
      fail (t) << "import during bootstrap";

    // General import format:
    //
    // import [<var>=](<project>|<project>/<target>])+
    //
    type atype; // Assignment type.
    value* val (nullptr);
    const build2::variable* var (nullptr);

    // We are now in the normal lexing mode and here is the problem: we need
    // to switch to the value mode so that we don't treat certain characters
    // as separators (e.g., + in 'libstdc++'). But at the same time we need
    // to detect if we have the <var>= part. So what we are going to do is
    // switch to the value mode, get the first token, and then re-parse it
    // manually looking for =/=+/+=.
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    // Get variable attributes, if any (note that here we will go into a
    // nested value mode with a different pair character).
    //
    auto at (attributes_push (t, tt));

    const location vloc (get_location (t));

    if (tt == type::word)
    {
      // Split the token into the variable name and value at position (p) of
      // '=', taking into account leading/trailing '+'. The variable name is
      // returned while the token is set to value. If the resulting token
      // value is empty, get the next token. Also set assignment type (at).
      //
      auto split = [&atype, &t, &tt, this] (size_t p) -> string
      {
        string& v (t.value);
        size_t e;

        if (p != 0 && v[p - 1] == '+') // +=
        {
          e = p--;
          atype = type::append;
        }
        else if (p + 1 != v.size () && v[p + 1] == '+') // =+
        {
          e = p + 1;
          atype = type::prepend;
        }
        else // =
        {
          e = p;
          atype = type::assign;
        }

        string nv (v, e + 1); // value
        v.resize (p);         // var name
        v.swap (nv);

        if (v.empty ())
          next (t, tt);

        return nv;
      };

      // Is this the 'foo=...' case?
      //
      size_t p (t.value.find ('='));
      auto& vp (ctx.var_pool.rw (*scope_));

      if (p != string::npos)
        var = &vp.insert (split (p), true /* overridable */);
      //
      // This could still be the 'foo =...' case.
      //
      else if (peek () == type::word)
      {
        const string& v (peeked ().value);
        size_t n (v.size ());

        // We should start with =/+=/=+.
        //
        if (n > 0 &&
            (v[p = 0] == '=' ||
             (n > 1 && v[0] == '+' && v[p = 1] == '=')))
        {
          var = &vp.insert (move (t.value), true /* overridable */);
          next (t, tt); // Get the peeked token.
          split (p);    // Returned name should be empty.
        }
      }
    }

    if (var != nullptr)
    {
      apply_variable_attributes (*var);

      if (var->visibility >= variable_visibility::target)
      {
        fail (vloc) << "variable " << *var << " has " << var->visibility
                    << " visibility but is assigned in import";
      }

      val = atype == type::assign
        ? &scope_->assign (*var)
        : &scope_->append (*var);
    }
    else
    {
      if (at.first)
        fail (at.second) << "attributes without variable";
      else
        attributes_pop ();
    }

    // The rest should be a list of projects and/or targets. Parse them as
    // names to get variable expansion and directory prefixes. Note: doesn't
    // make sense to expand patterns (what's the base directory?)
    //
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::ignore)
              : names ());

    for (name& n: ns)
    {
      if (n.pair)
        fail (l) << "unexpected pair in import";

      // import() will check the name, if required.
      //
      names r (import (*scope_, move (n), l));

      if (val != nullptr)
      {
        if (atype == type::assign)
        {
          val->assign (move (r), var);
          atype = type::append; // Append subsequent values.
        }
        else if (atype == type::prepend)
          val->prepend (move (r), var);
        else
          val->append (move (r), var);
      }
    }

    next_after_newline (t, tt);
  }

  void parser::
  parse_export (token& t, type& tt)
  {
    tracer trace ("parser::parse_export", &path_);

    scope* ps (scope_->parent_scope ());

    // This should be temp_scope.
    //
    if (ps == nullptr || ps->out_path () != scope_->out_path ())
      fail (t) << "export outside export stub";

    // The rest is a value. Parse it similar to a value on the RHS of an
    // assignment to get expansion. While it may seem like supporting
    // attributes is a good idea here, there is actually little benefit in
    // being able to type them or to return NULL.
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    auto at (attributes_push (t, tt));

    if (at.first)
      fail (at.second) << "attributes in export";
    else
      attributes_pop ();

    location l (get_location (t));
    value val (tt != type::newline && tt != type::eos
               ? parse_value (t, tt, pattern_mode::expand)
               : value (names ()));

    if (!val)
      fail (l) << "null value in export";

    if (val.type != nullptr)
      untypify (val);

    export_value_ = move (val).as<names> ();

    if (export_value_.empty ())
      fail (l) << "empty value in export";

    next_after_newline (t, tt);
  }

  void parser::
  parse_using (token& t, type& tt)
  {
    tracer trace ("parser::parse_using", &path_);

    bool optional (t.value.back () == '?');

    if (optional && boot_)
      fail (t) << "optional module in bootstrap";

    // The rest should be a list of module names. Parse them as names in the
    // value mode to get variable expansion, etc.
    //
    mode (lexer_mode::value, '@');
    next (t, tt);
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::ignore, "module", nullptr)
              : names ());

    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      string n;
      standard_version v;

      if (!i->simple ())
        fail (l) << "expected module name instead of " << *i;

      n = move (i->value);

      if (i->pair)
      try
      {
        if (i->pair != '@')
          fail (l) << "unexpected pair style in using directive";

        ++i;
        if (!i->simple ())
          fail (l) << "expected module version instead of " << *i;

        v = standard_version (i->value, standard_version::allow_earliest);
      }
      catch (const invalid_argument& e)
      {
        fail (l) << "invalid module version '" << i->value << "': " << e;
      }

      // Handle the special 'build' module.
      //
      if (n == "build")
      {
        standard_version_constraint c (move (v), false, nullopt, true); // >=

        if (!v.empty ())
          check_build_version (c, l);
      }
      else
      {
        assert (v.empty ()); // Module versioning not yet implemented.

        if (boot_)
          boot_module (*root_, n, l);
        else
          init_module (*root_, *scope_, n, l, optional);
      }
    }

    next_after_newline (t, tt);
  }

  void parser::
  parse_define (token& t, type& tt)
  {
    // define <derived>: <base>
    //
    // See tests/define.
    //
    if (next (t, tt) != type::word)
      fail (t) << "expected name instead of " << t << " in target type "
               << "definition";

    string dn (move (t.value));
    const location dnl (get_location (t));

    if (next (t, tt) != type::colon)
      fail (t) << "expected ':' instead of " << t << " in target type "
               << "definition";

    next (t, tt);

    if (tt == type::word)
    {
      // Target.
      //
      const string& bn (t.value);
      const target_type* bt (scope_->find_target_type (bn));

      if (bt == nullptr)
        fail (t) << "unknown target type " << bn;

      if (!root_->derive_target_type (move (dn), *bt).second)
        fail (dnl) << "target type " << dn << " already defined in this "
                   << "project";

      next (t, tt); // Get newline.
    }
    else
      fail (t) << "expected name instead of " << t << " in target type "
               << "definition";

    next_after_newline (t, tt);
  }

  void parser::
  parse_if_else (token& t, type& tt)
  {
    // Handle the whole if-else chain. See tests/if-else.
    //
    bool taken (false); // One of the branches has been taken.

    for (;;)
    {
      string k (move (t.value));

      next_with_attributes (t, tt); // Recognize attributes before value.

      bool take (false); // Take this branch?

      if (k != "else")
      {
        // Should we evaluate the expression if one of the branches has
        // already been taken? On the one hand, evaluating it is a waste
        // of time. On the other, it can be invalid and the only way for
        // the user to know their buildfile is valid is to test every
        // branch. There could also be side effects. We also have the same
        // problem with ignored branch blocks except there evaluating it
        // is not an option. So let's skip it.
        //
        if (taken)
          skip_line (t, tt);
        else
        {
          if (tt == type::newline || tt == type::eos)
            fail (t) << "expected " << k << "-expression instead of " << t;

          // Parse the condition similar to a value on the RHS of an
          // assignment (expansion, attributes). While at this stage the
          // attribute's usefulness in this context is not entirely clear, we
          // allow it for consistency with other similar directives (switch,
          // for) and also who knows what attributes we will have in the
          // future (maybe there will be a way to cast 0/[null] to bool, for
          // example).
          //
          // Note also that we expand patterns (they could be used in nested
          // contexts, etc; e.g., "if pattern expansion is empty" condition).
          //
          const location l (get_location (t));

          try
          {
            // Should evaluate to 'true' or 'false'.
            //
            bool e (
              convert<bool> (
                parse_value_with_attributes (t, tt,
                                             pattern_mode::expand,
                                             "expression",
                                             nullptr)));

            take = (k.back () == '!' ? !e : e);
          }
          catch (const invalid_argument& e) { fail (l) << e; }
        }
      }
      else
        take = !taken;

      if (tt != type::newline)
        fail (t) << "expected newline instead of " << t << " after " << k
                 << (k != "else" ? "-expression" : "");

      // This can be a block or a single line. The block part is a bit
      // tricky, consider:
      //
      // else
      //   {hxx cxx}{options}: install = false
      //
      // So we treat it as a block if it's followed immediately by newline.
      //
      // Note: identical code in parse_switch().
      //
      if (next (t, tt) == type::lcbrace && peek () == type::newline)
      {
        next (t, tt); // Get newline.
        next (t, tt);

        if (take)
        {
          parse_clause (t, tt);
          taken = true;
        }
        else
          skip_block (t, tt);

        if (tt != type::rcbrace)
          fail (t) << "expected name or '}' instead of " << t
                   << " at the end of " << k << "-block";

        next (t, tt);                    // Presumably newline after '}'.
        next_after_newline (t, tt, '}'); // Should be on its own line.
      }
      else
      {
        if (take)
        {
          if (!parse_clause (t, tt, true))
            fail (t) << "expected " << k << "-line instead of " << t;

          taken = true;
        }
        else
        {
          skip_line (t, tt);

          if (tt == type::newline)
            next (t, tt);
        }
      }

      // See if we have another el* keyword.
      //
      if (k != "else" && tt == type::word && keyword (t))
      {
        const string& n (t.value);

        if (n == "else" || n == "elif" || n == "elif!")
          continue;
      }

      break;
    }
  }

  void parser::
  parse_switch (token& t, type& tt)
  {
    // switch <value> [: <func> [<arg>]] [, <value>...]
    // {
    //   case <pattern> [, <pattern>...]
    //     <line>
    //
    //   case <pattern> [, <pattern>...]
    //   {
    //     <block>
    //   }
    //
    //   case <pattern> [, <pattern>...]
    //   ...
    //   case <pattern> [, <pattern>...]
    //     ...
    //
    //   case <pattern> [| <pattern>... ]
    //
    //   default
    //     ...
    // }

    assert (!pre_parse_); // Used to skip pattern alternatives.

    // Parse and evaluate the values we are matching. Similar to if-else, we
    // expand patterns.
    //
    struct expr
    {
      build2::value    value;
      optional<string> func;
      names            arg;
    };
    small_vector<expr, 1> exprs;

    mode (lexer_mode::switch_expressions); // Recognize `:` and `,`.

    do
    {
      next_with_attributes (t, tt); // Recognize attributes before value.

      if (tt == type::newline || tt == type::eos)
        fail (t) << "expected switch expression instead of " << t;

      expr e;

      e.value =
        parse_value_with_attributes (t, tt,
                                     pattern_mode::expand,
                                     "expression",
                                     nullptr);

      if (tt == type::colon)
      {
        next (t, tt);
        const location l (get_location (t));
        names ns (parse_names (t, tt, pattern_mode::ignore, "function name"));

        if (ns.empty () || ns[0].empty ())
          fail (l) << "function name expected after ':'";

        if (!ns[0].simple ())
          fail (l) << "function name expected instead of " << ns[0];

        e.func = move (ns[0].value);
        ns.erase (ns.begin ());
        e.arg = move (ns);
      }

      exprs.push_back (move (e));
    }
    while (tt == type::comma);

    next_after_newline (t, tt, "switch expression");

    // Next we should always have a block.
    //
    if (tt != type::lcbrace)
      fail (t) << "expected '{' instead of " << t << " after switch";

    next (t, tt);
    next_after_newline (t, tt, '{');

    // Next we have zero or more `case` lines/blocks (potentially with
    // multiple `case`s per line/block) optionally followed by the `default`
    // lines/blocks followed by the closing `}`.
    //
    bool taken (false); // One of the case/default has been taken.
    bool seen_default (false);

    auto special = [&seen_default, this] (const token& t, const type& tt)
    {
      if (tt == type::word && keyword (t))
      {
        if (t.value == "case")
        {
          if (seen_default)
            fail (t) << "case after default" <<
              info << "default must be last in the switch block";

          return true;
        }
        else if (t.value == "default")
        {
          if (seen_default)
            fail (t) << "multiple defaults";

          seen_default = true;
          return true;
        }
        // Fall through.
      }

      return false;
    };

    while (tt != type::eos)
    {
      if (tt == type::rcbrace)
        break;

      if (!special (t, tt))
        fail (t) << "expected case or default instead of " << t;

      string k (move (t.value));

      bool take (false); // Take this case/default?
      if (seen_default)
      {
        take = !taken;
        next (t, tt);
      }
      else
      {
        // Similar to if-else we are not going to evaluate the case conditions
        // if we are skipping.
        //
        if (taken)
          skip_line (t, tt);
        else
        {
          // Parse the patterns and match them against the values. Note that
          // here we don't expand patterns in names.
          //
          mode (lexer_mode::case_patterns); // Recognize `|` and `,`.

          auto parse_pattern_with_attributes = [this] (token& t, type& tt)
          {
            return parse_value_with_attributes (
              t, tt, pattern_mode::ignore, "pattern", nullptr);
          };

          for (size_t i (0);; ++i)
          {
            // Recognize attributes before first pattern.
            //
            next_with_attributes (t, tt);

            if (tt == type::newline || tt == type::eos)
              fail (t) << "expected case pattern instead of " << t;

            if (i == exprs.size ())
              fail (t) << "more patterns than switch expressions";

            // Handle pattern alternatives (<pattern>|<pattern>).
            //
            for (;;)
            {
              const location l (get_location (t));
              value p (parse_pattern_with_attributes (t, tt));
              expr& e (exprs[i]); // Note: value might be modified (typified).

              if (e.func)
              {
                // Call <func>(<value>, <pattern> [, <arg>]).
                //
                small_vector<value, 3> args {value (e.value), move (p)};

                if (!e.arg.empty ())
                  args.push_back (value (e.arg));

                value r (ctx.functions.call (scope_, *e.func, args, l));

                // We support two types of functions: matchers and extractors:
                // a matcher returns a statically-typed bool value while an
                // extractor returns NULL if there is no match and the
                // extracted value otherwise.
                //
                if (r.type == &value_traits<bool>::value_type)
                {
                  if (r.null)
                    fail (l) << "match function " << *e.func << " returned "
                             << "null";

                  take = r.as<bool> ();
                }
                else
                  take = !r.null;
              }
              else
                take = compare_values (type::equal, e.value, p, l);

              if (tt != type::bit_or)
                break;

              if (take)
              {
                // Use the pre-parse mechanism to skip remaining alternatives.
                //
                pre_parse_ = true;
                do
                {
                  next_with_attributes (t, tt); // Skip `|`.
                  parse_pattern_with_attributes (t, tt);
                }
                while (tt == type::bit_or);
                pre_parse_ = false;

                break;
              }

              // Recognize attributes before next pattern.
              //
              next_with_attributes (t, tt);
            }

            if (!take)
            {
              skip_line (t, tt); // Skip the rest.
              break;
            }

            // We reserve the ':' separator for possible future match
            // extraction support:
            //
            // case '...': x
            //   info "$x"
            //
            if (tt == type::colon)
              fail (t) << "unexpected ':' (match extraction is not yet "
                       << "supported)";

            if (tt != type::comma)
              break;
          }
        }
      }

      next_after_newline (t, tt, seen_default ? "default" : "case pattern");

      // This can be another `case` or `default`.
      //
      if (special (t, tt))
      {
        // If we are still looking for a match, simply restart from the
        // beginning as if this were the first `case` or `default`.
        //
        if (!take && !taken)
        {
          seen_default = false;
          continue;
        }

        // Otherwise, we need to skip this and all the subsequent special
        // lines.
        //
        do
        {
          skip_line (t, tt);
          next_after_newline (t, tt, seen_default ? "default" : "case pattern");
        }
        while (special (t, tt));
      }

      // Otherwise this must be a block or a single line (the same logic as in
      // if-else).
      //
      if (tt == type::lcbrace && peek () == type::newline)
      {
        next (t, tt); // Get newline.
        next (t, tt);

        if (take)
        {
          parse_clause (t, tt);
          taken = true;
        }
        else
          skip_block (t, tt);

        if (tt != type::rcbrace)
          fail (t) << "expected name or '}' instead of " << t
                   << " at the end of " << k << "-block";

        next (t, tt);                    // Presumably newline after '}'.
        next_after_newline (t, tt, '}'); // Should be on its own line.
      }
      else
      {
        if (take)
        {
          if (!parse_clause (t, tt, true))
            fail (t) << "expected " << k << "-line instead of " << t;

          taken = true;
        }
        else
        {
          skip_line (t, tt);

          if (tt == type::newline)
            next (t, tt);
        }
      }
    }

    if (tt != type::rcbrace)
      fail (t) << "expected '}' instead of " << t << " after switch-block";

    next (t, tt);                    // Presumably newline after '}'.
    next_after_newline (t, tt, '}'); // Should be on its own line.
  }

  void parser::
  parse_for (token& t, type& tt)
  {
    // for <varname>: <value>
    //   <line>
    //
    // for <varname>: <value>
    // {
    //   <block>
    // }

    // First take care of the variable name. There is no reason not to
    // support variable attributes.
    //
    next_with_attributes (t, tt);
    attributes_push (t, tt);

    // @@ PAT: currently we pattern-expand for var.
    //
    const location vloc (get_location (t));
    names vns (parse_names (t, tt, pattern_mode::expand));

    if (tt != type::colon)
      fail (t) << "expected ':' instead of " << t << " after variable name";

    const variable& var (parse_variable_name (move (vns), vloc));
    apply_variable_attributes (var);

    if (var.visibility >= variable_visibility::target)
    {
      fail (vloc) << "variable " << var << " has " << var.visibility
                  << " visibility but is assigned in for-loop";
    }

    // Now the value (list of names) to iterate over. Parse it similar to a
    // value on the RHS of an assignment (expansion, attributes).
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    value val (parse_value_with_attributes (t, tt, pattern_mode::expand));

    // If this value is a vector, then save its element type so that we
    // can typify each element below.
    //
    const value_type* etype (nullptr);

    if (val && val.type != nullptr)
    {
      etype = val.type->element_type;
      untypify (val);
    }

    if (tt != type::newline)
      fail (t) << "expected newline instead of " << t << " after for";

    // Finally the body. The initial thought was to use the token replay
    // facility but on closer inspection this didn't turn out to be a good
    // idea (no support for nested replays, etc). So instead we are going to
    // do a full-blown re-lex. Specifically, we will first skip the line/block
    // just as we do for non-taken if/else branches while saving the character
    // sequence that comprises the body. Then we re-lex/parse it on each
    // iteration.
    //
    string body;
    uint64_t line (lexer_->line); // Line of the first character to be saved.
    lexer::save_guard sg (*lexer_, body);

    // This can be a block or a single line, similar to if-else.
    //
    bool block (next (t, tt) == type::lcbrace && peek () == type::newline);

    if (block)
    {
      next (t, tt); // Get newline.
      next (t, tt);

      skip_block (t, tt);
      sg.stop ();

      if (tt != type::rcbrace)
        fail (t) << "expected '}' instead of " << t << " at the end of "
                 << "for-block";

      next (t, tt);                    // Presumably newline after '}'.
      next_after_newline (t, tt, '}'); // Should be on its own line.
    }
    else
    {
      skip_line (t, tt);
      sg.stop ();

      if (tt == type::newline)
        next (t, tt);
    }

    // Iterate.
    //
    value& v (scope_->assign (var)); // Assign even if no iterations.

    if (!val)
      return;

    names& ns (val.as<names> ());

    if (ns.empty ())
      return;

    istringstream is (move (body));

    for (auto i (ns.begin ()), e (ns.end ());; )
    {
      // Set the variable value.
      //
      bool pair (i->pair);
      names n;
      n.push_back (move (*i));
      if (pair) n.push_back (move (*++i));
      v = value (move (n));

      if (etype != nullptr)
        typify (v, *etype, &var);

      lexer l (is, *path_, line);
      lexer* ol (lexer_);
      lexer_ = &l;

      token t;
      type tt;
      next (t, tt);

      if (block)
      {
        next (t, tt); // {
        next (t, tt); // <newline>
      }

      parse_clause (t, tt);

      if (tt != (block ? type::rcbrace : type::eos))
        fail (t) << "expected name " << (block ? "or '}' " : "")
                 << "instead of " << t;

      lexer_ = ol;

      if (++i == e)
        break;

      // Rewind the stream.
      //
      is.clear ();
      is.seekg (0);
    }
  }

  void parser::
  parse_assert (token& t, type& tt)
  {
    bool neg (t.value.back () == '!');
    const location al (get_location (t));

    // Parse the next chunk (the condition) similar to a value on the RHS of
    // an assignment. We allow attributes (which will only apply to the
    // condition) for the same reason as in if-else (see parse_if_else()).
    //
    mode (lexer_mode::value);
    next_with_attributes (t, tt);

    const location el (get_location (t));

    try
    {
      // Should evaluate to 'true' or 'false'.
      //
      bool e (
        convert<bool> (
          parse_value_with_attributes (t, tt,
                                       pattern_mode::expand,
                                       "expression",
                                       nullptr,
                                       true /* chunk */)));
      e = (neg ? !e : e);

      if (e)
      {
        skip_line (t, tt);

        if (tt != type::eos)
          next (t, tt); // Swallow newline.

        return;
      }
    }
    catch (const invalid_argument& e) { fail (el) << e; }

    // Being here means things didn't end up well. Parse the description, if
    // any, with expansion. Then fail.
    //
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt,
                             pattern_mode::ignore,
                             "description",
                             nullptr)
              : names ());

    diag_record dr (fail (al));

    if (ns.empty ())
      dr << "assertion failed";
    else
      dr << ns;
  }

  void parser::
  parse_print (token& t, type& tt)
  {
    // Parse the rest similar to a value on the RHS of an assignment
    // (expansion, attributes).
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    if (value v = parse_value_with_attributes (t, tt, pattern_mode::expand))
    {
      names storage;
      cout << reverse (v, storage) << endl;
    }
    else
      cout << "[null]" << endl;

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  void parser::
  parse_diag (token& t, type& tt)
  {
    diag_record dr;
    const location l (get_location (t));

    switch (t.value[0])
    {
    case 'f': dr << fail (l); break;
    case 'w': dr << warn (l); break;
    case 'i': dr << info (l); break;
    case 't': dr << text (l); break;
    default: assert (false);
    }

    // Parse the rest similar to a value on the RHS of an assignment
    // (expansion, attributes).
    //
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    if (value v = parse_value_with_attributes (t, tt, pattern_mode::expand))
    {
      names storage;
      dr << reverse (v, storage);
    }

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  void parser::
  parse_dump (token& t, type& tt)
  {
    // dump [<target>...]
    //
    // If there are no targets, then we dump the current scope.
    //
    tracer trace ("parser::parse_dump", &path_);

    const location l (get_location (t));
    next (t, tt);
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt, pattern_mode::ignore)
              : names ());

    text (l) << "dump:";

    // Dump directly into diag_stream.
    //
    ostream& os (*diag_stream);

    if (ns.empty ())
    {
      if (scope_ != nullptr)
        dump (*scope_, "  "); // Indent two spaces.
      else
        os << "  <no current scope>" << endl;
    }
    else
    {
      for (auto i (ns.begin ()), e (ns.end ()); i != e; )
      {
        name& n (*i++);
        name o (n.pair ? move (*i++) : name ());

        const target* t (enter_target::find_target (*this, n, o, l, trace));

        if (t != nullptr)
          dump (*t, "  "); // Indent two spaces.
        else
        {
          os << "  <no target " << n;
          if (n.pair && !o.dir.empty ()) os << '@' << o.dir;
          os << '>' << endl;
        }

        if (i != e)
          os << endl;
      }
    }

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  const variable& parser::
  parse_variable_name (names&& ns, const location& l)
  {
    // The list should contain a single, simple name.
    //
    if (ns.size () != 1 || !ns[0].simple () || ns[0].empty ())
      fail (l) << "expected variable name instead of " << ns;

    string& n (ns[0].value);

    //@@ OLD
    if (n.front () == '.') // Fully qualified name.
      n.erase (0, 1);
    else
    {
      //@@ TODO: append namespace if any.
    }

    return ctx.var_pool.rw (*scope_).insert (move (n), true /* overridable */);
  }

  void parser::
  parse_variable (token& t, type& tt, const variable& var, type kind)
  {
    value rhs (parse_variable_value (t, tt));

    value& lhs (
      kind == type::assign

      ? (prerequisite_ != nullptr ? prerequisite_->assign (var) :
         target_ != nullptr       ? target_->assign (var)       :
         /*                      */ scope_->assign (var))

      : (prerequisite_ != nullptr ? prerequisite_->append (var, *target_) :
         target_ != nullptr       ? target_->append (var)                 :
         /*                      */ scope_->append (var)));

    apply_value_attributes (&var, lhs, move (rhs), kind);
  }

  void parser::
  parse_type_pattern_variable (token& t, token_type& tt,
                               const target_type& type, string pat,
                               const variable& var, token_type kind,
                               const location& loc)
  {
    // Parse target type/pattern-specific variable assignment.
    //
    // See old-tests/variable/type-pattern.

    // Note: expanding the value in the current scope context.
    //
    value rhs (parse_variable_value (t, tt));

    // Leave the value untyped unless we are assigning.
    //
    pair<reference_wrapper<value>, bool> p (
      scope_->target_vars[type][move (pat)].insert (
        var, kind == type::assign));

    value& lhs (p.first);

    // We store prepend/append values untyped (similar to overrides).
    //
    if (rhs.type != nullptr && kind != type::assign)
      untypify (rhs);

    if (p.second)
    {
      // Note: we are always using assign and we don't pass the variable in
      // case of prepend/append in order to keep the value untyped.
      //
      apply_value_attributes (kind == type::assign ? &var : nullptr,
                              lhs,
                              move (rhs),
                              type::assign);

      // Map assignment type to the value::extra constant.
      //
      lhs.extra = (kind == type::prepend ? 1 :
                   kind == type::append  ? 2 :
                   0);
    }
    else
    {
      // Existing value. What happens next depends on what we are trying to do
      // and what's already there.
      //
      // Assignment is the easy one: we simply overwrite what's already
      // there. Also, if we are appending/prepending to a previously assigned
      // value, then we simply append or prepend normally.
      //
      if (kind == type::assign || lhs.extra == 0)
      {
        // Above we've instructed insert() not to type the value so we have to
        // compensate for that now.
        //
        if (kind != type::assign)
        {
          if (var.type != nullptr && lhs.type != var.type)
            typify (lhs, *var.type, &var);
        }
        else
          lhs.extra = 0; // Change to assignment.

        apply_value_attributes (&var, lhs, move (rhs), kind);
      }
      else
      {
        // This is an append/prepent to a previously appended or prepended
        // value. We can handle it as long as things are consistent.
        //
        if (kind == type::prepend && lhs.extra == 2)
          fail (loc) << "prepend to a previously appended target type/pattern-"
                     << "specific variable " << var;

        if (kind == type::append && lhs.extra == 1)
          fail (loc) << "append to a previously prepended target type/pattern-"
                     << "specific variable " << var;

        // Do untyped prepend/append.
        //
        apply_value_attributes (nullptr, lhs, move (rhs), kind);
      }
    }

    if (lhs.extra != 0 && lhs.type != nullptr)
      fail (loc) << "typed prepend/append to target type/pattern-specific "
                 << "variable " << var;
  }

  value parser::
  parse_variable_value (token& t, type& tt)
  {
    mode (lexer_mode::value, '@');
    next_with_attributes (t, tt);

    // Parse value attributes if any. Note that it's ok not to have anything
    // after the attributes (e.g., foo=[null]).
    //
    attributes_push (t, tt, true);

    return tt != type::newline && tt != type::eos
      ? parse_value (t, tt, pattern_mode::expand)
      : value (names ());
  }

  static const value_type*
  map_type (const string& n)
  {
    auto ptr = [] (const value_type& vt) {return &vt;};

    return
      n == "bool"           ? ptr (value_traits<bool>::value_type)           :
      n == "uint64"         ? ptr (value_traits<uint64_t>::value_type)       :
      n == "string"         ? ptr (value_traits<string>::value_type)         :
      n == "path"           ? ptr (value_traits<path>::value_type)           :
      n == "dir_path"       ? ptr (value_traits<dir_path>::value_type)       :
      n == "abs_dir_path"   ? ptr (value_traits<abs_dir_path>::value_type)   :
      n == "name"           ? ptr (value_traits<name>::value_type)           :
      n == "name_pair"      ? ptr (value_traits<name_pair>::value_type)      :
      n == "target_triplet" ? ptr (value_traits<target_triplet>::value_type) :
      n == "project_name"   ? ptr (value_traits<project_name>::value_type)   :

      n == "uint64s"        ? ptr (value_traits<uint64s>::value_type)        :
      n == "strings"        ? ptr (value_traits<strings>::value_type)        :
      n == "paths"          ? ptr (value_traits<paths>::value_type)          :
      n == "dir_paths"      ? ptr (value_traits<dir_paths>::value_type)      :
      n == "names"          ? ptr (value_traits<vector<name>>::value_type)   :

      nullptr;
  }

  void parser::
  apply_variable_attributes (const variable& var)
  {
    attributes a (attributes_pop ());

    if (!a)
      return;

    const location& l (a.loc);
    const value_type* type (nullptr);

    auto print = [storage = names ()] (diag_record& dr, const value& v) mutable
    {
      storage.clear ();
      to_stream (dr.os, reverse (v, storage), true /* quote */, '@');
    };

    for (auto& p: a.ats)
    {
      string& k (p.first);
      value& v (p.second);

      if (const value_type* t = map_type (k))
      {
        if (type != nullptr && t != type)
          fail (l) << "multiple variable types: " << k << ", " << type->name;

        type = t;
        // Fall through.
      }
      else
      {
        diag_record dr (fail (l));
        dr << "unknown variable attribute " << k;

        if (!v.null)
        {
          dr << '=';
          print (dr, v);
        }
      }

      if (!v.null)
      {
        diag_record dr (fail (l));
        dr << "unexpected value for attribute " << k << ": ";
        print (dr, v);
      }
    }

    if (type != nullptr)
    {
      if (var.type == nullptr)
      {
        const bool o (true); // Allow overrides.
        ctx.var_pool.update (const_cast<variable&> (var), type, nullptr, &o);
      }
      else if (var.type != type)
        fail (l) << "changing variable " << var << " type from "
                 << var.type->name << " to " << type->name;
    }
  }

  void parser::
  apply_value_attributes (const variable* var,
                          value& v,
                          value&& rhs,
                          type kind)
  {
    attributes a (attributes_pop ());
    const location& l (a.loc);

    // Essentially this is an attribute-augmented assign/append/prepend.
    //
    bool null (false);
    const value_type* type (nullptr);

    auto print = [storage = names ()] (diag_record& dr, const value& v) mutable
    {
      storage.clear ();
      to_stream (dr.os, reverse (v, storage), true /* quote */, '@');
    };

    for (auto& p: a.ats)
    {
      string& k (p.first);
      value& v (p.second);

      if (k == "null")
      {
        if (rhs && !rhs.empty ()) // Note: null means we had an expansion.
          fail (l) << "value with null attribute";

        null = true;
        // Fall through.
      }
      else if (const value_type* t = map_type (k))
      {
        if (type != nullptr && t != type)
          fail (l) << "multiple value types: " << k << ", " << type->name;

        type = t;
        // Fall through.
      }
      else
      {
        diag_record dr (fail (l));
        dr << "unknown value attribute " << k;

        if (!v.null)
        {
          dr << '=';
          print (dr, v);
        }
      }

      if (!v.null)
      {
        diag_record dr (fail (l));
        dr << "unexpected value for attribute " << k << ": ";
        print (dr, v);
      }
    }

    // When do we set the type and when do we keep the original? This gets
    // tricky for append/prepend where both values contribute. The guiding
    // rule here is that if the user specified the type, then they reasonable
    // expect the resulting value to be of that type. So for assign we always
    // override the type since it's a new value. For append/prepend we
    // override if the LHS value is NULL (which also covers undefined). We
    // also override if LHS is untyped. Otherwise, we require that the types
    // be the same. Also check that the requested value type doesn't conflict
    // with the variable type.
    //
    if (var != nullptr && var->type != nullptr)
    {
      if (type == nullptr)
      {
        type = var->type;
      }
      else if (var->type != type)
      {
        fail (l) << "conflicting variable " << var->name << " type "
                 << var->type->name << " and value type " << type->name;
      }
    }

    // What if both LHS and RHS are typed? For now we do lexical conversion:
    // if this specific value can be converted, then all is good. The
    // alternative would be to do type conversion: if any value of RHS type
    // can be converted to LHS type, then we are good. This may be a better
    // option in the future but currently our parse_names() implementation
    // untypifies everything if there are multiple names. And having stricter
    // rules just for single-element values would be strange.
    //
    // We also have "weaker" type propagation for the RHS type.
    //
    bool rhs_type (false);
    if (rhs.type != nullptr)
    {
      // Only consider RHS type if there is no explicit or variable type.
      //
      if (type == nullptr)
      {
        type = rhs.type;
        rhs_type = true;
      }

      // Reduce this to the untyped value case for simplicity.
      //
      untypify (rhs);
    }

    if (kind == type::assign)
    {
      if (type != v.type)
      {
        v = nullptr; // Clear old value.
        v.type = type;
      }
    }
    else if (type != nullptr)
    {
      if (!v)
        v.type = type;
      else if (v.type == nullptr)
        typify (v, *type, var);
      else if (v.type != type && !rhs_type)
        fail (l) << "conflicting original value type " << v.type->name
                 << " and append/prepend value type " << type->name;
    }

    if (null)
    {
      if (kind == type::assign) // Ignore for prepend/append.
        v = nullptr;
    }
    else
    {
      if (kind == type::assign)
      {
        if (rhs)
          v.assign (move (rhs).as<names> (), var);
        else
          v = nullptr;
      }
      else if (rhs) // Don't append/prepent NULL.
      {
        if (kind == type::prepend)
          v.prepend (move (rhs).as<names> (), var);
        else
          v.append (move (rhs).as<names> (), var);
      }
    }
  }

  value parser::
  parse_value_with_attributes (token& t, token_type& tt,
                               pattern_mode pmode,
                               const char* what,
                               const string* separators,
                               bool chunk)
  {
    // Parse value attributes if any. Note that it's ok not to have anything
    // after the attributes (think [null]).
    //
    attributes_push (t, tt, true);

    value rhs (tt != type::newline && tt != type::eos
               ? parse_value (t, tt, pmode, what, separators, chunk)
               : value (names ()));

    if (pre_parse_)
      return rhs; // Empty.

    value lhs;
    apply_value_attributes (nullptr, lhs, move (rhs), type::assign);
    return lhs;
  }

  values parser::
  parse_eval (token& t, type& tt, pattern_mode pmode)
  {
    // enter: lparen
    // leave: rparen

    mode (lexer_mode::eval, '@'); // Auto-expires at rparen.
    next_with_attributes (t, tt);

    if (tt == type::rparen)
      return values ();

    values r (parse_eval_comma (t, tt, pmode, true));

    if (tt != type::rparen)
      fail (t) << "unexpected " << t; // E.g., stray ':'.

    return r;
  }

  values parser::
  parse_eval_comma (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Left-associative: parse in a loop for as long as we can.
    //
    values r;
    value lhs (parse_eval_ternary (t, tt, pmode, first));

    if (!pre_parse_)
      r.push_back (move (lhs));

    while (tt == type::comma)
    {
      next_with_attributes (t, tt); // Recognize attributes before value.

      value rhs (parse_eval_ternary (t, tt, pmode));

      if (!pre_parse_)
        r.push_back (move (rhs));
    }

    return r;
  }

  value parser::
  parse_eval_ternary (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Right-associative (kind of): we parse what's between ?: without
    // regard for priority and we recurse on what's after :. Here is an
    // example:
    //
    // a ? x ? y : z : b ? c : d
    //
    // This should be parsed/evaluated as:
    //
    // a ? (x ? y : z) : (b ? c : d)
    //
    location l (get_location (t));
    value lhs (parse_eval_or (t, tt, pmode, first));

    if (tt != type::question)
      return lhs;

    // Use the pre-parse mechanism to implement short-circuit.
    //
    bool pp (pre_parse_);

    bool q;
    try
    {
      q = pp ? true : convert<bool> (move (lhs));
    }
    catch (const invalid_argument& e) { fail (l) << e << endf; }

    if (!pp)
      pre_parse_ = !q; // Short-circuit middle?

    next_with_attributes (t, tt); // Recognize attributes before value.

    value mhs (parse_eval_ternary (t, tt, pmode));

    if (tt != type::colon)
      fail (t) << "expected ':' instead of " << t;

    if (!pp)
      pre_parse_ = q; // Short-circuit right?

    next_with_attributes (t, tt); // Recognize attributes before value.

    value rhs (parse_eval_ternary (t, tt, pmode));

    pre_parse_ = pp;
    return q ? move (mhs) : move (rhs);
  }

  value parser::
  parse_eval_or (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Left-associative: parse in a loop for as long as we can.
    //
    location l (get_location (t));
    value lhs (parse_eval_and (t, tt, pmode, first));

    // Use the pre-parse mechanism to implement short-circuit.
    //
    bool pp (pre_parse_);

    while (tt == type::log_or)
    {
      try
      {
        if (!pre_parse_ && convert<bool> (move (lhs)))
          pre_parse_ = true;

        next_with_attributes (t, tt); // Recognize attributes before value.

        l = get_location (t);
        value rhs (parse_eval_and (t, tt, pmode));

        if (pre_parse_)
          continue;

        // Store the result as bool value.
        //
        lhs = convert<bool> (move (rhs));
      }
      catch (const invalid_argument& e) { fail (l) << e; }
    }

    pre_parse_ = pp;
    return lhs;
  }

  value parser::
  parse_eval_and (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Left-associative: parse in a loop for as long as we can.
    //
    location l (get_location (t));
    value lhs (parse_eval_comp (t, tt, pmode, first));

    // Use the pre-parse mechanism to implement short-circuit.
    //
    bool pp (pre_parse_);

    while (tt == type::log_and)
    {
      try
      {
        if (!pre_parse_ && !convert<bool> (move (lhs)))
          pre_parse_ = true;

        next_with_attributes (t, tt); // Recognize attributes before value.

        l = get_location (t);
        value rhs (parse_eval_comp (t, tt, pmode));

        if (pre_parse_)
          continue;

        // Store the result as bool value.
        //
        lhs = convert<bool> (move (rhs));
      }
      catch (const invalid_argument& e) { fail (l) << e; }
    }

    pre_parse_ = pp;
    return lhs;
  }

  value parser::
  parse_eval_comp (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS (lexed with enabled attributes)
    // leave: next token after last RHS

    // Left-associative: parse in a loop for as long as we can.
    //
    value lhs (parse_eval_value (t, tt, pmode, first));

    while (tt == type::equal      ||
           tt == type::not_equal  ||
           tt == type::less       ||
           tt == type::less_equal ||
           tt == type::greater    ||
           tt == type::greater_equal)
    {
      type op (tt);
      location l (get_location (t));

      next_with_attributes (t, tt); // Recognize attributes before value.

      value rhs (parse_eval_value (t, tt, pmode));

      if (pre_parse_)
        continue;

      // Store the result as a bool value.
      //
      lhs = value (compare_values (op, lhs, rhs, l));
    }

    return lhs;
  }

  value parser::
  parse_eval_value (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of value (lexed with enabled attributes)
    // leave: next token after value

    // Parse value attributes if any. Note that it's ok not to have anything
    // after the attributes, as in, ($foo == [null]), or even ([null])
    //
    auto at (attributes_push (t, tt, true));

    const location l (get_location (t));

    value v;
    switch (tt)
    {
    case type::log_not:
      {
        next_with_attributes (t, tt); // Recognize attributes before value.

        v = parse_eval_value (t, tt, pmode);

        if (pre_parse_)
          break;

        try
        {
          // Store the result as bool value.
          //
          v = !convert<bool> (move (v));
        }
        catch (const invalid_argument& e) { fail (l) << e; }
        break;
      }
    default:
      {
        // If parse_value() gets called, it expects to see a value. Note that
        // it will also handle nested eval contexts.
        //
        v = (tt != type::colon         &&
             tt != type::question      &&
             tt != type::comma         &&

             tt != type::rparen        &&

             tt != type::equal         &&
             tt != type::not_equal     &&
             tt != type::less          &&
             tt != type::less_equal    &&
             tt != type::greater       &&
             tt != type::greater_equal &&

             tt != type::log_or        &&
             tt != type::log_and

             ? parse_value (t, tt, pmode)
             : value (names ()));
      }
    }

    // If this is the first expression then handle the eval-qual special case
    // (target-qualified name represented as a special ':'-style pair).
    //
    if (first && tt == type::colon)
    {
      if (at.first)
        fail (at.second) << "attributes before target-qualified variable name";

      if (!pre_parse_)
        attributes_pop ();

      const location nl (get_location (t));
      next (t, tt);
      value n (parse_value (t, tt, pattern_mode::ignore));

      if (tt != type::rparen)
        fail (t) << "expected ')' after variable name";

      if (pre_parse_)
        return v; // Empty.

      if (v.type != nullptr || !v || v.as<names> ().size () != 1)
        fail (l) << "expected target before ':'";

      if (n.type != nullptr || !n || n.as<names> ().size () != 1)
        fail (nl) << "expected variable name after ':'";

      names& ns (v.as<names> ());
      ns.back ().pair = ':';
      ns.push_back (move (n.as<names> ().back ()));
      return v;
    }
    else
    {
      if (pre_parse_)
        return v; // Empty.

      // Process attributes if any.
      //
      if (!at.first)
      {
        attributes_pop ();
        return v;
      }

      value r;
      apply_value_attributes (nullptr, r, move (v), type::assign);
      return r;
    }
  }

  bool parser::
  compare_values (type op, value& lhs, value& rhs, const location& loc) const
  {
    // Use (potentially typed) comparison via value. If one of the values is
    // typed while the other is not, then try to convert the untyped one to
    // the other's type instead of complaining. This seems like a reasonable
    // thing to do and will allow us to write:
    //
    // if ($build.version > 30000)
    //
    // Rather than having to write:
    //
    // if ($build.version > [uint64] 30000)
    //
    if (lhs.type != rhs.type)
    {
      // @@ Would be nice to pass location for diagnostics.
      //
      if (lhs.type == nullptr)
      {
        if (lhs)
          typify (lhs, *rhs.type, nullptr);
      }
      else if (rhs.type == nullptr)
      {
        if (rhs)
          typify (rhs, *lhs.type, nullptr);
      }
      else
        fail (loc) << "comparison between " << lhs.type->name << " and "
                   << rhs.type->name;
    }

    bool r;
    switch (op)
    {
    case type::equal:         r = lhs == rhs; break;
    case type::not_equal:     r = lhs != rhs; break;
    case type::less:          r = lhs <  rhs; break;
    case type::less_equal:    r = lhs <= rhs; break;
    case type::greater:       r = lhs >  rhs; break;
    case type::greater_equal: r = lhs >= rhs; break;
    default:                  r = false;      assert (false);
    }
    return r;
  }

  pair<bool, location> parser::
  attributes_push (token& t, type& tt, bool standalone)
  {
    location l (get_location (t));
    bool has (tt == type::lsbrace);

    if (!pre_parse_)
      attributes_.push (attributes {has, l, {}});

    if (!has)
      return make_pair (false, l);

    mode (lexer_mode::attributes);
    next (t, tt);

    if ((has = (tt != type::rsbrace)))
    {
      do
      {
        if (tt == type::newline || tt == type::eos)
          break;

        // Parse the attribute name with expansion (we rely on this in some
        // old and hairy tests).
        //
        const location l (get_location (t));

        names ns (
          parse_names (t, tt, pattern_mode::ignore, "attribute", nullptr));

        string n;
        value v;

        if (!pre_parse_)
        {
          // The list should contain a single, simple name.
          //
          if (ns.size () != 1 || !ns[0].simple () || ns[0].empty ())
            fail (l) << "expected attribute name instead of " << ns;

          n = move (ns[0].value);
        }

        if (tt == type::assign)
        {
          // To handle the value we switch into the attribute_value mode
          // (which doesn't treat `=` as special).
          //
          mode (lexer_mode::attribute_value, '@');
          next (t, tt);

          v = (tt != type::comma && tt != type::rsbrace
               ? parse_value (t, tt, pattern_mode::ignore, "attribute value")
               : value (names ()));

          expire_mode ();
        }

        if (!pre_parse_)
          attributes_.top ().ats.emplace_back (move (n), move (v));

        if (tt == type::comma)
          next (t, tt);
      }
      while (tt != type::rsbrace);
    }

    if (tt != type::rsbrace)
      fail (t) << "expected ']' instead of " << t;

    next (t, tt);

    if (tt == type::newline || tt == type::eos)
    {
      if (!standalone)
        fail (t) << "standalone attributes";
    }
    //
    // We require attributes to be separated from the following word or
    // "word-producing" tokens (`$` for variable expansions/function calls,
    // `(` for eval contexts, and `{` for name generation) to reduce the
    // possibility of confusing them with wildcard patterns. Consider:
    //
    // ./: [abc]-foo.txt
    //
    else if (!t.separated && (tt == type::word   ||
                              tt == type::dollar ||
                              tt == type::lparen ||
                              tt == type::lcbrace))
      fail (t) << "whitespace required after attributes" <<
        info << "use the '\\[' escape sequence if this is a wildcard pattern";

    return make_pair (has, l);
  }

  // Splice names from the name view into the destination name list while
  // doing sensible things with pairs, types, etc. Return the number of
  // the names added.
  //
  // If nv points to nv_storage then the names can be moved.
  //
  size_t parser::
  splice_names (const location& loc,
                const names_view& nv,
                names&& nv_storage,
                names& ns,
                const char* what,
                size_t pairn,
                const optional<project_name>& pp,
                const dir_path* dp,
                const string* tp)
  {
    // We could be asked to splice 0 elements (see the name pattern
    // expansion). In this case may need to pop the first half of the
    // pair.
    //
    if (nv.size () == 0)
    {
      if (pairn != 0)
        ns.pop_back ();

      return 0;
    }

    size_t start (ns.size ());

    // Move if nv points to nv_storage,
    //
    bool m (nv.data () == nv_storage.data ());

    for (const name& cn: nv)
    {
      name* n (m ? const_cast<name*> (&cn) : nullptr);

      // Project.
      //
      optional<project_name> p;
      if (cn.proj)
      {
        if (pp)
          fail (loc) << "nested project name " << *cn.proj << " in " << what;

        p = m ? move (n->proj) : cn.proj;
      }
      else if (pp)
        p = pp;

      // Directory.
      //
      dir_path d;
      if (!cn.dir.empty ())
      {
        if (dp != nullptr)
        {
          if (cn.dir.absolute ())
            fail (loc) << "nested absolute directory " << cn.dir << " in "
                       << what;

          d = *dp / cn.dir;
        }
        else
          d = m ? move (n->dir) : cn.dir;
      }
      else if (dp != nullptr)
        d = *dp;

      // Type.
      //
      string t;
      if (!cn.type.empty ())
      {
        if (tp != nullptr)
          fail (loc) << "nested type name " << cn.type << " in " << what;

        t = m ? move (n->type) : cn.type;
      }
      else if (tp != nullptr)
        t = *tp;

      // Value.
      //
      string v (m ? move (n->value) : cn.value);

      // If we are a second half of a pair.
      //
      if (pairn != 0)
      {
        // Check that there are no nested pairs.
        //
        if (cn.pair)
          fail (loc) << "nested pair in " << what;

        // And add another first half unless this is the first instance.
        //
        if (pairn != ns.size ())
          ns.push_back (ns[pairn - 1]);
      }

      ns.emplace_back (move (p), move (d), move (t), move (v));
      ns.back ().pair = cn.pair;
    }

    return ns.size () - start;
  }

  // Expand a name pattern. Note that the result can be empty (as in "no
  // elements").
  //
  size_t parser::
  expand_name_pattern (const location& l,
                       names&& pat,
                       names& ns,
                       const char* what,
                       size_t pairn,
                       const dir_path* dp,
                       const string* tp,
                       const target_type* tt)
  {
    assert (!pat.empty () && (tp == nullptr || tt != nullptr));

    // We are going to accumulate the result in a vector which can result in
    // quite a few linear searches. However, thanks to a few optimizations,
    // this shouldn't be an issue for the common cases (e.g., a pattern plus
    // a few exclusions).
    //
    names r;
    bool dir (false);

    // Figure out the start directory.
    //
    const dir_path* sp;
    dir_path s;
    if (dp != nullptr)
    {
      if (dp->absolute ())
        sp = dp;
      else
      {
        s = *pbase_ / *dp;
        sp = &s;
      }
    }
    else
      sp = pbase_;

    // Compare string to name as paths and according to dir.
    //
    auto equal = [&dir] (const string& v, const name& n) -> bool
    {
      // Use path comparison (which may be slash/case-insensitive).
      //
      return path::traits_type::compare (
        v, dir ? n.dir.representation () : n.value) == 0;
    };

    // Compare name to pattern as paths and according to dir.
    //
    auto match = [&dir, sp] (const name& n, const path& pattern) -> bool
    {
      const path& p (dir ? path_cast<path> (n.dir) : path (n.value));
      return path_match (p, pattern, *sp);
    };

    // Append name/extension to result according to dir. Store an indication
    // of whether it was amended as well as whether the extension is present
    // in the pair flag. The extension itself is stored in name::type.
    //
    auto append = [&r, &dir] (string&& v, optional<string>&& e, bool a)
    {
      name n (dir ? name (dir_path (move (v))) : name (move (v)));

      if (a)
        n.pair |= 0x01;

      if (e)
      {
        n.type = move (*e);
        n.pair |= 0x02;
      }

      r.push_back (move (n));
    };

    auto include_match = [&r, &equal, &append] (string&& m,
                                                optional<string>&& e,
                                                bool a)
    {
      auto i (find_if (
                r.begin (),
                r.end (),
                [&m, &equal] (const name& n) {return equal (m, n);}));

      if (i == r.end ())
        append (move (m), move (e), a);
    };

    // May throw invalid_path.
    //
    auto include_pattern =
      [&r, &append, &include_match, sp, &l, this] (string&& p,
                                                   optional<string>&& e,
                                                   bool a)
    {
      // If we don't already have any matches and our pattern doesn't contain
      // multiple recursive wildcards, then the result will be unique and we
      // can skip checking for duplicated. This should help quite a bit in the
      // common cases where we have a pattern plus maybe a few exclusions.
      //
      bool unique (r.empty () && path_pattern_recursive (path (p)) <= 1);

      function<void (string&&, optional<string>&&)> appf;
      if (unique)
        appf = [a, &append] (string&& v, optional<string>&& e)
        {
          append (move (v), move (e), a);
        };
      else
        appf = [a, &include_match] (string&& v, optional<string>&& e)
        {
          include_match (move (v), move (e), a);
        };

      auto process = [this, &e, &appf, sp] (path&& m,
                                            const string& p,
                                            bool interm)
      {
        // Ignore entries that start with a dot unless the pattern that
        // matched them also starts with a dot. Also ignore directories
        // containing the .buildignore file (ignoring the test if we don't
        // have a sufficiently setup project root).
        //
        const string& s (m.string ());
        if ((p[0] != '.' && s[path::traits_type::find_leaf (s)] == '.') ||
            (root_ != nullptr              &&
             root_->root_extra != nullptr  &&
             m.to_directory ()             &&
             exists (*sp / m / root_->root_extra->buildignore_file)))
          return !interm;

        // Note that we have to make copies of the extension since there will
        // multiple entries for each pattern.
        //
        if (!interm)
          appf (move (m).representation (), optional<string> (e));

        return true;
      };

      try
      {
        path_search (path (move (p)), process, *sp);
      }
      catch (const system_error& e)
      {
        fail (l) << "unable to scan " << *sp << ": " << e;
      }
    };

    auto exclude_match = [&r, &equal] (const string& m)
    {
      // We know there can only be one element so we use find_if() instead of
      // remove_if() for efficiency.
      //
      auto i (find_if (
                r.begin (),
                r.end (),
                [&m, &equal] (const name& n) {return equal (m, n);}));

      if (i != r.end ())
        r.erase (i);
    };

    auto exclude_pattern = [&r, &match] (const path& p)
    {
      for (auto i (r.begin ()); i != r.end (); )
      {
        if (match (*i, p))
          i = r.erase (i);
        else
          ++i;
      }
    };

    // Process the pattern and inclusions/exclusions.
    //
    for (auto b (pat.begin ()), i (b), end (pat.end ()); i != end; ++i)
    {
      name& n (*i);
      bool first (i == b);

      char s ('\0'); // Inclusion/exclusion sign (+/-).

      // Reduce inclusions/exclusions group (-/+{foo bar}) to simple name/dir.
      //
      if (n.typed () && n.type.size () == 1)
      {
        if (!first)
        {
          s = n.type[0];

          if (s == '-' || s == '+')
            n.type.clear ();
        }
        else
        {
          assert (n.type[0] == '+'); // Can only belong to inclusion group.
          n.type.clear ();
        }
      }

      if (n.empty () || !(n.simple () || n.directory ()))
        fail (l) << "invalid '" << n << "' in " << what << " pattern";

      string v (n.simple () ? move (n.value) : move (n.dir).representation ());

      // Figure out if this is inclusion or exclusion.
      //
      if (first)
        s = '+'; // Treat as inclusion.
      else if (s == '\0')
      {
        s = v[0];

        assert (s == '-' || s == '+'); // Validated at the token level.
        v.erase (0, 1);

        if (v.empty ())
          fail (l) << "empty " << what << " pattern";
      }

      // Amend the pattern or match in a target type-specific manner.
      //
      // Name splitting must be consistent with scope::find_target_type().
      // Since we don't do it for directories, we have to delegate it to the
      // target_type::pattern() call.
      //
      bool a (false);     // Amended.
      optional<string> e; // Extension.
      {
        bool d;

        if (tt != nullptr && tt->pattern != nullptr)
        {
          a = tt->pattern (*tt, *scope_, v, e, l, false);
          d = path::traits_type::is_separator (v.back ());
        }
        else
        {
          d = path::traits_type::is_separator (v.back ());

          if (!d)
            e = target::split_name (v, l);
        }

        // Based on the first pattern verify inclusions/exclusions are
        // consistently file/directory.
        //
        if (first)
          dir = d;
        else if (d != dir)
          fail (l) << "inconsistent file/directory result in " << what
                   << " pattern";
      }

      // Factor non-empty extension back into the name for searching.
      //
      // Note that doing it at this stage means we don't support extension
      // patterns.
      //
      if (e && !e->empty ())
      {
        v += '.';
        v += *e;
      }

      try
      {
        if (s == '+')
          include_pattern (move (v), move (e), a);
        else
        {
          path p (move (v));

          if (path_pattern (p))
            exclude_pattern (p);
          else
            exclude_match (move (p).representation ()); // Reuse the buffer.
        }
      }
      catch (const invalid_path& e)
      {
        fail (l) << "invalid path '" << e.path << "' in " << what
                 << " pattern";
      }
    }

    // Post-process the result: remove extension, reverse target type-specific
    // pattern/match amendments (essentially: cxx{*} -> *.cxx -> foo.cxx ->
    // cxx{foo}), and recombined the result.
    //
    for (name& n: r)
    {
      string v;
      optional<string> e;

      if (dir)
        v = move (n.dir).representation ();
      else
      {
        v = move (n.value);

        if ((n.pair & 0x02) != 0)
        {
          e = move (n.type);

          // Remove non-empty extension from the name (it got to be there, see
          // above).
          //
          if (!e->empty ())
            v.resize (v.size () - e->size () - 1);
        }
      }

      bool de (false); // Default extension.
      if ((n.pair & 0x01) != 0)
      {
        de = static_cast<bool> (e);
        tt->pattern (*tt, *scope_, v, e, l, true);
        de = de && !e;
      }

      if (dir)
        n.dir = dir_path (move (v));
      else
      {
        target::combine_name (v, e, de);
        n.value = move (v);
      }

      n.pair = '\0';
    }

    return splice_names (
      l, names_view (r), move (r), ns, what, pairn, nullopt, dp, tp);
  }

  // Parse names inside {} and handle the following "crosses" (i.e.,
  // {a b}{x y}) if any. Return the number of names added to the list.
  //
  size_t parser::
  parse_names_trailer (token& t, type& tt,
                       names& ns,
                       pattern_mode pmode,
                       const char* what,
                       const string* separators,
                       size_t pairn,
                       const optional<project_name>& pp,
                       const dir_path* dp,
                       const string* tp,
                       bool cross)
  {
    if (pp)
      pmode = pattern_mode::ignore;

    next (t, tt);                          // Get what's after '{'.
    const location loc (get_location (t)); // Start of names.

    size_t start (ns.size ());

    if (pairn == 0 && start != 0 && ns.back ().pair)
      pairn = start;

    names r;

    // Parse names until closing '}' expanding patterns.
    //
    auto parse = [&r, &t, &tt, pmode, what, separators, this] (
      const optional<project_name>& pp,
      const dir_path* dp,
      const string* tp)
    {
      const location loc (get_location (t));

      size_t start (r.size ());

      // This can be an ordinary name group or a pattern (with inclusions and
      // exclusions). We want to detect which one it is since for patterns we
      // want just the list of simple names without pair/dir/type added (those
      // are added after the pattern expansion in parse_names_pattern()).
      //
      // Detecting which one it is is tricky. We cannot just peek at the token
      // and look for some wildcards since the pattern can be the result of an
      // expansion (or, worse, concatenation). Thus pattern_mode::detect: we
      // are going to ask parse_names() to detect for us if the first name is
      // a pattern. And if it is, to refrain from adding pair/dir/type.
      //
      optional<const target_type*> pat_tt (
        parse_names (
          t, tt,
          r,
          pmode == pattern_mode::expand ? pattern_mode::detect : pmode,
          false /* chunk */,
          what,
          separators,
          0,                    // Handled by the splice_names() call below.
          pp, dp, tp,
          false /* cross */,
          true  /* curly */).pattern);

      if (tt != type::rcbrace)
        fail (t) << "expected '}' instead of " << t;

      // See if this is a pattern.
      //
      if (pat_tt)
      {
        // In the pre-parse mode the parse_names() result can never be a
        // pattern.
        //
        assert (!pre_parse_);

        // Move the pattern names our of the result.
        //
        names ps;
        if (start == 0)
          ps = move (r);
        else
          ps.insert (ps.end (),
                     make_move_iterator (r.begin () + start),
                     make_move_iterator (r.end ()));
        r.resize (start);

        expand_name_pattern (loc, move (ps), r, what, 0, dp, tp, *pat_tt);
      }
    };

    // Parse and expand the first group.
    //
    parse (pp, dp, tp);

    // Handle crosses. The overall plan is to take what's in r, cross each
    // element with the next group using the re-parse machinery, and store the
    // result back to r.
    //
    while (cross && peek () == type::lcbrace && !peeked ().separated)
    {
      next (t, tt); // Get '{'.

      names ln (move (r));
      r.clear ();

      // Cross with empty LHS/RHS is empty. Handle the LHS case now by parsing
      // and discaring RHS (empty RHS is handled "naturally" below).
      //
      if (ln.size () == 0)
      {
        next (t, tt); // Get what's after '{'.
        parse (nullopt, nullptr, nullptr);
        r.clear ();
        continue;
      }

      // In the pre-parse mode we fall back to the above "cross with empty
      // LHS" case.
      //
      assert (!pre_parse_);

      //@@ This can be a nested replay (which we don't support), for example,
      //   via target-specific var assignment. Add support for nested (2-level
      //   replay)? Why not use replay_guard for storage? Alternatively, don't
      //   use it here (see parse_for() for an alternative approach).
      //
      replay_guard rg (*this, ln.size () > 1);
      for (auto i (ln.begin ()), e (ln.end ()); i != e; )
      {
        next (t, tt); // Get what's after '{'.
        const location loc (get_location (t));

        name& l (*i);

        // "Promote" the lhs value to type.
        //
        if (!l.value.empty ())
        {
          if (!l.type.empty ())
            fail (loc) << "nested type name " << l.value;

          l.type.swap (l.value);
        }

        parse (l.proj,
               l.dir.empty () ? nullptr : &l.dir,
               l.type.empty () ? nullptr : &l.type);

        if (++i != e)
          rg.play (); // Replay.
      }
    }

    // We don't modify the resulting names during pre-parsing and so can bail
    // out now.
    //
    if (pre_parse_)
      return 0;

    // Splice the names into the result. Note that we have already handled
    // project/dir/type qualification but may still have a pair. Fast-path
    // common cases.
    //
    if (pairn == 0)
    {
      if (start == 0)
        ns = move (r);
      else
        ns.insert (ns.end (),
                   make_move_iterator (r.begin ()),
                   make_move_iterator (r.end ()));
    }
    else
      splice_names (loc,
                    names_view (r), move (r),
                    ns, what,
                    pairn,
                    nullopt, nullptr, nullptr);

    return ns.size () - start;
  }

  bool parser::
  start_names (type& tt, bool lp)
  {
    return (tt == type::word           ||
            tt == type::lcbrace        ||  // Untyped name group: '{foo ...'.
            tt == type::dollar         ||  // Variable expansion: '$foo ...'.
            (tt == type::lparen && lp) ||  // Eval context: '(foo) ...'.
            tt == type::pair_separator);   // Empty pair LHS: '@foo ...'.
  }

  // Slashe(s) plus '%'. Note that here we assume '/' is there since that's
  // in our buildfile "syntax".
  //
  const string parser::name_separators (
    string (path::traits_type::directory_separators) + '%');

  auto parser::
  parse_names (token& t, type& tt,
               names& ns,
               pattern_mode pmode,
               bool chunk,
               const char* what,
               const string* separators,
               size_t pairn,
               const optional<project_name>& pp,
               const dir_path* dp,
               const string* tp,
               bool cross,
               bool curly) -> parse_names_result
  {
    // Note that support for pre-parsing is partial, it does not handle
    // groups ({}).
    //
    // If pairn is not 0, then it is an index + 1 of the first half of the
    // pair for which we are parsing the second halves, for example:
    //
    // a@{b c d{e f} {}}

    tracer trace ("parser::parse_names", &path_);

    if (pp)
      pmode = pattern_mode::ignore;

    // Returned value NULL/type and pattern (see below).
    //
    bool vnull (false);
    const value_type* vtype (nullptr);
    optional<const target_type*> rpat;

    // Buffer that is used to collect the complete name in case of an
    // unseparated variable expansion or eval context, e.g., foo$bar($baz)fox.
    // The idea is to concatenate all the individual parts in this buffer and
    // then re-inject it into the loop as a single token.
    //
    // If the concatenation is untyped (see below), then the name should be
    // simple (i.e., just a string).
    //
    bool concat (false);
    bool concat_quoted (false);
    name concat_data;

    auto concat_typed = [&vnull, &vtype, &concat, &concat_data, this]
      (value&& rhs, const location& loc)
    {
      // If we have no LHS yet, then simply copy value/type.
      //
      if (concat)
      {
        small_vector<value, 2> a;

        // Convert LHS to value.
        //
        a.push_back (value (vtype)); // Potentially typed NULL value.

        if (!vnull)
          a.back ().assign (move (concat_data), nullptr);

        // RHS.
        //
        a.push_back (move (rhs));

        const char* l ((a[0].type != nullptr ? a[0].type->name : "<untyped>"));
        const char* r ((a[1].type != nullptr ? a[1].type->name : "<untyped>"));

        pair<value, bool> p;
        {
          // Print the location information in case the function fails.
          //
          auto df = make_diag_frame (
            [&loc, l, r] (const diag_record& dr)
            {
              dr << info (loc) << "while concatenating " << l << " to " << r;
              dr << info << "use quoting to force untyped concatenation";
            });

          p = ctx.functions.try_call (
            scope_, "builtin.concat", vector_view<value> (a), loc);
        }

        if (!p.second)
          fail (loc) << "no typed concatenation of " << l << " to " << r <<
            info << "use quoting to force untyped concatenation";

        rhs = move (p.first);

        // It seems natural to expect that a typed concatenation result
        // is also typed.
        //
        assert (rhs.type != nullptr);
      }

      vnull = rhs.null;
      vtype = rhs.type;

      if (!vnull)
      {
        if (vtype != nullptr)
          untypify (rhs);

        names& d (rhs.as<names> ());

        // If the value is empty, then untypify() will (typically; no pun
        // intended) represent it as an empty sequence of names rather than
        // a sequence of one empty name. This is usually what we need (see
        // simple_reverse() for details) but not in this case.
        //
        if (!d.empty ())
        {
          assert (d.size () == 1); // Must be a single value.
          concat_data = move (d[0]);
        }
      }
    };

    // Set the result pattern target type and switch to the ignore mode.
    //
    // The goal of the detect mode is to assemble the "raw" list (the pattern
    // itself plus inclusions/exclusions) that will then be passed to
    // parse_names_pattern(). So clear pair, directory, and type (they will be
    // added during pattern expansion) and change the mode to ignore (to
    // prevent any expansions in inclusions/exclusions).
    //
    auto pattern_detected =
      [&pairn, &dp, &tp, &rpat, &pmode] (const target_type* ttp)
    {
      assert (pmode == pattern_mode::detect);

      pairn = 0;
      dp = nullptr;
      tp = nullptr;
      pmode = pattern_mode::ignore;
      rpat = ttp;
    };

    // Return '+' or '-' if a token can start an inclusion or exclusion
    // (pattern or group), '\0' otherwise. The result can be used as bool.
    //
    // @@ Note that we only need to make sure that the leading '+' or '-'
    //    characters are unquoted. We could consider some partially quoted
    //    tokens as starting inclusion or exclusion as well, for example
    //    +'foo*'. However, currently we can not determine which part of a
    //    token is quoted, and so can't distinguish the above token from
    //    '+'foo*. This is why we end up with a criteria that is stricter than
    //    is really required.
    //
    auto pattern_prefix = [] (const token& t) -> char
    {
      char c;
      return t.type == type::word && ((c = t.value[0]) == '+' || c == '-') &&
             t.qtype == quote_type::unquoted
             ? c
             : '\0';
    };

    // A name sequence potentially starts with a pattern if it starts with a
    // literal unquoted plus character.
    //
    bool ppat (pmode == pattern_mode::detect && pattern_prefix (t) == '+');

    // Potential pattern inclusion group. To be recognized as such it should
    // start with the literal unquoted '+{' string and expand into a non-empty
    // name sequence.
    //
    // The first name in such a group is a pattern, regardless of whether it
    // contains wildcard characters or not. The trailing names are inclusions.
    // For example the following pattern groups are equivalent:
    //
    // cxx{+{f* *oo}}
    // cxx{f* +*oo}
    //
    bool pinc (ppat && t.value == "+" &&
               peek () == type::lcbrace && !peeked ().separated);

    // Number of names in the last group. This is used to detect when
    // we need to add an empty first pair element (e.g., @y) or when
    // we have a (for now unsupported) multi-name LHS (e.g., {x y}@z).
    //
    size_t count (0);
    size_t start (ns.size ());

    for (bool first (true);; first = false)
    {
      // Note that here we assume that, except for the first iterartion,
      // tt contains the type of the peeked token.

      // Automatically reset the detect pattern mode to expand after the
      // first element.
      //
      if (pmode == pattern_mode::detect && start != ns.size ())
        pmode = pattern_mode::expand;

      // Return true if the next token (which should be peeked at) won't be
      // part of the name.
      //
      auto last_token = [chunk, this] ()
      {
        const token& t (peeked ());
        type tt (t.type);

        return ((chunk && t.separated) || !start_names (tt));
      };

      // Return true if the next token (which should be peeked at) won't be
      // part of this concatenation. The et argument can be used to recognize
      // an extra (unseparated) token type as being concatenated.
      //
      auto last_concat = [this] (type et = type::eos)
      {
        const token& t (peeked ());
        type tt (t.type);

        return (t.separated         ||
                (tt != type::word   &&
                 tt != type::dollar &&
                 tt != type::lparen &&
                 (et == type::eos ? true : tt != et)));
      };

      // If we have accumulated some concatenations, then we have two options:
      // continue accumulating or inject. We inject if the next token is not a
      // word, var expansion, or eval context or if it is separated.
      //
      if (concat && last_concat ())
      {
        // Concatenation does not affect the tokens we get, only what we do
        // with them. As a result, we never set the concat flag during pre-
        // parsing.
        //
        assert (!pre_parse_);

        bool quoted (concat_quoted);

        concat = false;
        concat_quoted = false;

        // If this is a result of typed concatenation, then don't inject. For
        // one we don't want any of the "interpretations" performed in the
        // word parsing code below.
        //
        // And if this is the only name, then we also want to preserve the
        // type in the result.
        //
        // There is one exception, however: if the type is path, dir_path, or
        // string and what follows is an unseparated '{', then we need to
        // untypify it and inject in order to support our directory/target-
        // type syntax (this means that a target type must be a valid path
        // component). For example:
        //
        //   $out_root/foo/lib{bar}
        //   $out_root/$libtype{bar}
        //
        // And here is another exception: if we have a project, directory, or
        // type, then this is a name and we should also untypify it (let's for
        // now do it for the same set of types as the first exception). For
        // example:
        //
        //   dir/{$str}
        //   file{$str}
        //
        vnull = false; // A concatenation cannot produce NULL.

        if (vtype != nullptr)
        {
          bool e1 (tt == type::lcbrace && !peeked ().separated);
          bool e2 (pp || dp != nullptr || tp != nullptr);

          if (e1 || e2)
          {
            if (vtype == &value_traits<path>::value_type ||
                vtype == &value_traits<string>::value_type)
              ; // Representation is already in concat_data.value.
            else if (vtype == &value_traits<dir_path>::value_type)
              concat_data.value = move (concat_data.dir).representation ();
            else
            {
              diag_record dr (fail (t));

              if      (e1) dr << "expected directory and/or target type";
              else if (e2) dr << "expected name";

              dr << " instead of " << vtype->name << endf;
            }

            vtype = nullptr;
            // Fall through to injection.
          }
          else
          {
            ns.push_back (move (concat_data));

            // Clear the type information if that's not the only name.
            //
            if (start != ns.size () || !last_token ())
              vtype = nullptr;

            // Restart the loop (but now with concat mode off) to handle
            // chunking, etc.
            //
            continue;
          }
        }

        // Replace the current token with our injection (after handling it we
        // will peek at the current token again).
        //
        // We don't know what exactly was quoted so approximating as partially
        // mixed quoted.
        //
        tt = type::word;
        t = token (move (concat_data.value),
                   true,
                   quoted ? quote_type::mixed : quote_type::unquoted,
                   false,
                   t.line, t.column);
      }
      else if (!first)
      {
        // If we are chunking, stop at the next separated token.
        //
        next (t, tt);

        if (chunk && t.separated)
          break;

        // If we are parsing the pattern group, then space-separated tokens
        // must start inclusions or exclusions (see above).
        //
        if (rpat && t.separated && tt != type::rcbrace && !pattern_prefix (t))
          fail (t) << "expected name pattern inclusion or exclusion";
      }

      // Name.
      //
      // A user may specify a value that is an invalid name (e.g., it contains
      // '%' but the project name is invalid). While it may seem natural to
      // expect quoting/escaping to be the answer, we may need to quote names
      // (e.g., spaces in paths) and so in our model quoted values are still
      // treated as names and we rely on reversibility if we need to treat
      // them as values. The reasonable solution to the invalid name problem is
      // then to treat them as values if they are quoted.
      //
      if (tt == type::word)
      {
        tt = peek ();

        // Skip it in the pre-parse mode (any {...} that may follow will be
        // handled as an untyped group below).
        //
        if (pre_parse_)
          continue;

        string val (move (t.value));
        const location loc (get_location (t));
        bool quoted (t.qtype != quote_type::unquoted);

        // Should we accumulate? If the buffer is not empty, then we continue
        // accumulating (the case where we are separated should have been
        // handled by the injection code above). If the next token is a var
        // expansion or eval context and it is not separated, then we need to
        // start accumulating.
        //
        if (concat        || // Continue.
            !last_concat ()) // Start.
        {
          // If LHS is typed then do typed concatenation.
          //
          if (concat && vtype != nullptr)
          {
            // Create untyped RHS.
            //
            names ns;
            ns.push_back (name (move (val)));
            concat_typed (value (move (ns)), get_location (t));
          }
          else
          {
            auto& v (concat_data.value);

            if (v.empty ())
              v = move (val);
            else
              v += val;
          }

          concat = true;
          concat_quoted = quoted || concat_quoted;

          continue;
        }

        // Find a separator (slash or %).
        //
        string::size_type p (separators != nullptr
                             ? val.find_last_of (*separators)
                             : string::npos);

        // First take care of project. A project-qualified name is not very
        // common, so we can afford some copying for the sake of simplicity.
        //
        optional<project_name> p1;
        const optional<project_name>* pp1 (&pp);

        if (p != string::npos)
        {
          bool last (val[p] == '%');
          string::size_type q (last ? p : val.rfind ('%', p - 1));

          for (; q != string::npos; ) // Breakout loop.
          {
            // Process the project name.
            //
            string proj (val, 0, q);

            try
            {
              p1 = !proj.empty ()
                ? project_name (move (proj))
                : project_name ();
            }
            catch (const invalid_argument& e)
            {
              if (quoted) // See above.
                break;

              fail (loc) << "invalid project name '" << proj << "': " << e;
            }

            if (pp)
              fail (loc) << "nested project name " << *p1;

            pp1 = &p1;

            // Now fix the rest of the name.
            //
            val.erase (0, q + 1);
            p = last ? string::npos : p - (q + 1);

            break;
          }
        }

        string::size_type n (p != string::npos ? val.size () - 1 : 0);

        // See if this is a type name, directory prefix, or both. That
        // is, it is followed by an un-separated '{'.
        //
        if (tt == type::lcbrace && !peeked ().separated)
        {
          next (t, tt);

          // Resolve the target, if there is one, for the potential pattern
          // inclusion group. If we fail, then this is not an inclusion group.
          //
          const target_type* ttp (nullptr);

          if (pinc)
          {
            assert (val == "+");

            if (tp != nullptr && scope_ != nullptr)
            {
              ttp = scope_->find_target_type (*tp);

              if (ttp == nullptr)
                ppat = pinc = false;
            }
          }

          if (p != n && tp != nullptr && !pinc)
            fail (loc) << "nested type name " << val;

          dir_path d1;
          const dir_path* dp1 (dp);

          string t1;
          const string* tp1 (tp);

          try
          {
            if (p == string::npos) // type
              tp1 = &val;
            else if (p == n) // directory
            {
              if (dp == nullptr)
                d1 = dir_path (val);
              else
                d1 = *dp / dir_path (val);

              dp1 = &d1;
            }
            else // both
            {
              t1.assign (val, p + 1, n - p);

              if (dp == nullptr)
                d1 = dir_path (val, 0, p + 1);
              else
                d1 = *dp / dir_path (val, 0, p + 1);

              dp1 = &d1;
              tp1 = &t1;
            }
          }
          catch (const invalid_path& e)
          {
            fail (loc) << "invalid path '" << e.path << "'";
          }

          count = parse_names_trailer (
            t, tt, ns, pmode, what, separators, pairn, *pp1, dp1, tp1, cross);

          // If empty group or empty name, then this is not a pattern inclusion
          // group (see above).
          //
          if (pinc)
          {
            if (count != 0 && (count > 1 || !ns.back ().empty ()))
              pattern_detected (ttp);

            ppat = pinc = false;
          }

          tt = peek ();

          continue;
        }

        // See if this is a wildcard pattern.
        //
        // It should either contain a wildcard character or, in a curly
        // context, start with unquoted '+'.
        //
        // Note that in the general case we need to convert it to a path prior
        // to testing for being a pattern (think of b[a/r] that is not a
        // pattern). If the conversion fails then this is not a path pattern.
        //
        auto pattern = [&val, &loc, this] ()
        {
          // Let's optimize it a bit for the common cases.
          //
          if (val.find_first_of ("*?[") == string::npos)
            return false;

          if (path::traits_type::find_separator (val) == string::npos)
            return path_pattern (val);

          try
          {
            return path_pattern (path (val));
          }
          catch (const invalid_path& e)
          {
            fail (loc) << "invalid path '" << e.path << "'" << endf;
          }
        };

        if (pmode != pattern_mode::ignore   &&
            !*pp1                           && // Cannot be project-qualified.
            !quoted                         && // Cannot be quoted.
            ((dp != nullptr && dp->absolute ()) || pbase_ != nullptr) &&
            (pattern () || (curly && val[0] == '+')))
        {
          // Resolve the target if there is one. If we fail, then this is not
          // a pattern.
          //
          const target_type* ttp (tp != nullptr && scope_ != nullptr
                                  ? scope_->find_target_type (*tp)
                                  : nullptr);

          if (tp == nullptr || ttp != nullptr)
          {
            if (pmode == pattern_mode::detect)
            {
              // Strip the literal unquoted plus character for the first
              // pattern in the group.
              //
              if (ppat)
              {
                assert (val[0] == '+');

                val.erase (0, 1);
                ppat = pinc = false;
              }

              // Reset the detect pattern mode to expand if the pattern is not
              // followed by the inclusion/exclusion pattern/match. Note that
              // if it is '}' (i.e., the end of the group), then it is a single
              // pattern and the expansion is what we want.
              //
              if (!pattern_prefix (peeked ()))
                pmode = pattern_mode::expand;
            }

            if (pmode == pattern_mode::expand)
            {
              count = expand_name_pattern (get_location (t),
                                           names {name (move (val))},
                                           ns,
                                           what,
                                           pairn,
                                           dp, tp, ttp);
              continue;
            }

            pattern_detected (ttp);

            // Fall through.
          }
        }

        // If we are a second half of a pair, add another first half
        // unless this is the first instance.
        //
        if (pairn != 0 && pairn != ns.size ())
          ns.push_back (ns[pairn - 1]);

        count = 1;

        // If it ends with a directory separator, then it is a directory.
        // Note that at this stage we don't treat '.' and '..' as special
        // (unless they are specified with a directory separator) because
        // then we would have ended up treating '.: ...' as a directory
        // scope. Instead, this is handled higher up the processing chain,
        // in scope::find_target_type(). This would also mess up
        // reversibility to simple name.
        //
        if (p == n)
        {
          // For reversibility to simple name, only treat it as a directory
          // if the string is an exact representation.
          //
          dir_path dir (move (val), dir_path::exact);

          if (!dir.empty ())
          {
            if (dp != nullptr)
              dir = *dp / dir;

            ns.emplace_back (*pp1,
                             move (dir),
                             (tp != nullptr ? *tp : string ()),
                             string ());
            continue;
          }
        }

        ns.emplace_back (*pp1,
                         (dp != nullptr ? *dp : dir_path ()),
                         (tp != nullptr ? *tp : string ()),
                         move (val));
        continue;
      }

      // Variable expansion, function call, or eval context.
      //
      if (tt == type::dollar || tt == type::lparen)
      {
        // These cases are pretty similar in that in both we quickly end up
        // with a list of names that we need to splice into the result.
        //
        location loc;
        value result_data;
        const value* result (&result_data);
        const char* what; // Variable, function, or evaluation context.
        bool quoted (t.qtype != quote_type::unquoted);

        if (tt == type::dollar)
        {
          // Switch to the variable name mode. We want to use this mode for
          // $foo but not for $(foo). Since we don't know whether the next
          // token is a paren or a word, we turn it on and switch to the eval
          // mode if what we get next is a paren.
          //
          mode (lexer_mode::variable);
          next (t, tt);
          loc = get_location (t);

          name qual;
          string name;

          if (t.separated)
            ; // Leave the name empty to fail below.
          else if (tt == type::word)
          {
            if (!pre_parse_)
              name = move (t.value);
          }
          else if (tt == type::lparen)
          {
            expire_mode ();
            values vs (parse_eval (t, tt, pmode)); //@@ OUT will parse @-pair and do well?

            if (!pre_parse_)
            {
              if (vs.size () != 1)
                fail (loc) << "expected single variable/function name";

              value& v (vs[0]);

              if (!v)
                fail (loc) << "null variable/function name";

              names storage;
              vector_view<build2::name> ns (reverse (v, storage)); // Movable.
              size_t n (ns.size ());

              // We cannot handle scope-qualification in the eval context as
              // we do for target-qualification (see eval-qual) since then we
              // would be treating all paths as qualified variables. So we
              // have to do it here.
              //
              if      (n == 2 && ns[0].pair == ':')   // $(foo: x)
              {
                qual = move (ns[0]);

                if (qual.empty ())
                  fail (loc) << "empty variable/function qualification";
              }
              else if (n == 2 && ns[0].directory ())  // $(foo/ x)
              {
                qual = move (ns[0]);
                qual.pair = '/';
              }
              else if (n > 1)
                fail (loc) << "expected variable/function name instead of '"
                           << ns << "'";

              // Note: checked for empty below.
              //
              if (!ns[n - 1].simple ())
                fail (loc) << "expected variable/function name instead of '"
                           << ns[n - 1] << "'";

              name = move (ns[n - 1].value);
            }
          }
          else
            fail (t) << "expected variable/function name instead of " << t;

          if (!pre_parse_ && name.empty ())
            fail (loc) << "empty variable/function name";

          // Figure out whether this is a variable expansion or a function
          // call.
          //
          tt = peek ();

          // Note that we require function call opening paren to be
          // unseparated; consider: $x ($x == 'foo' ? 'FOO' : 'BAR').
          //
          if (tt == type::lparen && !peeked ().separated)
          {
            // Function call.
            //

            next (t, tt); // Get '('.

            // @@ Should we use (target/scope) qualification (of name) as the
            // context in which to call the function? Hm, interesting...
            //
            values args (parse_eval (t, tt, pmode));
            tt = peek ();

            if (pre_parse_)
              continue; // As if empty result.

            // Note that we "move" args to call().
            //
            result_data = ctx.functions.call (scope_, name, args, loc);
            what = "function call";
          }
          else
          {
            // Variable expansion.
            //

            if (pre_parse_)
              continue; // As if empty value.

            lookup l (lookup_variable (move (qual), move (name), loc));

            if (l.defined ())
              result = l.value; // Otherwise leave as NULL result_data.

            what = "variable expansion";
          }
        }
        else
        {
          // Context evaluation.
          //

          loc = get_location (t);
          values vs (parse_eval (t, tt, pmode));
          tt = peek ();

          if (pre_parse_)
            continue; // As if empty result.

          switch (vs.size ())
          {
          case 0:  result_data = value (names ()); break;
          case 1:  result_data = move (vs[0]); break;
          default: fail (loc) << "expected single value";
          }

          what = "context evaluation";
        }

        // We never end up here during pre-parsing.
        //
        assert (!pre_parse_);

        // Should we accumulate? If the buffer is not empty, then we continue
        // accumulating (the case where we are separated should have been
        // handled by the injection code above). If the next token is a word
        // or an expansion and it is not separated, then we need to start
        // accumulating. We also reduce the $var{...} case to concatention
        // and injection.
        //
        if (concat                     || // Continue.
            !last_concat (type::lcbrace)) // Start.
        {
          // This can be a typed or untyped concatenation. The rules that
          // determine which one it is are as follows:
          //
          // 1. Determine if to preserver the type of RHS: if its first
          //    token is quoted, then we do not.
          //
          // 2. Given LHS (if any) and RHS we do typed concatenation if
          //    either is typed.
          //
          // Here are some interesting corner cases to meditate on:
          //
          // $dir/"foo bar"
          // $dir"/foo bar"
          // "foo"$dir
          // "foo""$dir"
          // ""$dir
          //

          // First if RHS is typed but quoted then convert it to an untyped
          // string.
          //
          // Conversion to an untyped string happens differently, depending
          // on whether we are in a quoted or unquoted context. In an
          // unquoted context we use $representation() which must return a
          // "round-trippable representation" (and if that it not possible,
          // then it should not be overloaded for a type). In a quoted
          // context we use $string() which returns a "canonical
          // representation" (e.g., a directory path without a trailing
          // slash).
          //
          if (result->type != nullptr && quoted)
          {
            // RHS is already a value but it could be a const reference (to
            // the variable value) while we need to move things around. So in
            // this case we make a copy.
            //
            if (result != &result_data)
              result = &(result_data = *result);

            const char* t (result_data.type->name);

            pair<value, bool> p;
            {
              // Print the location information in case the function fails.
              //
              auto df = make_diag_frame (
                [&loc, t] (const diag_record& dr)
                {
                  dr << info (loc) << "while converting " << t << " to string";
                });

              p = ctx.functions.try_call (
                scope_, "string", vector_view<value> (&result_data, 1), loc);
            }

            if (!p.second)
              fail (loc) << "no string conversion for " << t;

            result_data = move (p.first);
            untypify (result_data); // Convert to untyped simple name.
          }

          if ((concat && vtype != nullptr) || // LHS typed.
              (result->type != nullptr))      // RHS typed.
          {
            if (result != &result_data) // Same reason as above.
              result = &(result_data = *result);

            concat_typed (move (result_data), loc);
          }
          //
          // Untyped concatenation. Note that if RHS is NULL/empty, we still
          // set the concat flag.
          //
          else if (!result->null && !result->empty ())
          {
            // This can only an untyped value.
            //
            // @@ Could move if result == &result_data.
            //
            const names& lv (cast<names> (*result));

            // This should be a simple value or a simple directory.
            //
            if (lv.size () > 1)
              fail (loc) << "concatenating " << what << " contains multiple "
                         << "values";

            const name& n (lv[0]);

            if (n.qualified ())
              fail (loc) << "concatenating " << what << " contains project "
                         << "name";

            if (n.typed ())
              fail (loc) << "concatenating " << what << " contains type";

            if (!n.dir.empty ())
            {
              if (!n.value.empty ())
                fail (loc) << "concatenating " << what << " contains "
                           << "directory";

              // Note that here we cannot assume what's in dir is really a
              // path (think s/foo/bar/) so we have to reverse it exactly.
              //
              concat_data.value += n.dir.representation ();
            }
            else
              concat_data.value += n.value;
          }

          concat = true;
          concat_quoted = quoted || concat_quoted;
        }
        else
        {
          // See if we should propagate the value NULL/type. We only do this
          // if this is the only expansion, that is, it is the first and the
          // next token is not part of the name.
          //
          if (first && last_token ())
          {
            vnull = result->null;
            vtype = result->type;
          }

          // Nothing else to do here if the result is NULL or empty.
          //
          if (result->null || result->empty ())
            continue;

          // @@ Could move if nv is result_data; see untypify().
          //
          names nv_storage;
          names_view nv (reverse (*result, nv_storage));

          count = splice_names (
            loc, nv, move (nv_storage), ns, what, pairn, pp, dp, tp);
        }

        continue;
      }

      // Untyped name group without a directory prefix, e.g., '{foo bar}'.
      //
      if (tt == type::lcbrace)
      {
        count = parse_names_trailer (
          t, tt, ns, pmode, what, separators, pairn, pp, dp, tp, cross);
        tt = peek ();
        continue;
      }

      // A pair separator.
      //
      if (tt == type::pair_separator)
      {
        if (pairn != 0)
          fail (t) << "nested pair on the right hand side of a pair";

        tt = peek ();

        if (!pre_parse_)
        {
          // Catch double pair separator ('@@'). Maybe we can use for
          // something later (e.g., escaping).
          //
          if (!ns.empty () && ns.back ().pair)
            fail (t) << "double pair separator";

          if (t.separated || count == 0)
          {
            // Empty LHS, (e.g., @y), create an empty name. The second test
            // will be in effect if we have something like v=@y.
            //
            ns.emplace_back (pp,
                             (dp != nullptr ? *dp : dir_path ()),
                             (tp != nullptr ? *tp : string ()),
                             string ());
            count = 1;
          }
          else if (count > 1)
            fail (t) << "multiple " << what << "s on the left hand side "
                     << "of a pair";

          ns.back ().pair = t.value[0];

          // If the next token is separated, then we have an empty RHS. Note
          // that the case where it is not a name/group (e.g., a newline/eos)
          // is handled below, once we are out of the loop.
          //
          if (peeked ().separated)
          {
            ns.emplace_back (pp,
                             (dp != nullptr ? *dp : dir_path ()),
                             (tp != nullptr ? *tp : string ()),
                             string ());
            count = 0;
          }
        }

        continue;
      }

      // Note: remember to update last_token() test if adding new recognized
      // tokens.

      if (!first)
        break;

      if (tt == type::rcbrace) // Empty name, e.g., dir{}.
      {
        // If we are a second half of a pair, add another first half
        // unless this is the first instance.
        //
        if (pairn != 0 && pairn != ns.size ())
          ns.push_back (ns[pairn - 1]);

        ns.emplace_back (pp,
                         (dp != nullptr ? *dp : dir_path ()),
                         (tp != nullptr ? *tp : string ()),
                         string ());
        break;
      }
      else
        // Our caller expected this to be something.
        //
        fail (t) << "expected " << what << " instead of " << t;
    }

    // Handle the empty RHS in a pair, (e.g., y@).
    //
    if (!ns.empty () && ns.back ().pair)
    {
      ns.emplace_back (pp,
                       (dp != nullptr ? *dp : dir_path ()),
                       (tp != nullptr ? *tp : string ()),
                       string ());
    }

    if (pre_parse_)
      assert (!vnull && vtype == nullptr && !rpat);

    return parse_names_result {!vnull, vtype, rpat};
  }

  void parser::
  skip_line (token& t, type& tt)
  {
    for (; tt != type::newline && tt != type::eos; next (t, tt)) ;
  }

  void parser::
  skip_block (token& t, type& tt)
  {
    // Skip until } or eos, keeping track of the {}-balance.
    //
    for (size_t b (0); tt != type::eos; )
    {
      if (tt == type::lcbrace || tt == type::rcbrace)
      {
        type ptt (peek ());
        if (ptt == type::newline || ptt == type::eos) // Block { or }.
        {
          if (tt == type::lcbrace)
            ++b;
          else
          {
            if (b == 0)
              break;

            --b;
          }
        }
      }

      skip_line (t, tt);

      if (tt != type::eos)
        next (t, tt);
    }
  }

  bool parser::
  keyword (const token& t)
  {
    assert (replay_ == replay::stop); // Can't be used in a replay.
    assert (t.type == type::word);

    // The goal here is to allow using keywords as variable names and
    // target types without imposing ugly restrictions/decorators on
    // keywords (e.g., '.using' or 'USING'). A name is considered a
    // potential keyword if:
    //
    // - it is not quoted [so a keyword can always be escaped] and
    // - next token is '\n' (or eos) or '(' [so if(...) will work] or
    // - next token is separated and is not '=', '=+', or '+=' [which
    //   means a "directive trailer" can never start with one of them].
    //
    // See tests/keyword.
    //
    if (t.qtype == quote_type::unquoted)
    {
      // We cannot peek at the whole token here since it might have to be
      // lexed in a different mode. So peek at its first character.
      //
      pair<char, bool> p (lexer_->peek_char ());
      char c (p.first);

      // @@ Just checking for leading '+' is not sufficient, for example:
      //
      // print +foo
      //
      return c == '\n' || c == '\0' || c == '(' ||
        (p.second && c != '=' && c != '+');
    }

    return false;
  }

  // Buildspec parsing.
  //

  // Here is the problem: we "overload" '(' and ')' to mean operation
  // application rather than the eval context. At the same time we want to use
  // parse_names() to parse names, get variable expansion/function calls,
  // quoting, etc. We just need to disable the eval context. The way this is
  // done has two parts: Firstly, we parse names in chunks and detect and
  // handle the opening paren ourselves. In other words, a buildspec like
  // 'clean (./)' is "chunked" as 'clean', '(', etc. While this is fairly
  // straightforward, there is one snag: concatenating eval contexts, as in
  // 'clean(./)'.  Normally, this will be treated as a single chunk and we
  // don't want that. So here comes the trick (or hack, if you like): the
  // buildspec lexer mode makes every opening paren token "separated" (i.e.,
  // as if it was preceeded by a space). This will disable concatenating
  // eval.
  //
  // In fact, because this is only done in the buildspec mode, we can still
  // use eval contexts provided that we quote them: '"cle(an)"'. Note that
  // function calls also need quoting (since a separated '(' is not treated as
  // a function call): '"$identity(update)"'.
  //
  // This poses a problem, though: if it's quoted then it is a concatenated
  // expansion and therefore cannot contain multiple values, for example,
  // $identity(foo/ bar/). So what we do is disable this chunking/separation
  // after both meta-operation and operation were specified. So if we specify
  // both explicitly, then we can use eval context, function calls, etc.,
  // normally: perform(update($identity(foo/ bar/))).
  //
  buildspec parser::
  parse_buildspec (istream& is, const path_name& in)
  {
    path_ = &in;

    // We do "effective escaping" and only for ['"\$(] (basically what's
    // necessary inside a double-quoted literal plus the single quote).
    //
    lexer l (is, *path_, 1 /* line */, "\'\"\\$(");
    lexer_ = &l;
    scope_ = root_ = &ctx.global_scope.rw ();
    pbase_ = &work; // Use current working directory.
    target_ = nullptr;
    prerequisite_ = nullptr;

    // Turn on the buildspec mode/pairs recognition with '@' as the pair
    // separator (e.g., src_root/@out_root/exe{foo bar}).
    //
    mode (lexer_mode::buildspec, '@');

    token t;
    type tt;
    next (t, tt);

    buildspec r (tt != type::eos
                 ? parse_buildspec_clause (t, tt, 0)
                 : buildspec ());

    if (tt != type::eos)
      fail (t) << "expected operation or target instead of " << t;

    return r;
  }

  static bool
  opname (const name& n)
  {
    // First it has to be a non-empty simple name.
    //
    if (n.pair || !n.simple () || n.empty ())
      return false;

    // Like C identifier but with '-' instead of '_' as the delimiter.
    //
    for (size_t i (0); i != n.value.size (); ++i)
    {
      char c (n.value[i]);
      if (c != '-' && !(i != 0 ? alnum (c) : alpha (c)))
        return false;
    }

    return true;
  }

  buildspec parser::
  parse_buildspec_clause (token& t, type& tt, size_t depth)
  {
    buildspec bs;

    for (bool first (true);; first = false)
    {
      // We always start with one or more names. Eval context (lparen) only
      // allowed if quoted.
      //
      if (!start_names (tt, mode () == lexer_mode::double_quoted))
      {
        if (first)
          fail (t) << "expected operation or target instead of " << t;

        break;
      }

      const location l (get_location (t)); // Start of names.

      // This call will parse the next chunk of output and produce zero or
      // more names.
      //
      names ns (parse_names (t, tt, pattern_mode::expand, depth < 2));

      if (ns.empty ()) // Can happen if pattern expansion.
        fail (l) << "expected operation or target";

      // What these names mean depends on what's next. If it is an opening
      // paren, then they are operation/meta-operation names. Otherwise they
      // are targets.
      //
      if (tt == type::lparen) // Got by parse_names().
      {
        if (ns.empty ())
          fail (t) << "expected operation name before '('";

        for (const name& n: ns)
          if (!opname (n))
            fail (l) << "expected operation name instead of '" << n << "'";

        // Inside '(' and ')' we have another, nested, buildspec. Push another
        // mode to keep track of the depth (used in the lexer implementation
        // to decide when to stop separating '(').
        //
        mode (lexer_mode::buildspec, '@');

        next (t, tt); // Get what's after '('.
        const location l (get_location (t)); // Start of nested names.
        buildspec nbs (parse_buildspec_clause (t, tt, depth + 1));

        // Parse additional operation/meta-operation parameters.
        //
        values params;
        while (tt == type::comma)
        {
          next (t, tt);

          // Note that for now we don't expand patterns. If it turns out we
          // need this, then will probably have to be (meta-) operation-
          // specific (via pre-parse or some such).
          //
          params.push_back (tt != type::rparen
                            ? parse_value (t, tt, pattern_mode::ignore)
                            : value (names ()));
        }

        if (tt != type::rparen)
          fail (t) << "expected ')' instead of " << t;

        expire_mode ();
        next (t, tt); // Get what's after ')'.

        // Merge the nested buildspec into ours. But first determine if we are
        // an operation or meta-operation and do some sanity checks.
        //
        bool meta (false);
        for (const metaopspec& nms: nbs)
        {
          // We definitely shouldn't have any meta-operations.
          //
          if (!nms.name.empty ())
            fail (l) << "nested meta-operation " << nms.name;

          if (!meta)
          {
            // If we have any operations in the nested spec, then this mean
            // that our names are meta-operation names.
            //
            for (const opspec& nos: nms)
            {
              if (!nos.name.empty ())
              {
                meta = true;
                break;
              }
            }
          }
        }

        // No nested meta-operations means we should have a single
        // metaopspec object with empty meta-operation name.
        //
        assert (nbs.size () == 1);
        const metaopspec& nmo (nbs.back ());

        if (meta)
        {
          for (name& n: ns)
          {
            bs.push_back (nmo);
            bs.back ().name = move (n.value);
            bs.back ().params = params;
          }
        }
        else
        {
          // Since we are not a meta-operation, the nested buildspec should be
          // just a bunch of targets.
          //
          assert (nmo.size () == 1);
          const opspec& nos (nmo.back ());

          if (bs.empty () || !bs.back ().name.empty ())
            bs.push_back (metaopspec ()); // Empty (default) meta operation.

          for (name& n: ns)
          {
            bs.back ().push_back (nos);
            bs.back ().back ().name = move (n.value);
            bs.back ().back ().params = params;
          }
        }
      }
      else if (!ns.empty ())
      {
        // Group all the targets into a single operation. In other
        // words, 'foo bar' is equivalent to 'update(foo bar)'.
        //
        if (bs.empty () || !bs.back ().name.empty ())
          bs.push_back (metaopspec ()); // Empty (default) meta operation.

        metaopspec& ms (bs.back ());

        for (auto i (ns.begin ()), e (ns.end ()); i != e; ++i)
        {
          // @@ We may actually want to support this at some point.
          //
          if (i->qualified ())
            fail (l) << "expected target name instead of " << *i;

          if (opname (*i))
            ms.push_back (opspec (move (i->value)));
          else
          {
            // Do we have the src_base?
            //
            dir_path src_base;
            if (i->pair)
            {
              if (i->pair != '@')
                fail << "unexpected pair style in buildspec";

              if (i->typed ())
                fail (l) << "expected target src_base instead of " << *i;

              src_base = move (i->dir);

              if (!i->value.empty ())
                src_base /= dir_path (move (i->value));

              ++i;
              assert (i != e); // Got to have the second half of the pair.
            }

            if (ms.empty () || !ms.back ().name.empty ())
              ms.push_back (opspec ()); // Empty (default) operation.

            opspec& os (ms.back ());
            os.emplace_back (move (src_base), move (*i));
          }
        }
      }
    }

    return bs;
  }

  lookup parser::
  lookup_variable (name&& qual, string&& name, const location& loc)
  {
    tracer trace ("parser::lookup_variable", &path_);

    // Process variable name. @@ OLD
    //
    if (name.front () == '.') // Fully namespace-qualified name.
      name.erase (0, 1);
    else
    {
      //@@ TODO : append namespace if any.
    }

    const scope* s (nullptr);
    const target* t (nullptr);
    const prerequisite* p (nullptr);

    // If we are qualified, it can be a scope or a target.
    //
    enter_scope sg;
    enter_target tg;

    if (qual.empty ())
    {
      s = scope_;
      t = target_;
      p = prerequisite_;
    }
    else
    {
      switch (qual.pair)
      {
      case '/':
        {
          assert (qual.directory ());
          sg = enter_scope (*this, move (qual.dir));
          s = scope_;
          break;
        }
      case ':':
        {
          qual.pair = '\0';

          // @@ OUT TODO
          //
          tg = enter_target (
            *this, move (qual), build2::name (), true, loc, trace);
          t = target_;
          break;
        }
      default: assert (false);
      }
    }

    // Lookup.
    //
    const auto& var (ctx.var_pool.rw (*scope_).insert (move (name), true));

    if (p != nullptr)
    {
      // The lookup depth is a bit of a hack but should be harmless since
      // unused.
      //
      pair<lookup, size_t> r (p->vars[var], 1);

      if (!r.first.defined ())
        r = t->find_original (var);

      return var.overrides == nullptr
        ? r.first
        : t->base_scope ().find_override (var, move (r), true).first;
    }

    if (t != nullptr)
    {
      if (var.visibility > variable_visibility::target)
      {
        fail (loc) << "variable " << var << " has " << var.visibility
                   << " visibility but is expanded in target context";
      }

      return (*t)[var];
    }

    if (s != nullptr)
    {
      if (var.visibility > variable_visibility::scope)
      {
        fail (loc) << "variable " << var << " has " << var.visibility
                   << " visibility but is expanded in scope context";
      }

      return (*s)[var];
    }

    // Undefined/NULL namespace variables are not allowed.
    //
    // @@ TMP this isn't proving to be particularly useful.
    //
    // if (!l)
    // {
    //   if (var.name.find ('.') != string::npos)
    //     fail (loc) << "undefined/null namespace variable " << var;
    // }

    return lookup ();
  }

  void parser::
  switch_scope (const dir_path& d)
  {
    tracer trace ("parser::switch_scope", &path_);

    auto p (build2::switch_scope (*root_, d));
    scope_ = &p.first;
    pbase_ = scope_->src_path_ != nullptr ? scope_->src_path_ : &d;

    if (p.second != root_)
    {
      root_ = p.second;
      l5 ([&]
          {
            if (root_ != nullptr)
              trace << "switching to root scope " << *root_;
            else
              trace << "switching to out of project scope";
          });
    }
  }

  void parser::
  process_default_target (token& t)
  {
    tracer trace ("parser::process_default_target", &path_);

    // The logic is as follows: if we have an explicit current directory
    // target, then that's the default target. Otherwise, we take the
    // first target and use it as a prerequisite to create an implicit
    // current directory target, effectively making it the default
    // target via an alias. If there are no targets in this buildfile,
    // then we don't do anything.
    //
    if (default_target_ == nullptr) // No targets in this buildfile.
      return;

    target& dt (*default_target_);

    target* ct (
      const_cast<target*> (                // Ok (serial execution).
        ctx.targets.find (dir::static_type,    // Explicit current dir target.
                          scope_->out_path (),
                          dir_path (),         // Out tree target.
                          string (),
                          nullopt,
                          trace)));

    if (ct == nullptr)
    {
      l5 ([&]{trace (t) << "creating current directory alias for " << dt;});

      // While this target is not explicitly mentioned in the buildfile, we
      // say that we behave as if it were. Thus not implied.
      //
      ct = &ctx.targets.insert (dir::static_type,
                                scope_->out_path (),
                                dir_path (),
                                string (),
                                nullopt,
                                false,
                                trace).first;
      // Fall through.
    }
    else if (ct->implied)
    {
      ct->implied = false;
      // Fall through.
    }
    else
      return; // Existing and not implied.

    ct->prerequisites_state_.store (2, memory_order_relaxed);
    ct->prerequisites_.emplace_back (prerequisite (dt));
  }

  void parser::
  enter_buildfile (const path_name& pn)
  {
    tracer trace ("parser::enter_buildfile", &path_);

    const path& p (pn.path != nullptr ? *pn.path : path ());
    dir_path d (p.directory ()); // Empty for a path name with the NULL path.

    // Figure out if we need out.
    //
    dir_path out;
    if (scope_->src_path_ != nullptr &&
        scope_->src_path () != scope_->out_path () &&
        d.sub (scope_->src_path ()))
    {
      out = out_src (d, *root_);
    }

    ctx.targets.insert<buildfile> (
      move (d),
      move (out),
      pn.name ? *pn.name : p.leaf ().base ().string (),
      p.extension (),                                   // Always specified.
      trace);
  }

  type parser::
  next (token& t, type& tt)
  {
    replay_token r;

    if (peeked_)
    {
      r = move (peek_);
      peeked_ = false;
    }
    else
      r = replay_ != replay::play ? lexer_next () : replay_next ();

    if (replay_ == replay::save)
      replay_data_.push_back (r);

    t = move (r.token);
    tt = t.type;
    return tt;
  }

  inline type parser::
  next_after_newline (token& t, type& tt, char a)
  {
    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
    {
      diag_record dr (fail (t));
      dr << "expected newline instead of " << t;

      if (a != '\0')
        dr << " after '" << a << "'";
    }

    return tt;
  }

  inline type parser::
  next_after_newline (token& t, type& tt, const char* a)
  {
    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
    {
      diag_record dr (fail (t));
      dr << "expected newline instead of " << t;

      if (a != nullptr)
        dr << " after " << a;
    }

    return tt;
  }

  type parser::
  peek ()
  {
    if (!peeked_)
    {
      peek_ = (replay_ != replay::play ? lexer_next () : replay_next ());
      peeked_ = true;
    }

    return peek_.token.type;
  }
}
