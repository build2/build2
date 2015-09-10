// file      : build/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/parser>

#include <cctype>   // is{alpha alnum}()

#include <memory>   // unique_ptr
#include <fstream>
#include <utility>  // move()
#include <iterator> // make_move_iterator()
#include <iostream>

#include <build/token>
#include <build/lexer>

#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/variable>
#include <build/module>
#include <build/file>
#include <build/diagnostics>
#include <build/context>

using namespace std;

namespace build
{
  static location
  get_location (const token&, const void*);

  typedef token_type type;

  void parser::
  parse_buildfile (istream& is, const path& p, scope& root, scope& base)
  {
    enter_buildfile (p);

    string rw (diag_relative (p)); // Relative to work.
    path_ = &rw;

    lexer l (is, rw);
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
  parse_variable (lexer& l, scope& s, string name, token_type kind)
  {
    path_ = &l.name ();
    lexer_ = &l;
    target_ = nullptr;
    scope_ = &s;

    type tt;
    token t (type::eos, false, 0, 0);
    variable (t, tt, name, kind);
    return t;
  }

  void parser::
  clause (token& t, token_type& tt)
  {
    tracer trace ("parser::clause", &path_);

    while (tt != type::eos)
    {
      // We always start with one or more names.
      //
      if (tt != type::name    &&
          tt != type::lcbrace && // Untyped name group: '{foo ...'
          tt != type::dollar  && // Variable expansion: '$foo ...'
          tt != type::lparen  && // Eval context: '(foo) ...'
          tt != type::colon)     // Empty name: ': ...'
        break; // Something else. Let our caller handle that.

      // See if this is one of the directives. This should be an
      // unquoted literal name.
      //
      if (tt == type::name && !t.quoted)
      {
        const string& n (t.value);

        if (n == "print")
        {
          // @@ Is this the only place where it is valid? Probably also
          // in var namespace.
          //
          print (t, tt);
          continue;
        }
        else if (n == "source")
        {
          source (t, tt);
          continue;
        }
        else if (n == "include")
        {
          include (t, tt);
          continue;
        }
        else if (n == "import")
        {
          import (t, tt);
          continue;
        }
        else if (n == "export")
        {
          export_ (t, tt);
          continue;
        }
        else if (n == "using")
        {
          using_ (t, tt);
          continue;
        }
      }

      // ': foo' is equvalent to '{}: foo' and to 'dir{}: foo'.
      //
      const location nloc (get_location (t, &path_));
      names_type ns (tt != type::colon
                     ? names (t, tt)
                     : names_type ({name ("dir", string ())}));

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
              // A name represents directory as an empty value.
              //
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
              dir_path p (move (ns[0].dir)); // Steal.

              // Relative scopes are opened relative to out, not src.
              //
              if (p.relative ())
                p = scope_->out_path () / p;

              p.normalize ();

              scope* ors (root_);
              scope* ocs (scope_);
              switch_scope (p);

              // A directory scope can contain anything that a top level can.
              //
              clause (t, tt);

              scope_ = ocs;
              root_ = ors;
            }
            else
            {
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
          // prerequisites.
          //
        }

        // Dependency declaration or scope/target-specific variable
        // assignment.
        //
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
          if (tt == type::equal || tt == type::plus_equal)
          {
            string var (variable_name (move (pns), ploc));

            // Enter the target/scope and set it as current.
            //
            if (ns.size () != 1)
              fail (nloc) << "multiple names in scope/target-specific "
                          << "variable assignment";

            name& n (ns[0]);

            if (n.qualified ())
              fail (nloc) << "project name in scope/target " << n;

            if (n.directory ())
            {
              // The same code as in directory scope handling code above.
              //
              dir_path p (move (n.dir));

              if (p.relative ())
                p = scope_->out_path () / p;

              p.normalize ();

              scope* ors (root_);
              scope* ocs (scope_);
              switch_scope (p);

              variable (t, tt, move (var), tt);

              scope_ = ocs;
              root_ = ors;
            }
            else
            {
              target* ot (target_);
              target_ = &enter_target (move (n));

              variable (t, tt, move (var), tt);

              target_ = ot;
            }
          }
          // Dependency declaration.
          //
          else
          {
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
      if (tt == type::equal || tt == type::plus_equal)
      {
        variable (t, tt, variable_name (move (ns), nloc), tt);

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
        next (t, tt);
        continue;
      }

      fail (t) << "unexpected " << t;
    }
  }

  void parser::
  source (token& t, token_type& tt)
  {
    tracer trace ("parser::source", &path_);

    // The rest should be a list of buildfiles. Parse them as names
    // to get variable expansion and directory prefixes.
    //
    next (t, tt);
    const location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      if (n.qualified () || n.empty () || n.value.empty ())
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

      ifstream ifs (p.string ());

      if (!ifs.is_open ())
        fail (l) << "unable to open " << p;

      ifs.exceptions (ifstream::failbit | ifstream::badbit);

      level5 ([&]{trace (t) << "entering " << p;});

      enter_buildfile (p);

      string rw (diag_relative (p)); // Relative to work.
      const string* op (path_);
      path_ = &rw;

      lexer l (ifs, rw);
      lexer* ol (lexer_);
      lexer_ = &l;

      token t (type::eos, false, 0, 0);
      type tt;
      next (t, tt);
      clause (t, tt);

      if (tt != type::eos)
        fail (t) << "unexpected " << t;

      level5 ([&]{trace (t) << "leaving " << p;});

      lexer_ = ol;
      path_ = op;
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  include (token& t, token_type& tt)
  {
    tracer trace ("parser::include", &path_);

    if (root_->src_path_ == nullptr)
      fail (t) << "inclusion during bootstrap";

    // The rest should be a list of buildfiles. Parse them as names
    // to get variable expansion and directory prefixes.
    //
    next (t, tt);
    const location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      if (n.qualified () || n.empty ())
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

      if (!root_->buildfiles.insert (p).second) // Note: may be "new" root.
      {
        level5 ([&]{trace (l) << "skipping already included " << p;});
        scope_ = ocs;
        root_ = ors;
        continue;
      }

      ifstream ifs (p.string ());

      if (!ifs.is_open ())
        fail (l) << "unable to open " << p;

      ifs.exceptions (ifstream::failbit | ifstream::badbit);

      level5 ([&]{trace (t) << "entering " << p;});

      enter_buildfile (p);

      string rw (diag_relative (p)); // Relative to work.
      const string* op (path_);
      path_ = &rw;

      lexer l (ifs, rw);
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

      level5 ([&]{trace (t) << "leaving " << p;});

      default_target_ = odt;
      lexer_ = ol;
      path_ = op;

      scope_ = ocs;
      root_ = ors;
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  import (token& t, token_type& tt)
  {
    tracer trace ("parser::import", &path_);

    if (root_->src_path_ == nullptr)
      fail (t) << "import during bootstrap";

    next (t, tt);

    // General import format:
    //
    // import [<var>=](<project>|<project>/<target>])+
    //
    value* val (nullptr);
    const build::variable* var (nullptr);

    token_type at; // Assignment type.
    if (tt == type::name)
    {
      at = peek ();

      if (at == token_type::equal || at == token_type::plus_equal)
      {
        var = &variable_pool.find (t.value);
        val = at == token_type::equal
          ? &scope_->assign (*var)
          : &scope_->append (*var);
        next (t, tt); // Consume =/+=.
        lexer_->mode (lexer_mode::value);
        next (t, tt);
      }
    }

    // The rest should be a list of projects and/or targets. Parse
    // them as names to get variable expansion and directory prefixes.
    //
    const location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      // build::import() will check the name, if required.
      //
      names_type r (build::import (*scope_, move (n), l));

      if (val != nullptr)
      {
        if (at == token_type::equal)
          val->assign (move (r), *var);
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
  export_ (token& t, token_type& tt)
  {
    tracer trace ("parser::export", &path_);

    scope* ps (scope_->parent_scope ());

    // This should be temp_scope.
    //
    if (ps == nullptr || ps->out_path () != scope_->out_path ())
      fail (t) << "export outside export stub";

    // The rest is a value. Parse it as names to get variable expansion.
    // build::import() will check the names, if required.
    //
    lexer_->mode (lexer_mode::value);
    next (t, tt);

    if (tt != type::newline && tt != type::eos)
      export_value_ = names (t, tt);

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  using_ (token& t, token_type& tt)
  {
    tracer trace ("parser::using", &path_);

    // The rest should be a list of module names. Parse them as names
    // to get variable expansion, etc.
    //
    next (t, tt);
    const location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      // For now it should be a simple name.
      //
      if (!n.simple ())
        fail (l) << "module name expected instead of " << n;

      load_module (n.value, *root_, *scope_, l);
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  print (token& t, token_type& tt)
  {
    // Parse the rest as names to get variable expansion, etc. Switch
    // to the variable value lexing mode so that we don't treat special
    // characters (e.g., ':') as the end of the names.
    //
    lexer_->mode (lexer_mode::value);

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
  variable (token& t, token_type& tt, string name, token_type kind)
  {
    bool assign (kind == type::equal);
    const auto& var (variable_pool.find (move (name)));

    if (var.pairs != '\0')
      lexer_->mode (lexer_mode::pairs, var.pairs);
    else
      lexer_->mode (lexer_mode::value);

    next (t, tt);
    names_type vns (tt != type::newline && tt != type::eos
                    ? names (t, tt)
                    : names_type ());
    if (assign)
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
      v.append (move (vns), var);
    }
  }

  parser::names_type parser::
  eval (token& t, token_type& tt)
  {
    lexer_->mode (lexer_mode::eval);
    next (t, tt);

    names_type ns (tt != type::rparen ? names (t, tt) : names_type ());

    if (tt != type::rparen)
      fail (t) << "expected ')' instead of " << t;

    return ns;
  }

  void parser::
  names (token& t,
         type& tt,
         names_type& ns,
         bool chunk,
         size_t pair,
         const std::string* pp,
         const dir_path* dp,
         const string* tp)
  {
    // If pair is not 0, then it is an index + 1 of the first half of
    // the pair for which we are parsing the second halves, e.g.,
    // a={b c d{e f} {}}.
    //

    // Buffer that is used to collect the complete name in case of
    // an unseparated variable expansion or eval context, e.g.,
    // 'foo$bar($baz)fox'. The idea is to concatenate all the
    // individual parts in this buffer and then re-inject it into
    // the loop as a single token.
    //
    string concat;

    // Number of names in the last group. This is used to detect when
    // we need to add an empty first pair element (e.g., {=y}) or when
    // we have a for now unsupported multi-name LHS (e.g., {x y}=z).
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
        // If we are chunking, stop at the next separated token. Unless
        // current or next token is a pair separator, since we want the
        // "x = y" pair to be parsed as a single chunk.
        //
        if (chunk &&
            peeked ().separated &&
            (tt != type::pair_separator && t.type != type::pair_separator))
          break;

        next (t, tt);
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
        // is, it is followed by '{'.
        //
        if (tt == type::lcbrace)
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

          next (t, tt);
          count = ns.size ();
          names (t, tt,
                 ns,
                 false,
                 (pair != 0
                  ? pair
                  : (ns.empty () || ns.back ().pair == '\0' ? 0 : ns.size ())),
                 pp1, dp1, tp1);
          count = ns.size () - count;

          if (tt != type::rcbrace)
            fail (t) << "expected } instead of " << t;

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
        names_type lv_data;
        const names_type* plv;

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
          lexer_->mode (lexer_mode::variable);
          next (t, tt);
          loc = get_location (t, &path_);

          string n;
          if (tt == type::name)
            n = t.value;
          else if (tt == type::lparen)
          {
            lexer_->expire_mode ();
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

          // Figure out whether this is a variable expansion of a function
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

            if (lv_data.empty ())
              continue;

            plv = &lv_data;
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
            const auto& var (variable_pool.find (move (n)));
            auto l (target_ != nullptr ? (*target_)[var] : (*scope_)[var]);

            // Undefined/NULL namespace variables are not allowed.
            //
            if (!l && var.name.find ('.') != string::npos)
              fail (loc) << "undefined/null namespace variable " << var.name;

            if (!l || l->empty ())
              continue;

            plv = &l->data_;
            what = "variable expansion";
          }
        }
        else
        {
          loc = get_location (t, &path_);
          lv_data = eval (t, tt);

          tt = peek ();

          if (lv_data.empty ())
            continue;

          plv = &lv_data;
          what = "context evaluation";
        }

        // @@ Could move if (lv == &lv_data).
        //
        const names_type& lv (*plv);

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
              if (n.pair != '\0')
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
          }

          count = lv.size ();
        }

        continue;
      }

      // Untyped name group without a directory prefix, e.g., '{foo bar}'.
      //
      if (tt == type::lcbrace)
      {
        next (t, tt);
        count = ns.size ();
        names (t, tt,
               ns,
               false,
               (pair != 0
                ? pair
                : (ns.empty () || ns.back ().pair == '\0' ? 0 : ns.size ())),
               pp, dp, tp);
        count = ns.size () - count;

        if (tt != type::rcbrace)
          fail (t) << "expected } instead of " << t;

        tt = peek ();
        continue;
      }

      // A pair separator (only in the pairs mode).
      //
      if (tt == type::pair_separator)
      {
        if (pair != 0)
          fail (t) << "nested pair on the right hand side of a pair";

        if (count > 1)
          fail (t) << "multiple names on the left hand side of a pair";

        if (count == 0)
        {
          // Empty LHS, (e.g., {=y}), create an empty name.
          //
          ns.emplace_back (pp,
                           (dp != nullptr ? *dp : dir_path ()),
                           (tp != nullptr ? *tp : string ()),
                           string ());
          count = 1;
        }

        ns.back ().pair = lexer_->pair_separator ();
        tt = peek ();
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

    // Handle the empty RHS in a pair, (e.g., {y=}).
    //
    if (!ns.empty () && ns.back ().pair != '\0')
    {
      ns.emplace_back (pp,
                       (dp != nullptr ? *dp : dir_path ()),
                       (tp != nullptr ? *tp : string ()),
                       string ());
    }
  }

  // Buildspec parsing.
  //

  buildspec parser::
  parse_buildspec (istream& is, const std::string& name)
  {
    path_ = &name;

    lexer l (is, name);
    lexer_ = &l;
    target_ = nullptr;
    scope_ = root_ = global_scope;

    // Turn on pairs recognition with '@' as the pair separator (e.g.,
    // src_root/@out_root/exe{foo bar}).
    //
    lexer_->mode (lexer_mode::pairs, '@');

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
    if (n.pair != '\0' || !n.simple () || n.empty ())
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
  buildspec_clause (token& t, token_type& tt, token_type tt_end)
  {
    buildspec bs;

    while (tt != tt_end)
    {
      // We always start with one or more names. No eval context
      // support for the time being.
      //
      if (tt != type::name    &&
          tt != type::lcbrace &&      // Untyped name group: '{foo ...'
          tt != type::dollar  &&      // Variable expansion: '$foo ...'
          tt != type::pair_separator) // Empty pair LHS: '=foo ...'
        fail (t) << "operation or target expected instead of " << t;

      const location l (get_location (t, &path_)); // Start of names.

      // This call will produce zero or more names and should stop
      // at either tt_end or '('.
      //
      names_type ns (names (t, tt));
      size_t targets (ns.size ());

      if (tt == type::lparen)
      {
        if (targets == 0 || !opname (ns.back ()))
          fail (t) << "operation name expected before (";

        targets--; // Last one is an operation name.
      }

      // Group all the targets into a single operation. In other
      // words, 'foo bar' is equivalent to 'build(foo bar)'.
      //
      if (targets != 0)
      {
        if (bs.empty () || !bs.back ().name.empty ())
          bs.push_back (metaopspec ()); // Empty (default) meta operation.

        metaopspec& ms (bs.back ());

        for (auto i (ns.begin ()), e (i + targets); i != e; ++i)
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
            if (i->pair != '\0')
            {
              if (i->typed ())
                fail (l) << "expected target src_base instead of " << *i;

              src_base = move (i->dir);

              if (!i->value.empty ())
                src_base /= dir_path (move (i->value));

              ++i;
              assert (i != e);
            }

            if (ms.empty () || !ms.back ().name.empty ())
              ms.push_back (opspec ()); // Empty (default) operation.

            opspec& os (ms.back ());
            os.emplace_back (move (src_base), move (*i));
          }
        }
      }

      // Handle the operation.
      //
      if (tt == type::lparen)
      {
        // Inside '(' and ')' we have another buildspec.
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
          if (!nms.name.empty ())
            fail (l) << "nested meta-operation " << nms.name;

          if (!meta)
          {
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
        metaopspec& nmo (nbs.back ());

        if (meta)
        {
          nmo.name = move (ns.back ().value);
          bs.push_back (move (nmo));
        }
        else
        {
          // Since we are not a meta-operation, the nested buildspec
          // should be just a bunch of targets.
          //
          assert (nmo.size () == 1);
          opspec& nos (nmo.back ());

          if (bs.empty () || !bs.back ().name.empty ())
            bs.push_back (metaopspec ()); // Empty (default) meta operation.

          nos.name = move (ns.back ().value);
          bs.back ().push_back (move (nos));
        }

        next (t, tt); // Done with ')'.
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
      level5 ([&]{trace << "switching to root scope " << rs->out_path ();});
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

    level5 ([&]{trace (t) << "creating current directory alias for " << dt;});

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

  token_type parser::
  next (token& t, token_type& tt)
  {
    if (!peeked_)
      t = lexer_->next ();
    else
    {
      t = move (peek_);
      peeked_ = false;
    }

    tt = t.type;
    return tt;
  }

  token_type parser::
  peek ()
  {
    if (!peeked_)
    {
      peek_ = lexer_->next ();
      peeked_ = true;
    }

    return peek_.type;
  }

  static location
  get_location (const token& t, const void* data)
  {
    assert (data != nullptr);
    const string& p (**static_cast<const string* const*> (data));
    return location (p.c_str (), t.line, t.column);
  }
}
