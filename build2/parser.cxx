// file      : build2/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/parser>

#include <cctype>   // is{alpha alnum}()
#include <fstream>
#include <iostream>

#include <build2/version>

#include <build2/scope>
#include <build2/target>
#include <build2/prerequisite>
#include <build2/variable>
#include <build2/module>
#include <build2/file>
#include <build2/diagnostics>
#include <build2/context>

using namespace std;

namespace build2
{
  static location
  get_location (const token&, const void*);

  typedef token_type type;

  void parser::
  parse_buildfile (istream& is, const path& p, scope& root, scope& base)
  {
    enter_buildfile (p);

    path_ = &p;

    lexer l (is, *path_);
    lexer_ = &l;
    target_ = nullptr;
    scope_ = &base;
    root_ = &root;
    default_target_ = nullptr;

    token t (type::eos, false, 0, 0);
    type tt;
    next (t, tt);

    clause (t, tt);

    if (tt != type::eos)
      fail (t) << "unexpected " << t;

    process_default_target (t);
  }

  token parser::
  parse_variable (lexer& l, scope& s, const variable_type& var, type kind)
  {
    path_ = &l.name ();
    lexer_ = &l;
    target_ = nullptr;
    scope_ = &s;

    type tt;
    token t (type::eos, false, 0, 0);
    variable (t, tt, var, kind);
    return t;
  }

  token parser::
  parse_variable_value (lexer& l, scope& s, names_type& result)
  {
    path_ = &l.name ();
    lexer_ = &l;
    target_ = nullptr;
    scope_ = &s;

    type tt;
    token t (type::eos, false, 0, 0);
    result = variable_value (t, tt);
    return t;
  }

  void parser::
  clause (token& t, type& tt)
  {
    tracer trace ("parser::clause", &path_);

    // clause() should always stop at a token that is at the beginning of
    // the line (except for eof). That is, if something is called to parse
    // a line, it should parse it until newline (or fail). This is important
    // for if-else blocks, directory scopes, etc., that assume the } token
    // they see is on the new line.
    //
    while (tt != type::eos)
    {
      // Extract attributes if any.
      //
      location al (get_location (t, &path_));
      attributes_type* as (attributes (t, tt));

      // We always start with one or more names.
      //
      if (tt != type::name    &&
          tt != type::lcbrace && // Untyped name group: '{foo ...'
          tt != type::dollar  && // Variable expansion: '$foo ...'
          tt != type::lparen  && // Eval context: '(foo) ...'
          tt != type::colon)     // Empty name: ': ...'
        break; // Something else. Let our caller handle that.

      // See if this is one of the directives.
      //
      if (tt == type::name && keyword (t))
      {
        const string& n (t.value);
        void (parser::*f) (token&, token_type&) = nullptr;

        if (n == "print")
        {
          // @@ Is this the only place where it is valid? Probably also
          // in var namespace.
          //
          f = &parser::print;
        }
        else if (n == "source")
        {
          f = &parser::source;
        }
        else if (n == "include")
        {
          f = &parser::include;
        }
        else if (n == "import")
        {
          f = &parser::import;
        }
        else if (n == "export")
        {
          f = &parser::export_;
        }
        else if (n == "using" ||
                 n == "using?")
        {
          f = &parser::using_;
        }
        else if (n == "define")
        {
          f = &parser::define;
        }
        else if (n == "if" ||
                 n == "if!")
        {
          f = &parser::if_else;
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
          if (as != nullptr)
            fail (al) << "attributes before " << n;

          (this->*f) (t, tt);
          continue;
        }
      }

      // ': foo' is equvalent to '{}: foo' and to 'dir{}: foo'.
      //
      // @@ I think we should make ': foo' invalid.
      //
      const location nloc (get_location (t, &path_));
      names_type ns (tt != type::colon
                     ? names (t, tt)
                     : names_type ({name ("dir", string ())}));

      if (tt == type::colon)
      {
        scope* old_root (nullptr);
        scope* old_scope (nullptr);

        auto enter_scope = [&old_root, &old_scope, this] (dir_path&& p)
        {
          // Relative scopes are opened relative to out, not src.
          //
          if (p.relative ())
            p = scope_->out_path () / p;

          p.normalize ();

          old_root = root_;
          old_scope = scope_;
          switch_scope (p);
        };

        // If called without the corresponding enter_scope(), then a noop.
        //
        auto leave_scope = [&old_root, &old_scope, this] ()
        {
          if (old_root != nullptr)
          {
            scope_ = old_scope;
            root_ = old_root;

            old_scope = nullptr;
            old_root = nullptr;
          }
        };

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
              if (as != nullptr)
                fail (al) << "attributes before directory scope";

              // Can contain anything that a top level can.
              //
              enter_scope (move (ns[0].dir)); // Steal.
              clause (t, tt);
              leave_scope ();
            }
            else
            {
              if (as != nullptr)
                fail (al) << "attributes before target scope";

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

        // Will have to stash them if later support attributes on
        // target/scope.
        //
        if (as != nullptr)
          fail (al) << "attributes before target/scope";

        al = get_location (t, &path_);
        as = attributes (t, tt);

        if (tt == type::name    ||
            tt == type::lcbrace ||
            tt == type::dollar  ||
            tt == type::lparen  ||
            tt == type::newline ||
            tt == type::eos)
        {
          const location ploc (get_location (t, &path_));
          names_type pns (tt != type::newline && tt != type::eos
                          ? names (t, tt)
                          : names_type ());

          // Common target entering code used in both cases.
          //
          auto enter_target = [this, &nloc, &trace] (name&& tn) -> target&
          {
            const string* e;
            const target_type* ti (scope_->find_target_type (tn, e));

            if (ti == nullptr)
              fail (nloc) << "unknown target type " << tn.type;

            path& d (tn.dir);

            if (d.empty ())
              d = scope_->out_path (); // Already normalized.
            else
            {
              if (d.relative ())
                d = scope_->out_path () / d;

              d.normalize ();
            }

            // Find or insert.
            //
            return targets.insert (
              *ti, move (tn.dir), move (tn.value), e, trace).first;
          };

          // Scope/target-specific variable assignment.
          //
          if (tt == type::assign || tt == type::prepend || tt == type::append)
          {
            token at (t);
            type att (tt);

            const variable_type& var (
              var_pool.find (
                variable_name (move (pns), ploc)));

            // Handle variable attributes.
            //
            if (as != nullptr)
              variable_attribute (var, *as, al);

            // If we have multiple targets/scopes, then we save the value
            // tokens when parsing the first one and then replay them for
            // the subsequent. We have to do it this way because the value
            // may contain variable expansions that would be sensitive to
            // the target/scope context in which they are evaluated.
            //
            replay_guard rg (*this, ns.size () > 1);

            for (name& n: ns)
            {
              if (n.qualified ())
                fail (nloc) << "project name in scope/target " << n;

              if (n.directory ())
              {
                // Scope variable.
                //
                enter_scope (move (n.dir));
                variable (t, tt, var, att);
                leave_scope ();
              }
              else
              {
                // Figure out if this is a target or type/pattern-specific
                // variable.
                //
                size_t p (n.value.find ('*'));

                if (p == string::npos)
                {
                  target* ot (target_);
                  target_ = &enter_target (move (n));
                  variable (t, tt, var, att);
                  target_ = ot;
                }
                else
                {
                  // See tests/variable/type-pattern.
                  //
                  if (n.value.find ('*', p + 1) != string::npos)
                    fail (nloc) << "multiple wildcards in target type/pattern "
                                << n;

                  // If we have the directory, then it is the scope.
                  //
                  if (!n.dir.empty ())
                    enter_scope (move (n.dir));

                  // Resolve target type. If none is specified, use the root
                  // of the hierarchy.
                  //
                  const target_type* ti (
                    n.untyped ()
                    ? &target::static_type
                    : scope_->find_target_type (n.type));

                  if (ti == nullptr)
                    fail (nloc) << "unknown target type " << n.type;

                  if (att == type::prepend)
                    fail (at) << "prepend to target type/pattern-specific "
                              << "variable " << var.name;

                  if (att == type::append)
                    fail (at) << "append to target type/pattern-specific "
                              << "variable " << var.name;

                  // Note: expanding variables in the value in the context of
                  // the scope.
                  //
                  names_type vns (variable_value (t, tt));
                  value& val (scope_->target_vars[*ti][move (n.value)].assign (
                                var).first);
                  val.assign (move (vns), var);

                  leave_scope ();
                }
              }

              rg.play (); // Replay.
            }
          }
          // Dependency declaration.
          //
          else
          {
            if (as != nullptr)
              fail (al) << "attributes before prerequisites";

            // Prepare the prerequisite list.
            //
            target::prerequisites_type ps;
            ps.reserve (pns.size ());

            for (auto& pn: pns)
            {
              const string* e;
              const target_type* ti (scope_->find_target_type (pn, e));

              if (ti == nullptr)
                fail (ploc) << "unknown target type " << pn.type;

              pn.dir.normalize ();

              // Find or insert.
              //
              prerequisite& p (
                scope_->prerequisites.insert (
                  pn.proj,
                  *ti,
                  move (pn.dir),
                  move (pn.value),
                  e,
                  *scope_,
                  trace).first);

              ps.emplace_back (p);
            }

            for (auto& tn: ns)
            {
              if (tn.qualified ())
                fail (nloc) << "project name in target " << tn;

              target& t (enter_target (move (tn)));

              //@@ OPT: move if last/single target (common cases).
              //
              t.prerequisites.insert (t.prerequisites.end (),
                                      ps.begin (),
                                      ps.end ());

              if (default_target_ == nullptr)
                default_target_ = &t;
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
        const variable_type& var (
          var_pool.find (variable_name (move (ns), nloc)));

        // Handle variable attributes.
        //
        if (as != nullptr)
          variable_attribute (var, *as, al);

        variable (t, tt, var, tt);

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
        if (as != nullptr)
          fail (al) << "standalone attributes";

        next (t, tt);
        continue;
      }

      fail (t) << "unexpected " << t;
    }
  }

  void parser::
  source (token& t, type& tt)
  {
    tracer trace ("parser::source", &path_);

    // The rest should be a list of buildfiles. Parse them as names
    // to get variable expansion and directory prefixes.
    //
    mode (lexer_mode::pairs, '@');
    next (t, tt);
    const location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      if (n.pair || n.qualified () || n.empty () || n.value.empty ())
        fail (l) << "expected buildfile instead of " << n;

      // Construct the buildfile path.
      //
      path p (move (n.dir));
      p /= path (move (n.value));

      // If the path is relative then use the src directory corresponding
      // to the current directory scope.
      //
      if (root_->src_path_ != nullptr && p.relative ())
        p = src_out (scope_->out_path (), *root_) / p;

      p.normalize ();

      try
      {
        ifstream ifs (p.string ());

        if (!ifs.is_open ())
          fail (l) << "unable to open " << p;

        ifs.exceptions (ifstream::failbit | ifstream::badbit);

        l5 ([&]{trace (t) << "entering " << p;});

        enter_buildfile (p);

        const path* op (path_);
        path_ = &p;

        lexer l (ifs, *path_);
        lexer* ol (lexer_);
        lexer_ = &l;

        token t (type::eos, false, 0, 0);
        type tt;
        next (t, tt);
        clause (t, tt);

        if (tt != type::eos)
          fail (t) << "unexpected " << t;

        l5 ([&]{trace (t) << "leaving " << p;});

        lexer_ = ol;
        path_ = op;
      }
      catch (const ifstream::failure&)
      {
        fail (l) << "unable to read buildfile " << p;
      }
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  include (token& t, type& tt)
  {
    tracer trace ("parser::include", &path_);

    if (root_->src_path_ == nullptr)
      fail (t) << "inclusion during bootstrap";

    // The rest should be a list of buildfiles. Parse them as names
    // to get variable expansion and directory prefixes.
    //
    mode (lexer_mode::pairs, '@');
    next (t, tt);
    const location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      if (n.pair || n.qualified () || n.empty ())
        fail (l) << "expected buildfile instead of " << n;

      // Construct the buildfile path. If it is a directory, then append
      // 'buildfile'.
      //
      path p (move (n.dir));
      if (n.value.empty ())
        p /= path ("buildfile");
      else
      {
        bool d (path::traits::is_separator (n.value.back ())
                || n.type == "dir");

        p /= path (move (n.value));
        if (d)
          p /= path ("buildfile");
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
        scope_ = ocs;
        root_ = ors;
        continue;
      }

      try
      {
        ifstream ifs (p.string ());

        if (!ifs.is_open ())
          fail (l) << "unable to open " << p;

        ifs.exceptions (ifstream::failbit | ifstream::badbit);

        l5 ([&]{trace (t) << "entering " << p;});

        enter_buildfile (p);

        const path* op (path_);
        path_ = &p;

        lexer l (ifs, *path_);
        lexer* ol (lexer_);
        lexer_ = &l;

        target* odt (default_target_);
        default_target_ = nullptr;

        token t (type::eos, false, 0, 0);
        type tt;
        next (t, tt);
        clause (t, tt);

        if (tt != type::eos)
          fail (t) << "unexpected " << t;

        process_default_target (t);

        l5 ([&]{trace (t) << "leaving " << p;});

        default_target_ = odt;
        lexer_ = ol;
        path_ = op;
      }
      catch (const ifstream::failure&)
      {
        fail (l) << "unable to read buildfile " << p;
      }

      scope_ = ocs;
      root_ = ors;
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  import (token& t, type& tt)
  {
    tracer trace ("parser::import", &path_);

    if (root_->src_path_ == nullptr)
      fail (t) << "import during bootstrap";

    // General import format:
    //
    // import [<var>=](<project>|<project>/<target>])+
    //
    type at; // Assignment type.
    value* val (nullptr);
    const build2::variable* var (nullptr);

    // We are now in the normal lexing mode and here is the problem: we need
    // to switch to the value mode so that we don't treat certain characters
    // as separators (e.g., + in 'libstdc++'). But at the same time we need
    // to detect if we have the <var>= part. So what we are going to do is
    // switch to the value mode, get the first token, and then re-parse it
    // manually looking for =/=+/+=.
    //
    mode (lexer_mode::pairs, '@');
    next (t, tt);

    // Get attributes, if any (note that here we will go into a nested pairs
    // mode).
    //
    location al (get_location (t, &path_));
    attributes_type* as (attributes (t, tt));

    if (tt == type::name)
    {
      // Split the token into the variable name and value at position (p) of
      // '=', taking into account leading/trailing '+'. The variable name is
      // returned while the token is set to value. If the resulting token
      // value is empty, get the next token. Also set assignment type (at).
      //
      auto split = [&at, &t, &tt, this] (size_t p) -> string
      {
        string& v (t.value);
        size_t e;

        if (p != 0 && v[p - 1] == '+') // +=
        {
          e = p--;
          at = type::append;
        }
        else if (p + 1 != v.size () && v[p + 1] == '+') // =+
        {
          e = p + 1;
          at = type::prepend;
        }
        else // =
        {
          e = p;
          at = type::assign;
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

      if (p != string::npos)
        var = &var_pool.find (split (p));
      //
      // This could still be the 'foo =...' case.
      //
      else if (peek () == type::name)
      {
        const string& v (peeked ().value);
        size_t n (v.size ());

        // We should start with =/+=/=+.
        //
        if (n > 0 &&
            (v[p = 0] == '=' ||
             (n > 1 && v[0] == '+' && v[p = 1] == '=')))
        {
          var = &var_pool.find (t.value);
          next (t, tt); // Get the peeked token.
          split (p);    // Returned name should be empty.
        }
      }
    }

    if (var != nullptr)
    {
      // Handle variable attributes.
      //
      if (as != nullptr)
        variable_attribute (*var, *as, al);

      val = at == type::assign
        ? &scope_->assign (*var)
        : &scope_->append (*var);
    }
    else if (as != nullptr)
      fail (al) << "attributes without variable";

    // The rest should be a list of projects and/or targets. Parse
    // them as names to get variable expansion and directory prefixes.
    //
    const location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      if (n.pair)
        fail (l) << "unexpected pair in import";

      // build2::import() will check the name, if required.
      //
      names_type r (build2::import (*scope_, move (n), l));

      if (val != nullptr)
      {
        if (at == type::assign)
          val->assign (move (r), *var);
        else if (at == type::prepend)
          val->prepend (move (r), *var);
        else
          val->append (move (r), *var);
      }
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  export_ (token& t, type& tt)
  {
    tracer trace ("parser::export", &path_);

    scope* ps (scope_->parent_scope ());

    // This should be temp_scope.
    //
    if (ps == nullptr || ps->out_path () != scope_->out_path ())
      fail (t) << "export outside export stub";

    // The rest is a value. Parse it as names to get variable expansion.
    // build2::import() will check the names, if required.
    //
    mode (lexer_mode::pairs, '@');
    next (t, tt);

    if (tt != type::newline && tt != type::eos)
      export_value_ = names (t, tt);

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  using_ (token& t, type& tt)
  {
    tracer trace ("parser::using", &path_);

    bool optional (t.value.back () == '?');

    if (optional && boot_)
      fail (t) << "optional module in bootstrap";

    // The rest should be a list of module names. Parse them as names
    // to get variable expansion, etc.
    //
    mode (lexer_mode::pairs, '@');
    next (t, tt);
    const location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (auto i (ns.begin ()); i != ns.end (); ++i)
    {
      string n, v;

      if (!i->simple ())
        fail (l) << "module name expected instead of " << *i;

      n = move (i->value);

      if (i->pair)
      {
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
            fail (l) << "invalid version '" << v << "': " << e.what ();
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
          boot_module (n, *root_, l);
        else
          load_module (optional, n, *root_, *scope_, l);
      }
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  static target*
  derived_factory (const target_type& t, dir_path d, string n, const string* e)
  {
    // Pass our type to the base factory so that it can detect that it is
    // being called to construct a derived target. This can be used, for
    // example, to decide whether to "link up" to the group.
    //
    // One exception: if we are derived from a derived target type, the this
    // logic will lead to infinite recursion. In this case get the ultimate
    // base.
    //
    const target_type* bt (t.base);
    for (; bt->factory == &derived_factory; bt = bt->base) ;

    target* r (bt->factory (t, move (d), move (n), e));
    r->derived_type = &t;
    return r;
  }

  constexpr const char derived_ext_var[] = "extension";

  void parser::
  define (token& t, type& tt)
  {
    // define <derived>: <base>
    //
    // See tests/define.
    //
    if (next (t, tt) != type::name)
      fail (t) << "expected name instead of " << t << " in target type "
               << "definition";

    string dn (move (t.value));
    const location dnl (get_location (t, &path_));

    if (next (t, tt) != type::colon)
      fail (t) << "expected ':' instead of " << t << " in target type "
               << "definition";

    next (t, tt);

    if (tt == type::name)
    {
      // Target.
      //
      const string& bn (t.value);
      const target_type* bt (scope_->find_target_type (bn));

      if (bt == nullptr)
        fail (t) << "unknown target type " << bn;

      unique_ptr<target_type> dt (new target_type (*bt));
      dt->base = bt;
      dt->factory = &derived_factory;

      // Override extension derivation function: we most likely don't want
      // to use the same default as our base (think cli: file). But, if our
      // base doesn't use extensions, then most likely neither do we (think
      // foo: alias).
      //
      if (bt->extension != nullptr)
        dt->extension = &target_extension_var<derived_ext_var, nullptr>;

      target_type& rdt (*dt); // Save a non-const reference to the object.

      auto pr (scope_->target_types.emplace (dn, target_type_ref (move (dt))));

      if (!pr.second)
        fail (dnl) << "target type " << dn << " already define in this scope";

      // Patch the alias name to use the map's key storage.
      //
      rdt.name = pr.first->first.c_str ();

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
  if_else (token& t, type& tt)
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

          // Parse as names to get variable expansion, evaluation, etc.
          //
          const location nsl (get_location (t, &path_));
          names_type ns (names (t, tt));

          // Should evaluate to 'true' or 'false'.
          //
          try
          {
            if (ns.size () != 1)
              throw invalid_argument (string ());

            bool e (convert<bool> (move (ns[0])));
            take = (k.back () == '!' ? !e : e);
          }
          catch (const invalid_argument&)
          {
            fail (nsl) << "expected " << k << "-expression to evaluate to "
                       << "'true' or 'false' instead of '" << ns << "'";
          }
        }
      }
      else
        take = !taken;

      if (tt != type::newline)
        fail (t) << "expected newline instead of " << t << " after " << k
                 << (k != "else" ? "-expression" : "");

      if (next (t, tt) != type::lcbrace)
        fail (t) << "expected { instead of " << t << " at the beginning of "
                 << k << "-block";

      if (next (t, tt) != type::newline)
        fail (t) << "expected newline after {";

      next (t, tt);

      if (take)
      {
        clause (t, tt);
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

      // See if we have another el* keyword.
      //
      if (k != "else" && tt == type::name && keyword (t))
      {
        const string& n (t.value);

        if (n == "else" || n == "elif" || n == "elif!")
          continue;
      }

      break;
    }
  }

  void parser::
  print (token& t, type& tt)
  {
    // Parse the rest as names to get variable expansion, etc. Switch to the
    // variable lexing mode so that we don't treat special characters (e.g.,
    // ':') as the end of the names.
    //
    mode (lexer_mode::pairs, '@');
    next (t, tt);
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    cout << ns << endl;

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  string parser::
  variable_name (names_type&& ns, const location& l)
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
  variable (token& t, type& tt, const variable_type& var, type kind)
  {
    names_type vns (variable_value (t, tt));

    if (kind == type::assign)
    {
      value& v (target_ != nullptr
                ? target_->assign (var)
                : scope_->assign (var));
      v.assign (move (vns), var);
    }
    else
    {
      value& v (target_ != nullptr
                ? target_->append (var)
                : scope_->append (var));

      if (kind == type::prepend)
        v.prepend (move (vns), var);
      else
        v.append (move (vns), var);
    }
  }

  names parser::
  variable_value (token& t, type& tt)
  {
    mode (lexer_mode::pairs, '@');
    next (t, tt);
    return (tt != type::newline && tt != type::eos
            ? names (t, tt)
            : names_type ());
  }

  void parser::
  variable_attribute (const variable_type& var,
                      attributes_type& as,
                      const location& al)
  {
    const value_type* type (nullptr);

    for (auto& p: as)
    {
      string& k (p.first);
      string& v (p.second);

      if (const value_type* t =
          k == "bool"      ? &value_traits<bool>::value_type       :
          k == "uint64"    ? &value_traits<uint64_t>::value_type   :
          k == "string"    ? &value_traits<string>::value_type     :
          k == "path"      ? &value_traits<path>::value_type       :
          k == "dir_path"  ? &value_traits<dir_path>::value_type   :
          k == "name"      ? &value_traits<name>::value_type       :
          k == "strings"   ? &value_traits<strings>::value_type    :
          k == "paths"     ? &value_traits<paths>::value_type      :
          k == "dir_paths" ? &value_traits<dir_paths>::value_type  :
          k == "names"     ? &value_traits<names_type>::value_type :
          nullptr)
      {
        if (!v.empty ())
          fail (al) << "value in variable type " << k << ": " << v;

        if (type != nullptr)
          fail (al) << "multiple variable types: " << k << ", " << type->name;

        type = t;
        continue;
      }

      fail (al) << "unknown variable attribute " << k;
    }

    if (type != nullptr)
    {
      if (var.type == nullptr)
        var.type = type;
      else if (var.type != type)
        fail (al) << "changing variable " << var.name << " type from "
                  << var.type->name << " to " << type->name;
    }
  }

  parser::names_type parser::
  eval (token& t, type& tt)
  {
    mode (lexer_mode::eval);
    next (t, tt);

    names_type ns;
    eval_trailer (t, tt, ns);
    return ns;
  }

  void parser::
  eval_trailer (token& t, type& tt, names_type& ns)
  {
    // Note that names() will handle the ( == foo) case since if it gets
    // called, it expects to see a name.
    //
    if (tt != type::rparen)
      names (t, tt, ns);

    switch (tt)
    {
    case type::equal:
    case type::not_equal:
      {
        type op (tt);

        // ==, != are left-associative, so get the rhs name and evaluate.
        //
        next (t, tt);
        names_type rhs (names (t, tt));

        bool r;
        switch (op)
        {
        case type::equal:     r = ns == rhs; break;
        case type::not_equal: r = ns != rhs; break;
        default:              r = false;     assert (false);
        }

        ns.resize (1);
        ns[0] = name (r ? "true" : "false");

        eval_trailer (t, tt, ns);
        break;
      }
    case type::rparen:
      break;
    default:
      fail (t) << "expected ')' instead of " << t;
    }
  }

  parser::attributes_type* parser::
  attributes (token& t, token_type& tt)
  {
    attrs_.clear ();

    if (tt != type::lsbrace)
      return nullptr;

    // Using '@' for key-value pairs would be just too ugly. Seeing that we
    // control what goes into keys/values, let's use a much nicer '='.
    //
    mode (lexer_mode::pairs, '=');
    next (t, tt);

    if (tt != type::rsbrace && tt != type::newline && tt != type::eos)
    {
      const location l (get_location (t, &path_));
      names_type ns (names (t, tt));

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
          try
          {
            v = convert<string> (move (*++i));
          }
          catch (const invalid_argument&)
          {
            fail (l) << "invalid attribute value '" << *i << "'";
          }
        }

        attrs_.emplace_back (move (k), move (v));
      }
    }

    // Manually expire the pairs mode if we haven't reached newline/eos (where
    // it expires automatically).
    //
    if (lexer_->mode () == lexer_mode::pairs)
      lexer_->expire_mode ();

    if (tt != type::rsbrace)
      fail (t) << "expected ']' instead of " << t;

    next (t, tt);

    if (tt == type::newline || tt == type::eos)
      fail (t) << "standalone attributes";

    return &attrs_;
  }

  // Parse names inside {} and handle the following "crosses" (i.e.,
  // {a b}{x y}) if any. Return the number of names added to the list.
  //
  size_t parser::
  names_trailer (token& t, type& tt,
                 names_type& ns,
                 size_t pair,
                 const string* pp,
                 const dir_path* dp,
                 const string* tp)
  {
    next (t, tt); // Get what's after '{'.

    size_t count (ns.size ());
    names (t, tt,
           ns,
           false,
           (pair != 0
            ? pair
            : (ns.empty () || ns.back ().pair ? ns.size () : 0)),
           pp, dp, tp);
    count = ns.size () - count;

    if (tt != type::rcbrace)
      fail (t) << "expected } instead of " << t;

    // See if we have a cross. See tests/names.
    //
    if (peek () == type::lcbrace && !peeked ().separated)
    {
      next (t, tt); // Get '{'.
      const location loc (get_location (t, &path_));

      names_type x; // Parse into a separate list of names.
      names_trailer (t, tt, x, 0, nullptr, nullptr, nullptr);

      if (size_t n = x.size ())
      {
        // Now cross the last 'count' names in 'ns' with 'x'. First we will
        // allocate n - 1 additional sets of last 'count' names in 'ns'.
        //
        size_t b (ns.size () - count); // Start of 'count' names.
        ns.reserve (ns.size () + count * (n - 1));
        for (size_t i (0); i != n - 1; ++i)
          for (size_t j (0); j != count; ++j)
            ns.push_back (ns[b + j]);

        // Now cross each name, this time including the first set.
        //
        for (size_t i (0); i != n; ++i)
        {
          for (size_t j (0); j != count; ++j)
          {
            name& l (ns[b + i * count + j]);
            const name& r (x[i]);

            // Move the project names.
            //
            if (r.proj != nullptr)
            {
              if (l.proj != nullptr)
                fail (loc) << "nested project name " << *r.proj;

              l.proj = r.proj;
            }

            // Merge directories.
            //
            if (!r.dir.empty ())
            {
              if (l.dir.empty ())
                l.dir = move (r.dir);
              else
                l.dir /= r.dir;
            }

            // Figure out the type. As a first step, "promote" the lhs value
            // to type.
            //
            if (!l.value.empty ())
            {
              if (!l.type.empty ())
                fail (loc) << "nested type name " << l.value;

              l.type.swap (l.value);
            }

            if (!r.type.empty ())
            {
              if (!l.type.empty ())
                fail (loc) << "nested type name " << r.type;

              l.type = move (r.type);
            }

            l.value = move (r.value);

            // @@ TODO: need to handle pairs on lhs. I think all that needs
            //    to be done is skip pair's first elements. Maybe also check
            //    that there are no pairs on the rhs. There is just no easy
            //    way to enable the pairs mode to test it, yet.
          }
        }

        count *= n;
      }
    }

    return count;
  }

  void parser::
  names (token& t, type& tt,
         names_type& ns,
         bool chunk,
         size_t pair,
         const string* pp,
         const dir_path* dp,
         const string* tp)
  {
    // If pair is not 0, then it is an index + 1 of the first half of
    // the pair for which we are parsing the second halves, e.g.,
    // a@{b c d{e f} {}}.
    //

    // Buffer that is used to collect the complete name in case of
    // an unseparated variable expansion or eval context, e.g.,
    // 'foo$bar($baz)fox'. The idea is to concatenate all the
    // individual parts in this buffer and then re-inject it into
    // the loop as a single token.
    //
    string concat;

    // Number of names in the last group. This is used to detect when
    // we need to add an empty first pair element (e.g., @y) or when
    // we have a (for now unsupported) multi-name LHS (e.g., {x y}@z).
    //
    size_t count (0);

    for (bool first (true);; first = false)
    {
      // If the accumulating buffer is not empty, then we have two options:
      // continue accumulating or inject. We inject if the next token is
      // not a name, var expansion, or eval context or if it is separated.
      //
      if (!concat.empty () &&
          ((tt != type::name   &&
            tt != type::dollar &&
            tt != type::lparen) || peeked ().separated))
      {
        tt = type::name;
        t = token (move (concat), true, false, t.line, t.column);
        concat.clear ();
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
      if (tt == type::name)
      {
        string name (t.value); //@@ move?
        tt = peek ();

        // Should we accumulate? If the buffer is not empty, then
        // we continue accumulating (the case where we are separated
        // should have been handled by the injection code above). If
        // the next token is a var expansion or eval context and it
        // is not separated, then we need to start accumulating.
        //
        if (!concat.empty () ||                                // Continue.
            ((tt == type::dollar ||
              tt == type::lparen) && !peeked ().separated))    // Start.
        {
          concat += name;
          continue;
        }

        string::size_type p (name.find_last_of ("/%"));

        // First take care of project. A project-qualified name is
        // not very common, so we can afford some copying for the
        // sake of simplicity.
        //
        const string* pp1 (pp);

        if (p != string::npos)
        {
          bool last (name[p] == '%');
          string::size_type p1 (last ? p : name.rfind ('%', p - 1));

          if (p1 != string::npos)
          {
            string proj;
            proj.swap (name);

            // First fix the rest of the name.
            //
            name.assign (proj, p1 + 1, string::npos);
            p = last ? string::npos : p - (p1 + 1);

            // Now process the project name.
            // @@ Validate it.
            //
            proj.resize (p1);

            if (pp != nullptr)
              fail (t) << "nested project name " << proj;

            pp1 = &project_name_pool.find (proj);
          }
        }

        string::size_type n (p != string::npos ? name.size () - 1 : 0);

        // See if this is a type name, directory prefix, or both. That
        // is, it is followed by an un-separated '{'.
        //
        if (tt == type::lcbrace && !peeked ().separated)
        {
          next (t, tt);

          if (p != n && tp != nullptr)
            fail (t) << "nested type name " << name;

          dir_path d1;
          const dir_path* dp1 (dp);

          string t1;
          const string* tp1 (tp);

          if (p == string::npos) // type
            tp1 = &name;
          else if (p == n) // directory
          {
            if (dp == nullptr)
              d1 = dir_path (name);
            else
              d1 = *dp / dir_path (name);

            dp1 = &d1;
          }
          else // both
          {
            t1.assign (name, p + 1, n - p);

            if (dp == nullptr)
              d1 = dir_path (name, 0, p + 1);
            else
              d1 = *dp / dir_path (name, 0, p + 1);

            dp1 = &d1;
            tp1 = &t1;
          }

          count = names_trailer (t, tt, ns, pair, pp1, dp1, tp1);
          tt = peek ();
          continue;
        }

        // If we are a second half of a pair, add another first half
        // unless this is the first instance.
        //
        if (pair != 0 && pair != ns.size ())
          ns.push_back (ns[pair - 1]);

        count = 1;

        // If it ends with a directory separator, then it is a directory.
        // Note that at this stage we don't treat '.' and '..' as special
        // (unless they are specified with a directory separator) because
        // then we would have ended up treating '.: ...' as a directory
        // scope. Instead, this is handled higher up the processing chain,
        // in target_types::find(). This would also mess up reversibility
        // to simple name.
        //
        // @@ TODO: and not quoted
        //
        if (p == n)
        {
          // For reversibility to simple name, only treat it as a directory
          // if the string is an exact representation.
          //
          if (p != 0 && name[p - 1] != '/') // Take care of the "//" case.
            name.resize (p); // Strip trailing '/'.

          dir_path dir (move (name), dir_path::exact);

          if (!dir.empty ())
          {
            if (dp != nullptr)
              dir = *dp / dir;

            ns.emplace_back (pp1,
                             move (dir),
                             (tp != nullptr ? *tp : string ()),
                             string ());
            continue;
          }

          // Add the trailing slash back and treat it as a simple name.
          //
          if (p != 0 && name[p - 1] != '/')
            name.push_back ('/');
        }

        ns.emplace_back (pp1,
                         (dp != nullptr ? *dp : dir_path ()),
                         (tp != nullptr ? *tp : string ()),
                         move (name));
        continue;
      }

      // Variable expansion/function call or eval context.
      //
      if (tt == type::dollar || tt == type::lparen)
      {
        // These two cases are pretty similar in that in both we
        // pretty quickly end up with a list of names that we need
        // to splice into the result.
        //
        names_type lv_storage;
        names_view lv;

        location loc;
        const char* what; // Variable or evaluation context.

        if (tt == type::dollar)
        {
          // Switch to the variable name mode. We want to use this
          // mode for $foo but not for $(foo). Since we don't know
          // whether the next token is a paren or a name, we turn
          // it on and switch to the eval mode if what we get next
          // is a paren.
          //
          mode (lexer_mode::variable);
          next (t, tt);
          loc = get_location (t, &path_);

          string n;
          if (tt == type::name)
            n = t.value;
          else if (tt == type::lparen)
          {
            expire_mode ();
            names_type ns (eval (t, tt));

            // Make sure the result of evaluation is a single, simple name.
            //
            if (ns.size () != 1 || !ns.front ().simple ())
              fail (loc) << "variable/function name expected instead of '"
                         << ns << "'";

            n = move (ns.front ().value);
          }
          else
            fail (t) << "variable/function name expected instead of " << t;

          if (n.empty ())
            fail (loc) << "empty variable/function name";

          // Figure out whether this is a variable expansion or a function
          // call.
          //
          tt = peek ();

          if (tt == type::lparen)
          {
            next (t, tt); // Get '('.
            names_type ns (eval (t, tt));

            // Just a stub for now.
            //
            cout << n << "(" << ns << ")" << endl;

            tt = peek ();

            if (lv_storage.empty ())
              continue;

            lv = lv_storage;
            what = "function call";
          }
          else
          {
            // Process variable name.
            //
            if (n.front () == '.') // Fully qualified name.
              n.erase (0, 1);
            else
            {
              //@@ TODO: append namespace if any.
            }

            // Lookup.
            //
            const auto& var (var_pool.find (move (n)));
            auto l (target_ != nullptr ? (*target_)[var] : (*scope_)[var]);

            // Undefined/NULL namespace variables are not allowed.
            //
            if (!l && var.name.find ('.') != string::npos)
              fail (loc) << "undefined/null namespace variable " << var.name;

            if (!l || l->empty ())
              continue;

            lv = reverse (*l, lv_storage);
            what = "variable expansion";
          }
        }
        else
        {
          loc = get_location (t, &path_);
          lv_storage = eval (t, tt);

          tt = peek ();

          if (lv_storage.empty ())
            continue;

          lv = lv_storage;
          what = "context evaluation";
        }

        // @@ Could move if lv is lv_storage.
        //

        // Should we accumulate? If the buffer is not empty, then
        // we continue accumulating (the case where we are separated
        // should have been handled by the injection code above). If
        // the next token is a name or var expansion and it is not
        // separated, then we need to start accumulating.
        //
        if (!concat.empty () ||                       // Continue.
            ((tt == type::name   ||                   // Start.
              tt == type::dollar ||
              tt == type::lparen) && !peeked ().separated))
        {
          // This should be a simple value or a simple directory. The
          // token still points to the name (or closing paren).
          //
          if (lv.size () > 1)
            fail (loc) << "concatenating " << what << " contains multiple "
                       << "values";

          const name& n (lv[0]);

          if (n.qualified ())
            fail (loc) << "concatenating " << what << " contains project name";

          if (n.typed ())
            fail (loc) << "concatenating " << what << " contains type";

          if (!n.dir.empty ())
          {
            if (!n.value.empty ())
              fail (loc) << "concatenating " << what << " contains directory";

            concat += n.dir.string ();
          }
          else
            concat += n.value;
        }
        else
        {
          // Copy the names from the variable into the resulting name list
          // while doing sensible things with the types and directories.
          //
          for (const name& n: lv)
          {
            const string* pp1 (pp);
            const dir_path* dp1 (dp);
            const string* tp1 (tp);

            if (n.proj != 0)
            {
              if (pp == nullptr)
                pp1 = n.proj;
              else
                fail (loc) << "nested project name " << *n.proj << " in "
                           << what;
            }

            dir_path d1;
            if (!n.dir.empty ())
            {
              if (dp != nullptr)
              {
                if (n.dir.absolute ())
                  fail (loc) << "nested absolute directory " << n.dir
                             << " in " << what;

                d1 = *dp / n.dir;
                dp1 = &d1;
              }
              else
                dp1 = &n.dir;
            }

            if (!n.type.empty ())
            {
              if (tp == nullptr)
                tp1 = &n.type;
              else
                fail (loc) << "nested type name " << n.type << " in " << what;
            }

            // If we are a second half of a pair.
            //
            if (pair != 0)
            {
              // Check that there are no nested pairs.
              //
              if (n.pair)
                fail (loc) << "nested pair in " << what;

              // And add another first half unless this is the first instance.
              //
              if (pair != ns.size ())
                ns.push_back (ns[pair - 1]);
            }

            ns.emplace_back (pp1,
                             (dp1 != nullptr ? *dp1 : dir_path ()),
                             (tp1 != nullptr ? *tp1 : string ()),
                             n.value);

            ns.back ().pair = n.pair;
          }

          count = lv.size ();
        }

        continue;
      }

      // Untyped name group without a directory prefix, e.g., '{foo bar}'.
      //
      if (tt == type::lcbrace)
      {
        count = names_trailer (t, tt, ns, pair, pp, dp, tp);
        tt = peek ();
        continue;
      }

      // A pair separator (only in the pairs mode).
      //
      if (tt == type::pair_separator)
      {
        if (pair != 0)
          fail (t) << "nested pair on the right hand side of a pair";

        // Catch '@@'. Maybe we can use for something later (e.g., escaping).
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
          fail (t) << "multiple names on the left hand side of a pair";

        ns.back ().pair = true;
        tt = peek ();

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

        continue;
      }

      if (!first)
        break;

      if (tt == type::rcbrace) // Empty name, e.g., dir{}.
      {
        // If we are a second half of a pair, add another first half
        // unless this is the first instance.
        //
        if (pair != 0 && pair != ns.size ())
          ns.push_back (ns[pair - 1]);

        ns.emplace_back (pp,
                         (dp != nullptr ? *dp : dir_path ()),
                         (tp != nullptr ? *tp : string ()),
                         string ());
        break;
      }
      else
        // Our caller expected this to be a name.
        //
        fail (t) << "expected name instead of " << t;
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
    assert (t.type == type::name);

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
    if (!t.quoted)
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
  // application rather than the eval context. At the same time we want
  // to use names() to parse names, get variable expansion/function calls,
  // quoting, etc. We just need to disable the eval context. The way this
  // is done has two parts: Firstly, we parse names in chunks and detect
  // and handle the opening paren. In other words, a buildspec like
  // 'clean (./)' is "chunked" as 'clean', '(', etc. While this is fairly
  // straightforward, there is one snag: concatenating eval contexts, as
  // in 'clean(./)'. Normally, this will be treated as a single chunk and
  // we don't want that. So here comes the trick (or hack, if you like):
  // we will make every opening paren token "separated" (i.e., as if it
  // was proceeded by a space). This will disable concatenating eval. In
  // fact, we will even go a step further and only do this if we are in
  // the original pairs mode. This will allow us to still use eval
  // contexts in buildspec, provided that we quote it: '"cle(an)"'. Note
  // also that function calls still work as usual: '$filter (clean test)'.
  // To disable a function call and make it instead a var that is expanded
  // into operation name(s), we can use quoting: '"$ops"(./)'.
  //
  static void
  paren_processor (token& t, const lexer& l)
  {
    if (t.type == type::lparen && l.mode () == lexer_mode::pairs)
      t.separated = true;
  }

  buildspec parser::
  parse_buildspec (istream& is, const path& name)
  {
    path_ = &name;

    lexer l (is, *path_, &paren_processor);
    lexer_ = &l;
    target_ = nullptr;
    scope_ = root_ = global_scope;

    // Turn on pairs recognition with '@' as the pair separator (e.g.,
    // src_root/@out_root/exe{foo bar}).
    //
    mode (lexer_mode::pairs, '@');

    token t (type::eos, false, 0, 0);
    type tt;
    next (t, tt);

    return buildspec_clause (t, tt, type::eos);
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
      if (c != '_' && !(i != 0 ? isalnum (c) : isalpha (c)))
        return false;
    }

    return true;
  }

  buildspec parser::
  buildspec_clause (token& t, type& tt, type tt_end)
  {
    buildspec bs;

    while (tt != tt_end)
    {
      // We always start with one or more names. Eval context
      // (lparen) only allowed if quoted.
      //
      if (tt != type::name    &&
          tt != type::lcbrace &&      // Untyped name group: '{foo ...'
          tt != type::dollar  &&      // Variable expansion: '$foo ...'
          !(tt == type::lparen && mode () == lexer_mode::quoted) &&
          tt != type::pair_separator) // Empty pair LHS: '@foo ...'
        fail (t) << "operation or target expected instead of " << t;

      const location l (get_location (t, &path_)); // Start of names.

      // This call will parse the next chunk of output and produce
      // zero or more names.
      //
      names_type ns (names (t, tt, true));

      // What these names mean depends on what's next. If it is an
      // opening paren, then they are operation/meta-operation names.
      // Otherwise they are targets.
      //
      if (tt == type::lparen) // Peeked into by names().
      {
        if (ns.empty ())
          fail (t) << "operation name expected before '('";

        for (const name& n: ns)
          if (!opname (n))
            fail (l) << "operation name expected instead of '" << n << "'";

        // Inside '(' and ')' we have another, nested, buildspec.
        //
        next (t, tt);
        const location l (get_location (t, &path_)); // Start of nested names.
        buildspec nbs (buildspec_clause (t, tt, type::rparen));

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

  void parser::
  switch_scope (const dir_path& p)
  {
    tracer trace ("parser::switch_scope", &path_);

    // First, enter the scope into the map and see if it is in any
    // project. If it is not, then there is nothing else to do.
    //
    auto i (scopes.insert (p, nullptr, true, false));
    scope_ = i->second;
    scope* rs (scope_->root_scope ());

    if (rs == nullptr)
      return;

    // Path p can be src_base or out_base. Figure out which one it is.
    //
    dir_path out_base (p.sub (rs->out_path ()) ? p : src_out (p, *rs));

    // Create and bootstrap root scope(s) of subproject(s) that this
    // scope may belong to. If any were created, load them. Note that
    // we need to do this before figuring out src_base since we may
    // switch the root project (and src_root with it).
    //
    {
      scope* nrs (&create_bootstrap_inner (*rs, out_base));

      if (rs != nrs)
      {
        load_root_pre (*nrs); // Load outer roots recursively.
        rs = nrs;
      }
    }

    // Switch to the new root scope.
    //
    if (rs != root_)
    {
      l5 ([&]{trace << "switching to root scope " << rs->out_path ();});
      root_ = rs;
    }

    // Now we can figure out src_base and finish setting the scope.
    //
    dir_path src_base (src_out (out_base, *rs));
    setup_base (i, move (out_base), move (src_base));
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
                      "",
                      nullptr,
                      trace) != targets.end ())
      return;

    target& dt (*default_target_);

    l5 ([&]{trace (t) << "creating current directory alias for " << dt;});

    target& ct (
      targets.insert (
        dir::static_type, scope_->out_path (), "", nullptr, trace).first);

    prerequisite& p (
      scope_->prerequisites.insert (
        nullptr,
        dt.type (),
        dt.dir,
        dt.name,
        dt.ext,
        *scope_, // Doesn't matter which scope since dir is absolute.
        trace).first);

    p.target = &dt;
    ct.prerequisites.emplace_back (p);
  }

  void parser::
  enter_buildfile (const path& p)
  {
    tracer trace ("parser::enter_buildfile", &path_);

    const char* e (p.extension ());
    targets.insert<buildfile> (
      p.directory (),
      p.leaf ().base ().string (),
      &extension_pool.find (e == nullptr ? "" : e), // Always specified.
      trace);
  }

  type parser::
  next (token& t, type& tt)
  {
    if (peeked_)
    {
      t = move (peek_);
      peeked_ = false;
    }
    else
      t = (replay_ == replay::play ? replay_next () : lexer_->next ());

    if (replay_ == replay::save)
      replay_data_.push_back (t);

    tt = t.type;
    return tt;
  }

  type parser::
  peek ()
  {
    if (!peeked_)
    {
      peek_ = (replay_ == replay::play ? replay_next () : lexer_->next ());
      peeked_ = true;
    }

    return peek_.type;
  }

  static location
  get_location (const token& t, const void* data)
  {
    assert (data != nullptr); // &parser::path_
    const path* p (*static_cast<const path* const*> (data));
    return location (p, t.line, t.column);
  }
}
