// file      : build/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
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
    string rw (diag_relative (p)); // Relative to work.
    path_ = &rw;

    lexer l (is, rw);
    lexer_ = &l;
    scope_ = &base;
    root_ = nullptr;
    switch_root (&root);
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
    scope_ = &s;

    token t (type::eos, false, 0, 0);
    type tt;
    next (t, tt);

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
          tt != type::colon)     // Empty name: ': ...'
        break; // Something else. Let our caller handle that.

      // See if this is one of the keywords.
      //
      if (tt == type::name)
      {
        const string& n (t.name ());

        if (n == "print")
        {
          // @@ Is this the only place where it is valid? Probably also
          // in var namespace.
          //
          next (t, tt);
          print (t, tt);
          continue;
        }
        else if (n == "source")
        {
          next (t, tt);
          source (t, tt);
          continue;
        }
        else if (n == "include")
        {
          next (t, tt);
          include (t, tt);
          continue;
        }
        else if (n == "import")
        {
          next (t, tt);
          import (t, tt);
          continue;
        }
        else if (n == "export")
        {
          next (t, tt);
          export_ (t, tt);
          continue;
        }
        else if (n == "using")
        {
          next (t, tt);
          using_ (t, tt);
          continue;
        }
      }

      // ': foo' is equvalent to '{}: foo' and to 'dir{}: foo'.
      //
      location nloc (get_location (t, &path_));
      names_type ns (tt != type::colon
                     ? names (t, tt)
                     : names_type ({name ("dir", dir_path (), string ())}));

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
              if (n.type.empty () && n.value.empty ())
              {
                if (ns.size () != 1)
                {
                  // @@ TODO: point to name.
                  //
                  fail (t) << "multiple names in directory scope";
                }

                dir = true;
              }
            }

            next (t, tt);

            if (dir)
            {
              // Directory scope.
              //
              scope& prev (*scope_);

              dir_path p (move (ns[0].dir)); // Steal.

              if (p.relative ())
                p = prev.path () / p;

              p.normalize ();
              scope_ = &scopes[p];

              // If this is a known project root scope, switch the
              // parser state to use it.
              //
              scope* ors (switch_root (scope_->root () ? scope_ : root_));

              if (ors != root_)
                level4 ([&]{trace (nloc) << "switching to root scope " << p;});

              // A directory scope can contain anything that a top level can.
              //
              clause (t, tt);

              switch_root (ors);
              scope_ = &prev;
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

        // Dependency declaration.
        //
        if (tt == type::name    ||
            tt == type::lcbrace ||
            tt == type::dollar  ||
            tt == type::newline ||
            tt == type::eos)
        {
          location ploc (get_location (t, &path_));
          names_type pns (tt != type::newline && tt != type::eos
                          ? names (t, tt)
                          : names_type ());

          // Prepare the prerequisite list.
          //
          target::prerequisites_type ps;
          ps.reserve (pns.size ());

          for (auto& pn: pns)
          {
            const string* e;
            const target_type* ti (target_types.find (pn, e));

            if (ti == nullptr)
              fail (ploc) << "unknown target type " << pn.type;

            pn.dir.normalize ();

            // Find or insert.
            //
            prerequisite& p (
              scope_->prerequisites.insert (
                *ti, move (pn.dir), move (pn.value), e, *scope_, trace).first);

            ps.emplace_back (p);
          }

          for (auto& tn: ns)
          {
            const string* e;
            const target_type* ti (target_types.find (tn, e));

            if (ti == nullptr)
              fail (nloc) << "unknown target type " << tn.type;

            path& d (tn.dir);

            if (d.empty ())
              d = scope_->path (); // Already normalized.
            else
            {
              if (d.relative ())
                d = scope_->path () / d;

              d.normalize ();
            }

            // Find or insert.
            //
            target& t (
              targets.insert (
                *ti, move (tn.dir), move (tn.value), e, trace).first);

            //@@ OPT: move if last/single target (common cases).
            //
            t.prerequisites.insert (t.prerequisites.end (),
                                    ps.begin (),
                                    ps.end ());

            if (default_target_ == nullptr)
              default_target_ = &t;
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
        // LHS should be a single, simple name.
        //
        if (ns.size () != 1 || !ns[0].type.empty () || !ns[0].dir.empty ())
          fail (t) << "variable name expected before " << t;

        string name;
        if (ns[0].value.front () == '.') // Fully qualified name.
          name.assign (ns[0].value, 1, string::npos);
        else
          //@@ TODO: append namespace if any.
          name = move (ns[0].value);

        type kind (tt);
        next (t, tt);
        variable (t, tt, move (name), kind);

        if (tt == type::newline)
          next (t, tt);
        else if (tt != type::eos)
          fail (t) << "expected newline instead of " << t;

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
    location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      // Construct the buildfile path.
      //
      path p (move (n.dir));
      p /= path (move (n.value));

      // If the path is relative then use the src directory corresponding
      // to the current directory scope.
      //
      if (src_root_ != nullptr && p.relative ())
        p = src_out (scope_->path (), *out_root_, *src_root_) / p;

      p.normalize ();

      ifstream ifs (p.string ());

      if (!ifs.is_open ())
        fail (l) << "unable to open " << p;

      ifs.exceptions (ifstream::failbit | ifstream::badbit);

      level4 ([&]{trace (t) << "entering " << p;});

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

      level4 ([&]{trace (t) << "leaving " << p;});

      lexer_ = ol;
      path_ = op;

      // If src_root is unknown (happens during bootstrap), reload it
      // in case the just sourced buildfile set it. This way, once it
      // is set, all the parser mechanism that were disabled (like
      // relative file source'ing) will start working. Note that they
      // will still be disabled inside the file that set src_root. For
      // this to work we would need to keep a reference to the value
      // stored in the variable plus the update would need to update
      // the value in place (see value_proxy).
      //
      if (src_root_ == nullptr)
      {
        auto v (root_->vars["src_root"]);
        src_root_ = v ? &v.as<const dir_path&> () : nullptr;
      }
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

    if (src_root_ == nullptr)
      fail (t) << "inclusion during bootstrap";

    // The rest should be a list of buildfiles. Parse them as names
    // to get variable expansion and directory prefixes.
    //
    location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
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

      bool in_out (false);
      if (p.absolute ())
      {
        p.normalize ();

        // Make sure the path is in this project. Include is only meant
        // to be used for intra-project inclusion.
        //
        if (!p.sub (*src_root_) && !(in_out = p.sub (*out_root_)))
          fail (l) << "out of project include " << p;
      }
      else
      {
        // Use the src directory corresponding to the current directory scope.
        //
        p = src_out (scope_->path (), *out_root_, *src_root_) / p;
        p.normalize ();
      }

      if (!root_->buildfiles.insert (p).second)
      {
        level4 ([&]{trace (l) << "skipping already included " << p;});
        continue;
      }

      // Determine new bases.
      //
      dir_path out_base;
      dir_path src_base;

      if (in_out)
      {
        out_base = p.directory ();
        src_base = src_out (out_base, *out_root_, *src_root_);
      }
      else
      {
        src_base = p.directory ();
        out_base = out_src (src_base, *out_root_, *src_root_);
      }

      // Create and bootstrap root scope(s) of subproject(s) that
      // this out_base belongs to. If any were created, load them
      // and update parser state.
      //
      scope* ors (switch_root (&create_bootstrap_inner (*root_, out_base)));

      if (root_ != ors)
        load_root_pre (*root_); // Loads outer roots recursively.

      ifstream ifs (p.string ());

      if (!ifs.is_open ())
        fail (l) << "unable to open " << p;

      ifs.exceptions (ifstream::failbit | ifstream::badbit);

      level4 ([&]{trace (t) << "entering " << p;});

      string rw (diag_relative (p)); // Relative to work.
      const string* op (path_);
      path_ = &rw;

      lexer l (ifs, rw);
      lexer* ol (lexer_);
      lexer_ = &l;

      scope* os (scope_);
      scope_ = &scopes[out_base];

      scope_->assign ("out_base") = move (out_base);
      auto v (scope_->assign ("src_base") = move (src_base));
      scope_->src_path_ = &v.as<const dir_path&> ();

      target* odt (default_target_);
      default_target_ = nullptr;

      token t (type::eos, false, 0, 0);
      type tt;
      next (t, tt);
      clause (t, tt);

      if (tt != type::eos)
        fail (t) << "unexpected " << t;

      process_default_target (t);

      level4 ([&]{trace (t) << "leaving " << p;});

      default_target_ = odt;
      scope_ = os;
      lexer_ = ol;
      path_ = op;

      switch_root (ors);
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

    if (src_root_ == nullptr)
      fail (t) << "import during bootstrap";

    // General import format:
    //
    // import [<var>=](<project>|<project>/<target>])+
    //
    value_proxy val;
    token_type at; // Assignment type.
    if (tt == type::name)
    {
      at = peek ();

      if (at == token_type::equal || at == token_type::plus_equal)
      {
        val.rebind (at == token_type::equal
                    ? scope_->assign (t.name ())
                    : scope_->append (t.name ()));
        next (t, tt); // Consume =/+=.
        next (t, tt);
      }
    }

    // The rest should be a list of projects and/or targets. Parse
    // them as names to get variable expansion and directory prefixes.
    //
    location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      list_value r (build::import (*scope_, n, l));

      if (val.defined ())
      {
        if (at == token_type::equal)
          val = move (r);
        else
          val += move (r);
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
    if (ps == nullptr || ps->path () != scope_->path ())
      fail (t) << "export outside export stub";

    // The rest is a value. Parse it as names to get variable expansion.
    //
    location l (get_location (t, &path_));
    export_value_ = (tt != type::newline && tt != type::eos
                     ? names (t, tt)
                     : names_type ());

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
    location l (get_location (t, &path_));
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    for (name& n: ns)
    {
      // For now it should be a simple name.
      //
      if (!n.type.empty () || !n.dir.empty ())
        fail (l) << "module name expected instead of " << n;

      const string& name (n.value);
      auto i (modules.find (name));

      if (i == modules.end ())
        fail (l) << "unknown module " << name;

      i->second (*root_, *scope_, l);
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  print (token& t, token_type& tt)
  {
    names_type ns (tt != type::newline && tt != type::eos
                   ? names (t, tt)
                   : names_type ());

    cout << ns << endl;

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  void parser::
  variable (token& t, token_type& tt, string name, token_type kind)
  {
    bool assign (kind == type::equal);

    names_type vns (tt != type::newline && tt != type::eos
                    ? names (t, tt)
                    : names_type ());

    // Enter the variable.
    //
    const auto& var (variable_pool.find (move (name)));

    if (assign)
    {
      value_ptr& val (scope_->assign (var));

      if (val == nullptr) // Initialization.
      {
        val.reset (new list_value (move (vns)));
      }
      else // Assignment.
      {
        //@@ TODO: assuming it is a list.
        //
        dynamic_cast<list_value&> (*val) = move (vns);
      }
    }
    else
    {
      if (auto val = (*scope_)[var])
      {
        //@@ TODO: assuming it is a list.
        //
        list_value* lv (&val.as<list_value&> ());

        if (!val.belongs (*scope_)) // Append to value from parent scope?
        {
          list_value_ptr nval (new list_value (*lv));
          lv = nval.get (); // Append to.
          scope_->vars.emplace (var, move (nval));
        }

        lv->insert (lv->end (),
                    make_move_iterator (vns.begin ()),
                    make_move_iterator (vns.end ()));
      }
      else // Initialization.
      {
        list_value_ptr nval (new list_value (move (vns)));
        scope_->vars.emplace (var, move (nval));
      }
    }
  }

  void parser::
  names (token& t,
         type& tt,
         names_type& ns,
         size_t pair,
         const dir_path* dp,
         const string* tp)
  {
    // If pair is not 0, then it is an index + 1 of the first half of
    // the pair for which we are parsing the second halves, e.g.,
    // a={b c d{e f} {}}.
    //

    // Buffer that is used to collect the complete name in case of an
    // unseparated variable expansion, e.g., 'foo$bar$(baz)fox'. The
    // idea is to concatenate all the individual parts in this buffer
    // and then re-inject it into the loop as a single token.
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
      // not a name or var expansion or if it is separated.
      //
      if (!concat.empty () &&
          ((tt != type::name && tt != type::dollar) || peeked ().separated ()))
      {
        tt = type::name;
        t = token (move (concat), true, t.line (), t.column ());
        concat.clear ();
      }
      else if (!first)
        next (t, tt);

      // Name.
      //
      if (tt == type::name)
      {
        string name (t.name ()); //@@ move?
        tt = peek ();

        // Should we accumulate? If the buffer is not empty, then
        // we continue accumulating (the case where we are separated
        // should have been handled by the injection code above). If
        // the next token is a var expansion and it is not separated,
        // then we need to start accumulating.
        //
        if (!concat.empty () ||                              // Continue.
            (tt == type::dollar && !peeked ().separated ())) // Start.
        {
          concat += name;
          continue;
        }

        string::size_type p (name.rfind ('/'));
        string::size_type n (name.size () - 1);

        // See if this is a type name, directory prefix, or both. That is,
        // it is followed by '{'.
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
                 (pair != 0
                  ? pair
                  : (ns.empty () || !ns.back ().pair ? 0 : ns.size ())),
                 dp1, tp1);
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

        // If it ends with a directory separator, then it is a directory.
        // Note that at this stage we don't treat '.' and '..' as special
        // (unless they are specified with a directory separator) because
        // then we would have ended up treating '.: ...' as a directory
        // scope. Instead, this is handled higher up the processing chain,
        // in target_types::find().
        //
        // @@ TODO: and not quoted
        //
        if (p == n)
        {
          // On Win32 translate the root path to the special empty path.
          // Search for global_scope for details.
          //
#ifdef _WIN32
          dir_path dir (name != "/" ? dir_path (name) : dir_path ());
#else
          dir_path dir (name);
#endif
          if (dp != nullptr)
            dir = *dp / dir;

          ns.emplace_back ((tp != nullptr ? *tp : string ()),
                           move (dir),
                           string ());
        }
        else
          ns.emplace_back ((tp != nullptr ? *tp : string ()),
                           (dp != nullptr ? *dp : dir_path ()),
                           move (name));

        count = 1;
        continue;
      }

      // Variable expansion.
      //
      if (tt == type::dollar)
      {
        next (t, tt);

        bool paren (tt == type::lparen);
        if (paren)
          next (t, tt);

        if (tt != type::name)
          fail (t) << "variable name expected instead of " << t;

        string n;
        if (t.name ().front () == '.') // Fully qualified name.
          n.assign (t.name (), 1, string::npos);
        else
          //@@ TODO: append namespace if any.
          n = t.name ();

        const auto& var (variable_pool.find (move (n)));
        auto val ((*scope_)[var]);

        // Undefined namespaces variables are not allowed.
        //
        if (!val && var.name.find ('.') != string::npos)
          fail (t) << "undefined namespace variable " << var.name;

        if (paren)
        {
          next (t, tt);

          if (tt != type::rparen)
            fail (t) << "expected ) instead of " << t;
        }

        tt = peek ();

        if (!val)
          continue;

        //@@ TODO: assuming it is a list.
        //
        const list_value& lv (val.as<list_value&> ());

        if (lv.empty ())
          continue;

        // Should we accumulate? If the buffer is not empty, then
        // we continue accumulating (the case where we are separated
        // should have been handled by the injection code above). If
        // the next token is a name or var expansion and it is not
        // separated, then we need to start accumulating.
        //
        if (!concat.empty () ||                       // Continue.
            ((tt == type::name || tt == type::dollar) // Start.
             && !peeked ().separated ()))
        {
          // This should be a simple value or a simple directory. The
          // token still points to the name (or closing paren).
          //
          if (lv.size () > 1)
            fail (t) << "concatenating expansion of " << var.name
                     << " contains multiple values";

          const name& n (lv[0]);

          if (!n.type.empty ())
            fail (t) << "concatenating expansion of " << var.name
                     << " contains type";

          if (!n.dir.empty ())
          {
            if (!n.value.empty ())
              fail (t) << "concatenating expansion of " << var.name
                       << " contains directory";

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
            const dir_path* dp1 (dp);
            const string* tp1 (tp);

            dir_path d1;
            if (!n.dir.empty ())
            {
              if (dp != nullptr)
              {
                if (n.dir.absolute ())
                  fail (t) << "nested absolute directory " << n.dir
                           << " in variable expansion";

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
                fail (t) << "nested type name " << n.type << " in variable "
                         << "expansion";
            }

            // If we are a second half of a pair.
            //
            if (pair != 0)
            {
              // Check that there are no nested pairs.
              //
              if (n.pair)
                fail (t) << "nested pair in variable expansion";

              // And add another first half unless this is the first instance.
              //
              if (pair != ns.size ())
                ns.push_back (ns[pair - 1]);
            }

            ns.emplace_back ((tp1 != nullptr ? *tp1 : string ()),
                             (dp1 != nullptr ? *dp1 : dir_path ()),
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
               (pair != 0
                ? pair
                : (ns.empty () || !ns.back ().pair ? 0 : ns.size ())),
               dp, tp);
        count = ns.size () - count;

        if (tt != type::rcbrace)
          fail (t) << "expected } instead of " << t;

        tt = peek ();
        continue;
      }

      // A pair separator (only in the pair mode).
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
          ns.emplace_back ((tp != nullptr ? *tp : string ()),
                           (dp != nullptr ? *dp : dir_path ()),
                           "");
          count = 1;
        }

        ns.back ().pair = true;
        tt = peek ();
        continue;
      }

      if (!first)
        break;

      // Our caller expected this to be a name.
      //
      if (tt == type::rcbrace) // Empty name, e.g., dir{}.
      {
        // If we are a second half of a pair, add another first half
        // unless this is the first instance.
        //
        if (pair != 0 && pair != ns.size ())
          ns.push_back (ns[pair - 1]);

        ns.emplace_back ((tp != nullptr ? *tp : string ()),
                         (dp != nullptr ? *dp : dir_path ()),
                         "");
        break;
      }
      else
        fail (t) << "expected name instead of " << t;
    }

    // Handle the empty RHS in a pair, (e.g., {y=}).
    //
    if (!ns.empty () && ns.back ().pair)
    {
      ns.emplace_back ((tp != nullptr ? *tp : string ()),
                       (dp != nullptr ? *dp : dir_path ()),
                       "");
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
    if (n.pair || !n.type.empty () || !n.dir.empty () || n.value.empty ())
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
      // We always start with one or more names.
      //
      if (tt != type::name    &&
          tt != type::lcbrace &&      // Untyped name group: '{foo ...'
          tt != type::dollar  &&      // Variable expansion: '$foo ...'
          tt != type::pair_separator) // Empty pair LHS: '=foo ...'
        fail (t) << "operation or target expected instead of " << t;

      location l (get_location (t, &path_)); // Start of names.

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
          if (opname (*i))
            ms.push_back (opspec (move (i->value)));
          else
          {
            // Do we have the src_base?
            //
            dir_path src_base;
            if (i->pair)
            {
              if (!i->type.empty ())
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
        location l (get_location (t, &path_)); // Start of nested names.
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

  scope* parser::
  switch_root (scope* nr)
  {
    scope* r (root_);

    if (nr != root_)
    {
      root_ = nr;

      // During bootstrap we may not know src_root yet. We are also
      // not using the scopes's path() and src_path() since pointers
      // to their return values are not guaranteed to be stable (and,
      // in fact, path()'s is not).
      //
      out_root_ = &root_->vars["out_root"].as<const dir_path&> ();

      auto v (root_->vars["src_root"]);
      src_root_ = v ? &v.as<const dir_path&> () : nullptr;
    }

    return r;
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
                      scope_->path (),
                      "",
                      nullptr,
                      trace) != targets.end ())
      return;

    target& dt (*default_target_);

    level4 ([&]{trace (t) << "creating current directory alias for " << dt;});

    target& ct (
      targets.insert (
        dir::static_type, scope_->path (), "", nullptr, trace).first);

    prerequisite& p (
      scope_->prerequisites.insert (
        dt.type (),
        dt.dir,
        dt.name,
        dt.ext,
        *scope_, // Doesn't matter which scope since dir is absolute.
        trace).first);

    p.target = &dt;
    ct.prerequisites.emplace_back (p);
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

    tt = t.type ();
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

    return peek_.type ();
  }

  static location
  get_location (const token& t, const void* data)
  {
    assert (data != nullptr);
    const string& p (**static_cast<const string* const*> (data));
    return location (p.c_str (), t.line (), t.column ());
  }
}
