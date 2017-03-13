// file      : build2/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/parser>

#include <iostream> // cout

#include <butl/filesystem> // path_search(), path_match()

#include <build2/version>

#include <build2/file>
#include <build2/scope>
#include <build2/module>
#include <build2/target>
#include <build2/context>
#include <build2/function>
#include <build2/variable>
#include <build2/diagnostics>
#include <build2/prerequisite>

using namespace std;

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

    enter_target (parser& p,
                  name&& n,  // If n.pair, then o is out dir.
                  name&& o,
                  bool implied,
                  const location& loc,
                  tracer& tr)
        : p_ (&p), t_ (p.target_)
    {
      optional<string> e;
      const target_type* ti (p.scope_->find_target_type (n, e));

      if (ti == nullptr)
        p.fail (loc) << "unknown target type " << n.type;

      bool src (n.pair); // If out-qualified, then it is from src.
      if (src)
      {
        assert (n.pair == '@');

        if (!o.directory ())
          p.fail (loc) << "directory expected after @";
      }

      dir_path& d (n.dir);

      const dir_path& sd (p.scope_->src_path ());
      const dir_path& od (p.scope_->out_path ());

      if (d.empty ())
        d = src ? sd : od; // Already dormalized.
      else
      {
        if (d.relative ())
          d = (src ? sd : od) / d;

        d.normalize ();
      }

      dir_path out;
      if (src && sd != od) // If in-source build, then out must be empty.
      {
        out = o.dir.relative () ? od / o.dir : move (o.dir);
        out.normalize ();
      }

      // Find or insert.
      //
      p.target_ = &targets.insert (*ti,
                                   move (d),
                                   move (out),
                                   move (n.value),
                                   move (e),
                                   implied,
                                   tr).first;
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

  void parser::
  parse_buildfile (istream& is, const path& p, scope& root, scope& base)
  {
    path_ = &p;

    lexer l (is, *path_);
    lexer_ = &l;
    root_ = &root;
    scope_ = &base;
    pbase_ = scope_->src_path_;
    target_ = nullptr;
    default_target_ = nullptr;

    enter_buildfile (p); // Needs scope_.

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
    target_ = nullptr;
    scope_ = &s;
    pbase_ = scope_->src_path_; // Normally NULL.

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
    target_ = nullptr;
    scope_ = &s;
    pbase_ = b;

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

    // clause() should always stop at a token that is at the beginning of
    // the line (except for eof). That is, if something is called to parse
    // a line, it should parse it until newline (or fail). This is important
    // for if-else blocks, directory scopes, etc., that assume the } token
    // they see is on the new line.
    //
    bool parsed (false);

    while (tt != type::eos && !(one && parsed))
    {
      // Extract attributes if any.
      //
      assert (attributes_.empty ());
      auto at (attributes_push (t, tt));

      // We always start with one or more names.
      //
      if (tt != type::word    &&
          tt != type::lcbrace && // Untyped name group: '{foo ...'
          tt != type::dollar  && // Variable expansion: '$foo ...'
          tt != type::lparen  && // Eval context: '(foo) ...'
          tt != type::colon)     // Empty name: ': ...'
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
        else if (n == "print")
        {
          f = &parser::parse_print;
        }
        else if (n == "source")
        {
          f = &parser::parse_source;
        }
        else if (n == "include")
        {
          f = &parser::parse_include;
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
          // Valid ones are handled in if_else().
          //
          fail (t) << n << " without if";
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

      const location nloc (get_location (t));
      names ns (parse_names (t, tt, pattern_mode::ignore));

      if (tt == type::colon)
      {
        // While '{}:' means empty name, '{$x}:' where x is empty list
        // means empty list.
        //
        if (ns.empty ())
          fail (t) << "target expected before :";

        next (t, tt);

        if (tt == type::newline)
        {
          // See if this is a directory/target scope.
          //
          if (peek () == type::lcbrace)
          {
            next (t, tt);

            // Should be on its own line.
            //
            if (next (t, tt) != type::newline)
              fail (t) << "expected newline after {";

            // See if this is a directory or target scope. Different
            // things can appear inside depending on which one it is.
            //
            bool dir (false);
            for (const auto& n: ns)
            {
              if (n.directory ())
              {
                if (ns.size () != 1)
                {
                  // @@ TODO: point to name (and above).
                  //
                  fail (nloc) << "multiple names in directory scope";
                }

                dir = true;
              }
            }

            next (t, tt);

            if (dir)
            {
              // Directory scope.
              //
              if (at.first)
                fail (at.second) << "attributes before directory scope";
              else
                attributes_pop ();

              // Can contain anything that a top level can.
              //
              enter_scope sg (*this, move (ns[0].dir));
              parse_clause (t, tt);
            }
            else
            {
              if (at.first)
                fail (at.second) << "attributes before target scope";
              else
                attributes_pop ();

              // @@ TODO: target scope.
            }

            if (tt != type::rcbrace)
              fail (t) << "expected } instead of " << t;

            // Should be on its own line.
            //
            if (next (t, tt) == type::newline)
              next (t, tt);
            else if (tt != type::eos)
              fail (t) << "expected newline after }";

            continue;
          }

          // If this is not a scope, then it is a target without any
          // prerequisites. Fall through.
          //
        }

        // Dependency declaration or scope/target-specific variable
        // assignment.
        //

        if (at.first)
          fail (at.second) << "attributes before target/scope";
        else
          attributes_pop ();

        auto at (attributes_push (t, tt));

        if (tt == type::word    ||
            tt == type::lcbrace ||
            tt == type::dollar  ||
            tt == type::lparen  ||
            tt == type::newline ||
            tt == type::eos)
        {
          // @@ PAT: currently we pattern-expand scope/target-specific vars.
          //
          const location ploc (get_location (t));
          names pns (tt != type::newline && tt != type::eos
                     ? parse_names (t, tt, pattern_mode::expand)
                     : names ());

          // Scope/target-specific variable assignment.
          //
          if (tt == type::assign || tt == type::prepend || tt == type::append)
          {
            token at (t);
            type att (tt);

            const variable& var (
              var_pool.rw (*scope_).insert (
                parse_variable_name (move (pns), ploc),
                true)); // Allow overrides.

            // Apply variable attributes.
            //
            apply_variable_attributes (var);

            // If we have multiple targets/scopes, then we save the value
            // tokens when parsing the first one and then replay them for
            // the subsequent. We have to do it this way because the value
            // may contain variable expansions that would be sensitive to
            // the target/scope context in which they are evaluated.
            //
            // Note: watch out for an out-qualified single target (two names).
            //
            replay_guard rg (
              *this, ns.size () > 2 || (ns.size () == 2 && !ns[0].pair));

            for (auto i (ns.begin ()), e (ns.end ()); i != e; )
            {
              name& n (*i);

              if (n.qualified ())
                fail (nloc) << "project name in scope/target " << n;

              if (n.directory () && !n.pair) // Not out-qualified.
              {
                // Scope variable.
                //
                if (var.visibility == variable_visibility::target)
                  fail (ploc) << "variable " << var << " has target "
                              << "visibility but assigned in a scope" <<
                    info << "consider changing to '.../*: " << var << "'";

                enter_scope sg (*this, move (n.dir));
                parse_variable (t, tt, var, att);
              }
              else
              {
                // Figure out if this is a target or type/pattern-specific
                // variable.
                //
                size_t p (n.value.find ('*'));

                if (p == string::npos)
                {
                  name o (n.pair ? move (*++i) : name ());
                  enter_target tg (
                    *this, move (n), move (o), true, nloc, trace);
                  parse_variable (t, tt, var, att);
                }
                else
                {
                  // See tests/variable/type-pattern.
                  //
                  if (n.pair)
                    fail (nloc) << "out-qualified target type/pattern-"
                                << "specific variable";

                  if (n.value.find ('*', p + 1) != string::npos)
                    fail (nloc) << "multiple wildcards in target type/pattern "
                                << n;

                  // If we have the directory, then it is the scope.
                  //
                  enter_scope sg;
                  if (!n.dir.empty ())
                    sg = enter_scope (*this, move (n.dir));

                  // Resolve target type. If none is specified or if it is
                  // '*', use the root of the hierarchy. So these are all
                  // equivalent:
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

                  // Note: expanding the value in the context of the scope.
                  //
                  value rhs (parse_variable_value (t, tt));

                  // Leave the value untyped unless we are assigning.
                  //
                  pair<reference_wrapper<value>, bool> p (
                    scope_->target_vars[*ti][move (n.value)].insert (
                      var, att == type::assign));

                  value& lhs (p.first);

                  // We store prepend/append values untyped (similar to
                  // overrides).
                  //
                  if (rhs.type != nullptr && att != type::assign)
                    untypify (rhs);

                  if (p.second)
                  {
                    // Note: we are always using assign and we don't pass the
                    // variable in case of prepend/append in order to keep the
                    // value untyped.
                    //
                    apply_value_attributes (
                      att == type::assign ? &var : nullptr,
                      lhs,
                      move (rhs),
                      type::assign);

                    // Map assignment type to value::extra constant.
                    //
                    lhs.extra =
                      att == type::prepend ? 1 :
                      att == type::append  ? 2 :
                      0;
                  }
                  else
                  {
                    // Existing value. What happens next depends what we are
                    // trying to do and what's already there.
                    //
                    // Assignment is the easy one: we simply overwrite what's
                    // already there. Also, if we are appending/prepending to
                    // a previously assigned value, then we simply append or
                    // prepend normally.
                    //
                    if (att == type::assign || lhs.extra == 0)
                    {
                      // Above we instructed insert() not to type the value so
                      // we have to compensate for that now.
                      //
                      if (att != type::assign)
                      {
                        if (var.type != nullptr && lhs.type != var.type)
                          typify (lhs, *var.type, &var);
                      }
                      else
                        lhs.extra = 0; // Change to assignment.

                      apply_value_attributes (&var, lhs, move (rhs), att);
                    }
                    else
                    {
                      // This is an append/prepent to a previously appended or
                      // prepended value. We can handle it as long as things
                      // are consistent.
                      //
                      if (att == type::prepend && lhs.extra == 2)
                        fail (at) << "prepend to a previously appended target "
                                  << "type/pattern-specific variable " << var;

                      if (att == type::append && lhs.extra == 1)
                        fail (at) << "append to a previously prepended target "
                                  << "type/pattern-specific variable " << var;

                      // Do untyped prepend/append.
                      //
                      apply_value_attributes (nullptr, lhs, move (rhs), att);
                    }
                  }

                  if (lhs.extra != 0 && lhs.type != nullptr)
                    fail (at) << "typed prepend/append to target type/pattern-"
                              << "specific variable " << var;
                }
              }

              if (++i != e)
                rg.play (); // Replay.
            }
          }
          // Dependency declaration.
          //
          else
          {
            if (at.first)
              fail (at.second) << "attributes before prerequisites";
            else
              attributes_pop ();

            // First enter all the targets (normally we will have just one).
            //
            small_vector<reference_wrapper<target>, 1> tgs;

            for (auto& tn: ns)
            {
              if (tn.qualified ())
                fail (nloc) << "project name in target " << tn;

              // @@ OUT TODO
              //
              enter_target tg (*this, move (tn), name (), false, nloc, trace);

              if (default_target_ == nullptr)
                default_target_ = target_;

              target_->prerequisites_state_.store (2, memory_order_relaxed);
              target_->prerequisites_.reserve (pns.size ());
              tgs.push_back (*target_);
            }

            // Now enter each prerequisite into each target.
            //
            for (auto& pn: pns)
            {
              optional<string> e;
              const target_type* ti (scope_->find_target_type (pn, e));

              if (ti == nullptr)
                fail (ploc) << "unknown target type " << pn.type;

              // Current dir collapses to an empty one.
              //
              if (!pn.dir.empty ())
                pn.dir.normalize (false, true);

              // @@ OUT: for now we assume the prerequisite's out is
              // undetermined. The only way to specify an src prerequisite
              // will be with the explicit @-syntax.
              //
              // Perhaps use @file{foo} as a way to specify it is in the out
              // tree, e.g., to suppress any src searches? The issue is what
              // to use for such a special indicator. Also, one can easily and
              // natually suppress any searches by specifying the absolute
              // path.
              //
              prerequisite p (pn.proj,
                              *ti,
                              move (pn.dir),
                              dir_path (),
                              move (pn.value),
                              move (e),
                              *scope_);

              for (auto i (tgs.begin ()), e (tgs.end ()); i != e; )
              {
                // Move last prerequisite (which will normally be the only
                // one).
                //
                target& t (*i);
                t.prerequisites_.push_back (
                  ++i == e
                  ? move (p)
                  : prerequisite (p, memory_order_relaxed)); // Serial
              }
            }
          }

          if (tt == type::newline)
            next (t, tt);
          else if (tt != type::eos)
            fail (t) << "expected newline instead of " << t;

          continue;
        }

        if (tt == type::eos)
          continue;

        fail (t) << "expected newline instead of " << t;
      }

      // Variable assignment.
      //
      if (tt == type::assign || tt == type::prepend || tt == type::append)
      {
        const variable& var (
          var_pool.rw (*scope_).insert (
            parse_variable_name (move (ns), nloc),
            true)); // Allow overrides.

        // Apply variable attributes.
        //
        apply_variable_attributes (var);

        if (var.visibility == variable_visibility::target)
          fail (nloc) << "variable " << var << " has target visibility but "
                      << "assigned in a scope" <<
            info << "consider changing to '*: " << var << "'";

        parse_variable (t, tt, var, tt);

        if (tt == type::newline)
          next (t, tt);
        else if (tt != type::eos)
          fail (t) << "expected newline instead of " << t;

        continue;
      }

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

      fail (t) << "unexpected " << t;
    }

    return parsed;
  }

  void parser::
  parse_source (token& t, type& tt)
  {
    tracer trace ("parser::parse_source", &path_);

    // The rest should be a list of buildfiles. Parse them as names in the
    // value mode to get variable expansion and directory prefixes.
    //
    mode (lexer_mode::value, '@');
    next (t, tt);
    const location l (get_location (t));
    names ns (tt != type::newline && tt != type::eos
              ? parse_names (t, tt,
                             pattern_mode::expand,
                             false,
                             "path",
                             nullptr)
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

        l5 ([&]{trace (t) << "entering " << p;});

        enter_buildfile (p);

        const path* op (path_);
        path_ = &p;

        lexer l (ifs, *path_);
        lexer* ol (lexer_);
        lexer_ = &l;

        token t;
        type tt;
        next (t, tt);
        parse_clause (t, tt);

        if (tt != type::eos)
          fail (t) << "unexpected " << t;

        l5 ([&]{trace (t) << "leaving " << p;});

        lexer_ = ol;
        path_ = op;
      }
      catch (const io_error& e)
      {
        fail (l) << "unable to read buildfile " << p << ": " << e;
      }
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
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
              ? parse_names (t, tt,
                             pattern_mode::expand,
                             false,
                             "path",
                             nullptr)
              : names ());

    for (name& n: ns)
    {
      if (n.pair || n.qualified () || n.typed () || n.empty ())
        fail (l) << "expected buildfile instead of " << n;

      // Construct the buildfile path. If it is a directory, then append
      // 'buildfile'.
      //
      path p (move (n.dir));
      if (n.value.empty ())
        p /= "buildfile";
      else
      {
        bool d (path::traits::is_separator (n.value.back ()));

        p /= path (move (n.value));
        if (d)
          p /= "buildfile";
      }

      l6 ([&]{trace (l) << "relative path " << p;});

      // Determine new out_base.
      //
      dir_path out_base;

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

      // Switch the scope. Note that we need to do this before figuring
      // out the absolute buildfile path since we may switch the project
      // root and src_root with it (i.e., include into a sub-project).
      //
      scope* ors (root_);
      scope* ocs (scope_);
      const dir_path* opb (pbase_);
      switch_scope (out_base);

      // Use the new scope's src_base to get absolute buildfile path
      // if it is relative.
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

        l5 ([&]{trace (t) << "entering " << p;});

        enter_buildfile (p);

        const path* op (path_);
        path_ = &p;

        lexer l (ifs, *path_);
        lexer* ol (lexer_);
        lexer_ = &l;

        target* odt (default_target_);
        default_target_ = nullptr;

        token t;
        type tt;
        next (t, tt);
        parse_clause (t, tt);

        if (tt != type::eos)
          fail (t) << "unexpected " << t;

        process_default_target (t);

        l5 ([&]{trace (t) << "leaving " << p;});

        default_target_ = odt;
        lexer_ = ol;
        path_ = op;
      }
      catch (const io_error& e)
      {
        fail (l) << "unable to read buildfile " << p << ": " << e;
      }

      pbase_ = opb;
      scope_ = ocs;
      root_ = ors;
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
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
    next (t, tt);

    // Get variable attributes, if any (note that here we will go into a
    // nested value mode with a different pair character).
    //
    auto at (attributes_push (t, tt));

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
      auto& vp (var_pool.rw (*scope_));

      if (p != string::npos)
        var = &vp.insert (split (p), true); // Allow overrides.
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
          var = &vp.insert (move (t.value), true); // Allow overrides.
          next (t, tt); // Get the peeked token.
          split (p);    // Returned name should be empty.
        }
      }
    }

    if (var != nullptr)
    {
      // Apply variable attributes.
      //
      apply_variable_attributes (*var);

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

      // build2::import() will check the name, if required.
      //
      names r (build2::import (*scope_, move (n), l));

      if (val != nullptr)
      {
        if (atype == type::assign)
          val->assign (move (r), var);
        else if (atype == type::prepend)
          val->prepend (move (r), var);
        else
          val->append (move (r), var);
      }
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
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

    // The rest is a value. Parse it as a variable value to get expansion,
    // attributes, etc. build2::import() will check the names, if required.
    //
    location l (get_location (t));
    value rhs (parse_variable_value (t, tt));

    // While it may seem like supporting attributes is a good idea here,
    // there is actually little benefit in being able to type them or to
    // return NULL.
    //
    // export_value_ = value (); // Reset to untyped NULL value.
    // value_attributes (nullptr,
    //                   export_value_,
    //                   move (rhs),
    //                   type::assign);
    if (attributes& a = attributes_top ())
      fail (a.loc) << "attributes in export";
    else
      attributes_pop ();

    if (!rhs)
      fail (l) << "null value in export";

    if (rhs.type != nullptr)
      untypify (rhs);

    export_value_ = move (rhs).as<names> ();

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
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
              ? parse_names (t, tt,
                             pattern_mode::ignore,
                             false,
                             "module",
                             nullptr)
              : names ());

    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      string n, v;

      if (!i->simple ())
        fail (l) << "module name expected instead of " << *i;

      n = move (i->value);

      if (i->pair)
      {
        if (i->pair != '@')
          fail << "unexpected pair style in using directive";

        ++i;
        if (!i->simple ())
          fail (l) << "module version expected instead of " << *i;

        v = move (i->value);
      }

      // Handle the special 'build' module.
      //
      if (n == "build")
      {
        if (!v.empty ())
        {
          unsigned int iv;
          try {iv = to_version (v);}
          catch (const invalid_argument& e)
          {
            fail (l) << "invalid version '" << v << "': " << e << endf;
          }

          if (iv > BUILD2_VERSION)
            fail (l) << "build2 " << v << " required" <<
              info << "running build2 " << BUILD2_VERSION_STR;
        }
      }
      else
      {
        assert (v.empty ()); // Module versioning not yet implemented.

        if (boot_)
          boot_module (*root_, n, l);
        else
          load_module (*root_, *scope_, n, l, optional);
      }
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
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

      if (!scope_->derive_target_type (move (dn), *bt).second)
        fail (dnl) << "target type " << dn << " already define in this scope";

      next (t, tt); // Get newline.
    }
    else
      fail (t) << "expected name instead of " << t << " in target type "
               << "definition";

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
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
      next (t, tt);

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

          // Parse as names to get variable expansion, evaluation, etc. Note
          // that we also expand patterns (could be used in nested contexts,
          // etc; e.g., "if pattern expansion is empty" condition).
          //
          const location l (get_location (t));

          try
          {
            // Should evaluate to 'true' or 'false'.
            //
            bool e (
              convert<bool> (
                parse_value (t, tt,
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

      // This can be a block or a single line.
      //
      if (next (t, tt) == type::lcbrace)
      {
        if (next (t, tt) != type::newline)
          fail (t) << "expected newline after {";

        next (t, tt);

        if (take)
        {
          parse_clause (t, tt);
          taken = true;
        }
        else
          skip_block (t, tt);

        if (tt != type::rcbrace)
          fail (t) << "expected } instead of " << t << " at the end of " << k
                   << "-block";

        next (t, tt);

        if (tt == type::newline)
          next (t, tt);
        else if (tt != type::eos)
          fail (t) << "expected newline after }";
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
  parse_assert (token& t, type& tt)
  {
    bool neg (t.value.back () == '!');
    const location al (get_location (t));

    // Parse the next chunk as names to get variable expansion, evaluation,
    // etc. Do it in the value mode so that we don't treat ':', etc., as
    // special.
    //
    mode (lexer_mode::value);
    next (t, tt);

    const location el (get_location (t));

    try
    {
      // Should evaluate to 'true' or 'false'.
      //
      bool e (
        convert<bool> (
          parse_value (t, tt,
                       pattern_mode::expand,
                       "expression",
                       nullptr,
                       true)));
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
                             false,
                             "description",
                             nullptr)
              : names ());

    diag_record dr (fail (al));
    dr << "assertion failed";

    if (!ns.empty ())
      dr << ": " << ns;
  }

  void parser::
  parse_print (token& t, type& tt)
  {
    // Parse the rest as a variable value to get expansion, attributes, etc.
    //
    value rhs (parse_variable_value (t, tt));

    value lhs;
    apply_value_attributes (nullptr, lhs, move (rhs), type::assign);

    if (lhs)
    {
      names storage;
      cout << reverse (lhs, storage) << endl;
    }
    else
      cout << "[null]" << endl;

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  string parser::
  parse_variable_name (names&& ns, const location& l)
  {
    // The list should contain a single, simple name.
    //
    if (ns.size () != 1 || !ns[0].simple () || ns[0].empty ())
      fail (l) << "variable name expected instead of " << ns;

    string& n (ns[0].value);

    if (n.front () == '.') // Fully qualified name.
      return string (n, 1, string::npos);
    else
      //@@ TODO: append namespace if any.
      return move (n);
  }

  void parser::
  parse_variable (token& t, type& tt, const variable& var, type kind)
  {
    value rhs (parse_variable_value (t, tt));

    value& lhs (
      kind == type::assign
      ? (target_ != nullptr
         ? target_->assign (var)
         :  scope_->assign (var))
      : (target_ != nullptr
         ? target_->append (var)
         :  scope_->append (var)));

    apply_value_attributes (&var, lhs, move (rhs), kind);
  }

  value parser::
  parse_variable_value (token& t, type& tt)
  {
    mode (lexer_mode::value, '@');
    next (t, tt);

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
      n == "target_triplet" ? ptr (value_traits<target_triplet>::value_type) :
      n == "strings"        ? ptr (value_traits<strings>::value_type)        :
      n == "paths"          ? ptr (value_traits<paths>::value_type)          :
      n == "dir_paths"      ? ptr (value_traits<dir_paths>::value_type)      :
      n == "names"          ? ptr (value_traits<vector<name>>::value_type)   :
      n == "name_pair"      ? ptr (value_traits<name_pair>::value_type)      :
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

    for (auto& p: a.ats)
    {
      string& k (p.first);
      string& v (p.second);

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

        if (!v.empty ())
          dr << '=' << v;
      }

      if (!v.empty ())
        fail (l) << "unexpected value for attribute " << k << ": " << v;
    }

    if (type != nullptr)
    {
      if (var.type == nullptr)
      {
        const bool o (true); // Allow overrides.
        var_pool.update (const_cast<variable&> (var), type, nullptr, &o);
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

    for (auto& p: a.ats)
    {
      string& k (p.first);
      string& v (p.second);

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

        if (!v.empty ())
          dr << '=' << v;
      }

      if (!v.empty ())
        fail (l) << "unexpected value for attribute " << k << ": " << v;
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

  values parser::
  parse_eval (token& t, type& tt, pattern_mode pmode)
  {
    // enter: lparen
    // leave: rparen

    mode (lexer_mode::eval, '@'); // Auto-expires at rparen.
    next (t, tt);

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
    // enter: first token of LHS
    // leave: next token after last RHS

    // Left-associative: parse in a loop for as long as we can.
    //
    values r;
    value lhs (parse_eval_ternary (t, tt, pmode, first));

    if (!pre_parse_)
      r.push_back (move (lhs));

    while (tt == type::comma)
    {
      next (t, tt);
      value rhs (parse_eval_ternary (t, tt, pmode));

      if (!pre_parse_)
        r.push_back (move (rhs));
    }

    return r;
  }

  value parser::
  parse_eval_ternary (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS
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

    next (t, tt);
    value mhs (parse_eval_ternary (t, tt, pmode));

    if (tt != type::colon)
      fail (t) << "expected ':' instead of " << t;

    if (!pp)
      pre_parse_ = q; // Short-circuit right?

    next (t, tt);
    value rhs (parse_eval_ternary (t, tt, pmode));

    pre_parse_ = pp;
    return q ? move (mhs) : move (rhs);
  }

  value parser::
  parse_eval_or (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of LHS
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

        next (t, tt);
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
    // enter: first token of LHS
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

        next (t, tt);
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
    // enter: first token of LHS
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

      next (t, tt);
      value rhs (parse_eval_value (t, tt, pmode));

      if (pre_parse_)
        continue;

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
          fail (l) << "comparison between " << lhs.type->name << " and "
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

      // Store the result as a bool value.
      //
      lhs = value (r);
    }

    return lhs;
  }

  value parser::
  parse_eval_value (token& t, type& tt, pattern_mode pmode, bool first)
  {
    // enter: first token of value
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
        next (t, tt);
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
    // (scope/target qualified name represented as a special ':'-style pair).
    //
    if (first && tt == type::colon)
    {
      if (at.first)
        fail (at.second) << "attributes before qualified variable name";

      attributes_pop ();

      const location nl (get_location (t));
      next (t, tt);
      value n (parse_value (t, tt, pattern_mode::ignore));

      if (tt != type::rparen)
        fail (t) << "expected ')' after variable name";

      if (pre_parse_)
        return v; // Empty.

      if (v.type != nullptr || !v || v.as<names> ().size () != 1)
        fail (l) << "expected scope/target before ':'";

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

  pair<bool, location> parser::
  attributes_push (token& t, type& tt, bool standalone)
  {
    location l (get_location (t));
    bool has (tt == type::lsbrace);

    if (!pre_parse_)
      attributes_.push (attributes {has, l, {}});

    if (!has)
      return make_pair (false, l);

    // Using '@' for attribute key-value pairs would be just too ugly. Seeing
    // that we control what goes into keys/values, let's use a much nicer '='.
    //
    mode (lexer_mode::attribute, '=');
    next (t, tt);

    if (tt != type::rsbrace)
    {
      names ns (
        parse_names (
          t, tt, pattern_mode::ignore, false, "attribute", nullptr));

      if (!pre_parse_)
      {
        attributes& a (attributes_.top ());

        for (auto i (ns.begin ()); i != ns.end (); ++i)
        {
          string k, v;

          try
          {
            k = convert<string> (move (*i));
          }
          catch (const invalid_argument&)
          {
            fail (l) << "invalid attribute key '" << *i << "'";
          }

          if (i->pair)
          {
            if (i->pair != '=')
              fail (l) << "unexpected pair style in attributes";

            try
            {
              v = convert<string> (move (*++i));
            }
            catch (const invalid_argument&)
            {
              fail (l) << "invalid attribute value '" << *i << "'";
            }
          }

          a.ats.emplace_back (move (k), move (v));
        }
      }
    }

    if (tt != type::rsbrace)
      fail (t) << "expected ']' instead of " << t;

    next (t, tt);

    if (!standalone && (tt == type::newline || tt == type::eos))
      fail (t) << "standalone attributes";

    return make_pair (true, l);
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
                const optional<string>& pp,
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
      optional<string> p;
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

  // Expand a name pattern. Note that the result can empty (as in "no
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

    // Compare string to name as paths and according to dir.
    //
    auto equal = [&dir] (const string& v, const name& n) -> bool
    {
      // Use path comparison (which may be slash/case-insensitive).
      //
      return path::traits::compare (
        v, dir ? n.dir.representation () : n.value) == 0;
    };

    // Compare name to pattern as paths and according to dir.
    //
    auto match = [&dir] (const string& p, const name& n) -> bool
    {
      // Currently exclusions only match the last component to a pattern.
      //
      string c (dir
                ? n.dir.leaf ().representation ()
                : string (n.value, path::traits::find_leaf (n.value)));

      return butl::path_match (p, c);
    };

    // Append string to result according to dir. Store an indication of
    // whether it was amended in the pair flag.
    //
    auto append = [&r, &dir] (string&& v, bool a)
    {
      r.push_back (dir ? name (dir_path (move (v))) : name (move (v)));
      r.back ().pair = a ? 'a' : '\0';
    };

    auto include_match = [&r, &equal, &append] (string&& m, bool a)
    {
      auto i (find_if (
                r.begin (),
                r.end (),
                [&m, &equal] (const name& n) {return equal (m, n);}));

      if (i == r.end ())
        append (move (m), a);
    };

    auto include_pattern = [&r, &append, &include_match, dp, this]
      (string&& p, bool a)
    {
      // If we don't already have any matches and our pattern doesn't contain
      // multiple recursive wildcards, then the result will be unique and we
      // can skip checking for duplicated. This should help quite a bit in the
      // common cases where we have a pattern plus maybe a few exclusions.
      //
      bool unique (false);
      if (r.empty ())
      {
        size_t i (p.find ("**"));
        unique = (i == string::npos || p.find ("**", i + 2) == string::npos);
      }

      //@@ PAT TODO: weed out starting with dot (unless pattern starts
      //   with dot; last component? intermediate components?).
      //
      function<bool (path&&)> func;
      if (unique)
        func = [a, &append] (path&& m)
          {
            append (move (m).representation (), a);
            return true;
          };
      else
        func = [a, &include_match] (path&& m)
          {
            include_match (move (m).representation (), a);
            return true;
          };

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

      butl::path_search (path (move (p)), func, *sp);
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

    auto exclude_pattern = [&r, &match] (const string& p)
    {
      for (auto i (r.begin ()); i != r.end (); )
      {
        if (match (p, *i))
          i = r.erase (i);
        else
          ++i;
      }
    };

    // Process the pattern and inclusions/exclusions.
    //
    for (auto b (pat.begin ()), i (b), e (pat.end ()); i != e; ++i)
    {
      name& n (*i);
      bool first (i == b);

      if (n.empty () || !(n.simple () || n.directory ()))
        fail (l) << "invalid '" << n << "' in " << what << " pattern";

      string v (n.simple () ? move (n.value) : move (n.dir).representation ());

      // Figure out if this is inclusion or exclusion.
      //
      char s; // +/-
      if (first)
        s = '+'; // Treat as inclusion.
      else
      {
        s = v[0];

        if (s != '-' && s != '+')
          fail (l) << "missing leading +/- in '" << v << "' " << what
                   << " pattern";

        v.erase (0, 1);

        if (v.empty ())
          fail (l) << "empty " << what << " pattern";
      }

      // Amend the pattern or match in a target type-specific manner.
      //
      bool a (tt != nullptr &&
              tt->pattern != nullptr &&
              tt->pattern (*tt, *scope_, v, false));

      // Figure out if this is a pattern.
      //
      bool p (v.find_first_of ("*?") != string::npos);
      assert (p || !first); // First must be a pattern.

      // Based on the first pattern figure out if the result is a directory or
      // a file. For inclusions/exclusions verify it is consistent.
      //
      {
        bool d (path::traits::is_separator (v.back ()));

        if (first)
          dir = d;
        else if (d != dir)
          fail (l) << "inconsistent file/directory result in " << what
                   << " pattern";
      }

      if (s == '+')
      {
        if (p)
          include_pattern (move (v), a);
        else
          include_match (move (v), a);
      }
      else
      {
        if (p)
          exclude_pattern (v);
        else
          exclude_match (v);
      }
    }

    // Reverse target type-specific pattern/match amendments from the result.
    // Essentially: cxx{*} -> *.cxx -> foo.cxx -> cxx{foo}.
    //
    if (tt != nullptr && tt->pattern != nullptr)
    {
      for (name& n: r)
      {
        if (n.pair)
        {
          string v (dir ? move (n.dir).representation () : move (n.value));
          tt->pattern (*tt, *scope_, v, true);

          if (dir)
            n.dir = dir_path (move (v));
          else
            n.value = move (v);

          n.pair = '\0';
        }
      }
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
                       const optional<string>& pp,
                       const dir_path* dp,
                       const string* tp,
                       bool cross)
  {
    assert (!pre_parse_);

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
      const optional<string>& pp,
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
          false,
          what,
          separators,
          0,                    // Handled by the splice_names() call below.
          pp, dp, tp, false).pattern);

      if (tt != type::rcbrace)
        fail (t) << "expected '}' instead of " << t;

      // See if this is a pattern.
      //
      if (pat_tt)
      {
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
        parse (nullopt, nullptr, nullptr);
        r.clear ();
        continue;
      }

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

  // Slashe(s) plus '%'. Note that here we assume '/' is there since that's
  // in our buildfile "syntax".
  //
  const string parser::name_separators (
    string (path::traits::directory_separators) + '%');

  auto parser::
  parse_names (token& t, type& tt,
               names& ns,
               pattern_mode pmode,
               bool chunk,
               const char* what,
               const string* separators,
               size_t pairn,
               const optional<string>& pp,
               const dir_path* dp,
               const string* tp,
               bool cross) -> parse_names_result
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
          auto g (
            make_exception_guard (
              [&loc, l, r] ()
              {
                if (verb != 0)
                  info (loc) << "while concatenating " << l << " to " << r <<
                    info << "use quoting to force untyped concatenation";
              }));

          p = functions.try_call (
            "builtin.concat", vector_view<value> (a), loc);
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
        untypify (rhs);
        names& d (rhs.as<names> ());
        assert (d.size () == 1); // Must be single value.
        concat_data = move (d[0]);
      }
    };

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

      // Return true if the next token which should be peeked at won't be part
      // of the name.
      //
      auto last_token = [chunk, this] ()
      {
        const token& t (peeked ());
        type tt (t.type);

        return ((chunk && t.separated) ||
                (tt != type::word    &&
                 tt != type::dollar  &&
                 tt != type::lparen  &&
                 tt != type::lcbrace &&
                 tt != type::pair_separator));
      };

      // Return true if the next token which should be peeked at won't be part
      // of this concatenation.
      //
      auto last_concat = [this] ()
      {
        const token& t (peeked ());
        type tt (t.type);

        return (t.separated         ||
                (tt != type::word   &&
                 tt != type::dollar &&
                 tt != type::lparen));
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
        // de-type it and inject in order to support our directory/target-type
        // syntax, for example:
        //
        // $out_root/foo/lib{bar}
        // $out_root/$libtype{bar}
        //
        // This means that a target type must be a valid path component.
        //
        vnull = false; // A concatenation cannot produce NULL.

        if (vtype != nullptr)
        {
          if (tt == type::lcbrace && !peeked ().separated)
          {
            if (vtype == &value_traits<path>::value_type ||
                vtype == &value_traits<string>::value_type)
              ; // Representation is already in concat_data.value.
            else if (vtype == &value_traits<dir_path>::value_type)
              concat_data.value = move (concat_data.dir).representation ();
            else
              fail (t) << "expected directory and/or target type "
                       << "instead of " << vtype->name;

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
      }

      // Name.
      //
      if (tt == type::word)
      {
        tt = peek ();

        if (pre_parse_)
          continue;

        string val (move (t.value));

        // Should we accumulate? If the buffer is not empty, then
        // we continue accumulating (the case where we are separated
        // should have been handled by the injection code above). If
        // the next token is a var expansion or eval context and it
        // is not separated, then we need to start accumulating.
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
          concat_quoted = t.qtype != quote_type::unquoted || concat_quoted;
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
        optional<string> p1;
        const optional<string>* pp1 (&pp);

        if (p != string::npos)
        {
          bool last (val[p] == '%');
          string::size_type q (last ? p : val.rfind ('%', p - 1));

          if (q != string::npos)
          {
            string proj;
            proj.swap (val);

            // First fix the rest of the name.
            //
            val.assign (proj, q + 1, string::npos);
            p = last ? string::npos : p - (q + 1);

            // Now process the project name.
            //
            proj.resize (q);

            if (pp)
              fail (t) << "nested project name " << proj;

            p1 = move (proj);
            pp1 = &p1;
          }
        }

        string::size_type n (p != string::npos ? val.size () - 1 : 0);

        // See if this is a type name, directory prefix, or both. That
        // is, it is followed by an un-separated '{'.
        //
        if (tt == type::lcbrace && !peeked ().separated)
        {
          next (t, tt);

          if (p != n && tp != nullptr)
            fail (t) << "nested type name " << val;

          dir_path d1;
          const dir_path* dp1 (dp);

          string t1;
          const string* tp1 (tp);

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

          count = parse_names_trailer (
            t, tt, ns, pmode, what, separators, pairn, *pp1, dp1, tp1, cross);
          tt = peek ();
          continue;
        }

        // See if this is a wildcard pattern.
        //
        if (pmode != pattern_mode::ignore   &&
            !*pp1                           && // Cannot be project-qualified.
            t.qtype == quote_type::unquoted && // Cannot be quoted.
            ((dp != nullptr && dp->absolute ()) || pbase_ != nullptr) &&
            val.find_first_of ("*?") != string::npos)
        {
          // Resolve the target if there is one. If we fail, then this is not
          // a pattern.
          //
          const target_type* tt (tp != nullptr && scope_ != nullptr
                                 ? scope_->find_target_type (*tp)
                                 : nullptr);

          if (tp == nullptr || tt != nullptr)
          {
            if (pmode == pattern_mode::expand)
            {
              count = expand_name_pattern (get_location (t),
                                           names {name (move (val))},
                                           ns,
                                           what,
                                           pairn,
                                           dp, tp, tt);
              continue;
            }

            // The goal of the detect mode is to assemble the "raw" list (the
            // pattern itself plus inclusions/exclusions) that will be passed
            // to parse_names_pattern(). So clear pair, directory, and type
            // (they will be added during pattern expansion) and change the
            // mode to ignore (to prevent any expansions in
            // inclusions/exclusions).
            //
            pairn = 0;
            dp = nullptr;
            tp = nullptr;
            pmode = pattern_mode::ignore;
            rpat = tt;

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
        // @@ TODO: and not quoted (but what about partially quoted, e.g.,
        //    "foo bar"/ or concatenated, e.g., $dir/foo/).
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

          if (tt == type::word)
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

              // Make sure the result of evaluation is a potentially-qualified
              // simple name.
              //
              if (n > 2 ||
                  (n == 2 && ns[0].pair != ':') ||
                  !ns[n - 1].simple ())
                fail (loc) << "expected variable/function name instead of '"
                           << ns << "'";

              if (n == 2)
              {
                qual = move (ns[0]);

                if (qual.empty ())
                  fail (loc) << "empty variable/function qualification";

                qual.pair = '\0'; // We broke up the pair.
              }

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
            // context in which to call the function?
            //
            values args (parse_eval (t, tt, pmode));
            tt = peek ();

            if (pre_parse_)
              continue; // As if empty result.

            // Note that we "move" args to call().
            //
            result_data = functions.call (name, args, loc);
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
        // or var expansion and it is not separated, then we need to start
        // accumulating.
        //
        if (concat        || // Continue.
            !last_concat ()) // Start.
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
              auto g (
                make_exception_guard (
                  [&loc, t] ()
                  {
                    if (verb != 0)
                      info (loc) << "while converting " << t << " to string";
                  }));

              p = functions.try_call (
                "string", vector_view<value> (&result_data, 1), loc);
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

          ns.back ().pair = pair_separator ();

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
  keyword (token& t)
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
  // handle the opening paren. In other words, a buildspec like 'clean (./)'
  // is "chunked" as 'clean', '(', etc. While this is fairly straightforward,
  // there is one snag: concatenating eval contexts, as in
  // 'clean(./)'. Normally, this will be treated as a single chunk and we
  // don't want that. So here comes the trick (or hack, if you like): we will
  // make every opening paren token "separated" (i.e., as if it was proceeded
  // by a space). This will disable concatenating eval. In fact, we will even
  // go a step further and only do this if we are in the original value
  // mode. This will allow us to still use eval contexts in buildspec,
  // provided that we quote it: '"cle(an)"'. Note also that function calls
  // still work as usual: '$filter (clean test)'.  To disable a function call
  // and make it instead a var that is expanded into operation name(s), we can
  // use quoting: '"$ops"(./)'.
  //
  static void
  paren_processor (token& t, const lexer& l)
  {
    if (t.type == type::lparen && l.mode () == lexer_mode::value)
      t.separated = true;
  }

  buildspec parser::
  parse_buildspec (istream& is, const path& name)
  {
    path_ = &name;

    // We do "effective escaping" and only for ['"\$(] (basically what's
    // necessary inside a double-quoted literal plus the single quote).
    //
    lexer l (is, *path_, "\'\"\\$(", &paren_processor);
    lexer_ = &l;
    target_ = nullptr;
    scope_ = root_ = scope::global_;
    pbase_ = &work; // Use current working directory.

    // Turn on the value mode/pairs recognition with '@' as the pair separator
    // (e.g., src_root/@out_root/exe{foo bar}).
    //
    mode (lexer_mode::value, '@');

    token t;
    type tt;
    next (t, tt);

    return parse_buildspec_clause (t, tt, type::eos);
  }

  static bool
  opname (const name& n)
  {
    // First it has to be a non-empty simple name.
    //
    if (n.pair || !n.simple () || n.empty ())
      return false;

    // C identifier.
    //
    for (size_t i (0); i != n.value.size (); ++i)
    {
      char c (n.value[i]);
      if (c != '_' && !(i != 0 ? alnum (c) : alpha (c)))
        return false;
    }

    return true;
  }

  buildspec parser::
  parse_buildspec_clause (token& t, type& tt, type tt_end)
  {
    buildspec bs;

    while (tt != tt_end)
    {
      // We always start with one or more names. Eval context (lparen) only
      // allowed if quoted.
      //
      if (tt != type::word    &&
          tt != type::lcbrace &&      // Untyped name group: '{foo ...'
          tt != type::dollar  &&      // Variable expansion: '$foo ...'
          !(tt == type::lparen && mode () == lexer_mode::double_quoted) &&
          tt != type::pair_separator) // Empty pair LHS: '@foo ...'
        fail (t) << "operation or target expected instead of " << t;

      const location l (get_location (t)); // Start of names.

      // This call will parse the next chunk of output and produce zero or
      // more names.
      //
      names ns (parse_names (t, tt, pattern_mode::expand, true));

      if (ns.empty ()) // Can happen if pattern expansion.
        fail (l) << "operation or target expected";

      // What these names mean depends on what's next. If it is an
      // opening paren, then they are operation/meta-operation names.
      // Otherwise they are targets.
      //
      if (tt == type::lparen) // Peeked into by parse_names().
      {
        if (ns.empty ())
          fail (t) << "operation name expected before '('";

        for (const name& n: ns)
          if (!opname (n))
            fail (l) << "operation name expected instead of '" << n << "'";

        // Inside '(' and ')' we have another, nested, buildspec.
        //
        next (t, tt);
        const location l (get_location (t)); // Start of nested names.
        buildspec nbs (parse_buildspec_clause (t, tt, type::rparen));

        // Merge the nested buildspec into ours. But first determine
        // if we are an operation or meta-operation and do some sanity
        // checks.
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
            // If we have any operations in the nested spec, then this
            // mean that our names are meta-operation names.
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
          }
        }
        else
        {
          // Since we are not a meta-operation, the nested buildspec
          // should be just a bunch of targets.
          //
          assert (nmo.size () == 1);
          const opspec& nos (nmo.back ());

          if (bs.empty () || !bs.back ().name.empty ())
            bs.push_back (metaopspec ()); // Empty (default) meta operation.

          for (name& n: ns)
          {
            bs.back ().push_back (nos);
            bs.back ().back ().name = move (n.value);
          }
        }

        next (t, tt); // Done with '('.
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
            fail (l) << "target name expected instead of " << *i;

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

    // Process variable name.
    //
    if (name.front () == '.') // Fully namespace-qualified name.
      name.erase (0, 1);
    else
    {
      //@@ TODO: append namespace if any.
    }

    // If we are qualified, it can be a scope or a target.
    //
    enter_scope sg;
    enter_target tg;

    if (qual.directory ()) //@@ OUT
      sg = enter_scope (*this, move (qual.dir));
    else if (!qual.empty ())
      // @@ OUT TODO
      //
      tg = enter_target (
        *this, move (qual), build2::name (), true, loc, trace);

    // Lookup.
    //
    const auto& var (var_pool.rw (*scope_).insert (move (name), true));
    return target_ != nullptr ? (*target_)[var] : (*scope_)[var];

    // Undefined/NULL namespace variables are not allowed.
    //
    // @@ TMP this isn't proving to be particularly useful.
    //
    // if (!l)
    // {
    //   if (var.name.find ('.') != string::npos)
    //     fail (loc) << "undefined/null namespace variable " << var;
    // }
  }

  void parser::
  switch_scope (const dir_path& d)
  {
    tracer trace ("parser::switch_scope", &path_);

    auto p (build2::switch_scope (*root_, d));
    scope_ = &p.first;
    pbase_ = scope_->src_path_;

    if (p.second != nullptr && p.second != root_)
    {
      root_ = p.second;
      l5 ([&]{trace << "switching to root scope " << root_->out_path ();});
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
    if (default_target_ == nullptr ||      // No targets in this buildfile.
        targets.find (dir::static_type,    // Explicit current dir target.
                      scope_->out_path (),
                      dir_path (),         // Out tree target.
                      string (),
                      nullopt,
                      trace) != nullptr)
      return;

    target& dt (*default_target_);

    l5 ([&]{trace (t) << "creating current directory alias for " << dt;});

    // While this target is not explicitly mentioned in the buildfile, we say
    // that we behave as if it were. Thus not implied.
    //
    target& ct (
      targets.insert (dir::static_type,
                      scope_->out_path (),
                      dir_path (),
                      string (),
                      nullopt,
                      false,
                      trace).first);


    ct.prerequisites_state_.store (2, memory_order_relaxed);
    ct.prerequisites_.emplace_back (prerequisite (dt));
  }

  void parser::
  enter_buildfile (const path& p)
  {
    tracer trace ("parser::enter_buildfile", &path_);

    dir_path d (p.directory ());

    // Figure out if we need out.
    //
    dir_path out;
    if (scope_->src_path_ != nullptr &&
        scope_->src_path () != scope_->out_path () &&
        d.sub (scope_->src_path ()))
    {
      out = out_src (d, *root_);
    }

    targets.insert<buildfile> (
      move (d),
      move (out),
      p.leaf ().base ().string (),
      p.extension (),               // Always specified.
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
