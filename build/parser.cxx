// file      : build/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/parser>

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
#include <build/diagnostics>
#include <build/context>

using namespace std;

namespace build
{
  // Output the token type and value in a format suitable for diagnostics.
  //
  ostream&
  operator<< (ostream&, const token&);

  static location
  get_location (const token&, const void*);

  typedef token_type type;

  // Given a target or prerequisite name, figure out its type, taking
  // into account extensions, special names (e.g., '.' and '..'), or
  // anything else that might be relevant. Also process the name (in
  // place) by extracting the extension, adjusting dir/value, etc.
  //
  const target_type&
  find_target_type (name& n, const location& l, const string*& ext)
  {
    string& v (n.value);

    // First determine the target type.
    //
    const char* tt;
    if (n.type.empty ())
    {
      // Empty name or '.' and '..' signify a directory.
      //
      if (v.empty () || v == "." || v == "..")
        tt = "dir";
      else
        //@@ TODO: derive type from extension.
        //
        tt = "file";
    }
    else
      tt = n.type.c_str ();

    auto i (target_types.find (tt));
    if (i == target_types.end ())
      fail (l) << "unknown target type " << tt;

    const target_type& ti (i->second);

    ext = nullptr;

    // Directories require special name processing. If we find that more
    // targets deviate, then we should make this target-type-specific.
    //
    if (ti.id == dir::static_type.id || ti.id == fsdir::static_type.id)
    {
      // The canonical representation of a directory name is with empty
      // value.
      //
      if (!v.empty ())
      {
        n.dir /= path (v); // Move name value to dir.
        v.clear ();
      }
    }
    else
    {
      // Split the path into its directory part (if any) the name part,
      // and the extension (if any). We cannot assume the name part is
      // a valid filesystem name so we will have to do the splitting
      // manually.
      //
      path::size_type i (path::traits::rfind_separator (v));

      if (i != string::npos)
      {
        n.dir /= path (v, i != 0 ? i : 1); // Special case: "/".
        v = string (v, i + 1, string::npos);
      }

      // Extract the extension.
      //
      string::size_type j (path::traits::find_extension (v));

      if (j != string::npos)
      {
        ext = &extension_pool.find (v.c_str () + j);
        v.resize (j - 1);
      }
    }

    return ti;
  }

  void parser::
  parse (istream& is, const path& p, scope& s)
  {
    string rw (diag_relative_work (p));
    path_ = &rw;

    lexer l (is, p.string ());
    lexer_ = &l;
    scope_ = &s;
    default_target_ = nullptr;

    out_root_ = &s["out_root"].as<const path&> ();
    src_root_ = &s["src_root"].as<const path&> ();

    token t (type::eos, false, 0, 0);
    type tt;
    next (t, tt);

    clause (t, tt);

    if (tt != type::eos)
      fail (t) << "unexpected " << t;

    process_default_target (t);
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
      }

      // ': foo' is equvalent to '{}: foo' and to 'dir{}: foo'.
      //
      location nloc (get_location (t, &path_));
      names_type ns (tt != type::colon
                     ? names (t, tt)
                     : names_type ({name ("dir", path (), string ())}));

      if (tt == type::colon)
      {
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
              scope& prev (*scope_);

              path p (move (ns[0].dir)); // Steal.

              if (p.relative ())
                p = prev.path () / p;

              p.normalize ();
              scope_ = &scopes[p];

              // A directory scope can contain anything that a top level can.
              //
              clause (t, tt);

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
            const target_type& ti (find_target_type (pn, ploc, e));

            pn.dir.normalize ();

            // Find or insert.
            //
            prerequisite& p (
              scope_->prerequisites.insert (
                ti, move (pn.dir), move (pn.value), e, *scope_, trace).first);

            ps.push_back (p);
          }

          for (auto& tn: ns)
          {
            const string* e;
            const target_type& ti (find_target_type (tn, nloc, e));
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
                ti, move (tn.dir), move (tn.value), e, trace).first);

            t.prerequisites = ps; //@@ OPT: move if last target.

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
        bool assign (tt == type::equal);

        // LHS should be a single, simple name.
        //
        if (ns.size () != 1 || !ns[0].type.empty () || !ns[0].dir.empty ())
          fail (t) << "variable name expected before " << t;

        next (t, tt);

        names_type vns (tt != type::newline && tt != type::eos
                        ? names (t, tt)
                        : names_type ());

        // Enter the variable.
        //
        string name;
        if (ns[0].value.front () == '.') // Fully qualified name.
          name.assign (ns[0].value, 1, string::npos);
        else
          //@@ TODO: append namespace if any.
          name = move (ns[0].value);

        const variable& var (variable_pool.find (move (name)));

        if (assign)
        {
          value_ptr& val (scope_->variables[var]);

          if (val == nullptr) // Initialization.
          {
            val.reset (new list_value (*scope_, move (vns)));
          }
          else // Assignment.
          {
            //@@ TODO: assuming it is a list.
            //
            dynamic_cast<list_value&> (*val).data = move (vns);
          }
        }
        else
        {
          if (auto val = (*scope_)[var])
          {
            //@@ TODO: assuming it is a list.
            //
            list_value* lv (&val.as<list_value&> ());

            if (&lv->scope != scope_) // Append to value from parent scope?
            {
              list_value_ptr nval (new list_value (*scope_, lv->data));
              lv = nval.get (); // Append to.
              scope_->variables.emplace (var, move (nval));
            }

            lv->data.insert (lv->data.end (),
                             make_move_iterator (vns.begin ()),
                             make_move_iterator (vns.end ()));
          }
          else // Initialization.
          {
            list_value_ptr nval (new list_value (*scope_, move (vns)));
            scope_->variables.emplace (var, move (nval));
          }
        }

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
      if (p.relative ())
        p = src_out (scope_->path (), *out_root_, *src_root_) / p;

      ifstream ifs (p.string ());

      if (!ifs.is_open ())
        fail (l) << "unable to open " << p;

      ifs.exceptions (ifstream::failbit | ifstream::badbit);

      level4 ([&]{trace (t) << "entering " << p;});

      string rw (diag_relative_work (p));
      const string* op (path_);
      path_ = &rw;

      lexer l (ifs, p.string ());
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

      if (!include_.insert (p).second)
      {
        level4 ([&]{trace (l) << "skipping already included " << p;});
        continue;
      }

      // Determine new bases.
      //
      path out_base;
      path src_base;

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

      ifstream ifs (p.string ());

      if (!ifs.is_open ())
        fail (l) << "unable to open " << p;

      ifs.exceptions (ifstream::failbit | ifstream::badbit);

      level4 ([&]{trace (t) << "entering " << p;});

      string rw (diag_relative_work (p));
      const string* op (path_);
      path_ = &rw;

      lexer l (ifs, p.string ());
      lexer* ol (lexer_);
      lexer_ = &l;

      scope* os (scope_);
      scope_ = &scopes[out_base];

      scope_->variables["out_base"] = move (out_base);
      scope_->variables["src_base"] = move (src_base);

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
    }

    if (tt == type::newline)
      next (t, tt);
    else if (tt != type::eos)
      fail (t) << "expected newline instead of " << t;
  }

  void parser::
  print (token& t, token_type& tt)
  {
    for (; tt != type::newline && tt != type::eos; next (t, tt))
      cout << t;

    cout << endl;

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  void parser::
  names (token& t, type& tt, names_type& ns, const path* dp, const string* tp)
  {
    // Buffer that is used to collect the complete name in case of an
    // unseparated variable expansion, e.g., 'foo$bar$(baz)fox'. The
    // idea is to concatenate all the individual parts in this buffer
    // and then re-inject it into the loop as a single token.
    //
    string concat;

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

          path d1;
          const path* dp1 (dp);

          string t1;
          const string* tp1 (tp);

          if (p == string::npos) // type
            tp1 = &name;
          else if (p == n) // directory
          {
            if (dp == nullptr)
              d1 = path (name);
            else
              d1 = *dp / path (name);

            dp1 = &d1;
          }
          else // both
          {
            t1.assign (name, p + 1, n - p);

            if (dp == nullptr)
              d1 = path (name, 0, p + 1);
            else
              d1 = *dp / path (name, 0, p + 1);

            dp1 = &d1;
            tp1 = &t1;
          }

          next (t, tt);
          names (t, tt, ns, dp1, tp1);

          if (tt != type::rcbrace)
            fail (t) << "expected } instead of " << t;

          tt = peek ();
          continue;
        }

        // If it ends with a directory separator, then it is a directory.
        // Note that at this stage we don't treat '.' and '..' as special
        // (unless they are specified with a directory separator) because
        // then we would have ended up treating '.: ...' as a directory
        // scope. Instead, this is handled higher up, in find_target_type().
        //
        // @@ TODO: and not quoted
        //
        if (p == n)
        {
          // On Win32 translate the root path to the special empty path.
          // Search for root_scope for details.
          //
#ifdef _WIN32
          path dir (name != "/" ? path (name) : path ());
#else
          path dir (name);
#endif
          if (dp != nullptr)
            dir = *dp / dir;

          ns.emplace_back ((tp != nullptr ? *tp : string ()),
                           move (dir),
                           string ());
        }
        else
          ns.emplace_back ((tp != nullptr ? *tp : string ()),
                           (dp != nullptr ? *dp : path ()),
                           move (name));

        continue;
      }

      // Untyped name group without a directory prefix, e.g., '{foo bar}'.
      //
      if (tt == type::lcbrace)
      {
        next (t, tt);
        names (t, tt, ns, dp, tp);

        if (tt != type::rcbrace)
          fail (t) << "expected } instead of " << t;

        tt = peek ();
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

        const variable& var (variable_pool.find (move (n)));
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

        if (lv.data.empty ())
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
          if (lv.data.size () > 1)
            fail (t) << "concatenating expansion of " << var.name
                     << " contains multiple values";

          const name& n (lv.data[0]);

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
          for (const name& n: lv.data)
          {
            const path* dp1 (dp);
            const string* tp1 (tp);

            path d1;
            if (!n.dir.empty ())
            {
              if (dp != nullptr)
              {
                if (n.dir.absolute ())
                  fail (t) << "nested absolute directory " << n.dir.string ()
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

            ns.emplace_back ((tp1 != nullptr ? *tp1 : string ()),
                             (dp1 != nullptr ? *dp1 : path ()),
                             n.value);
          }
        }

        continue;
      }

      if (!first)
        break;

      if (tt == type::rcbrace) // Empty name, e.g., dir{}.
      {
        ns.emplace_back ((tp != nullptr ? *tp : string ()),
                         (dp != nullptr ? *dp : path ()),
                         "");
        break;
      }
      else
        fail (t) << "expected name instead of " << t;
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
        targets.find (dir::static_type.id, // Explicit current dir target.
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
    ct.prerequisites.push_back (p);
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

  // Output the token type and value in a format suitable for diagnostics.
  //
  ostream&
  operator<< (ostream& os, const token& t)
  {
    switch (t.type ())
    {
    case token_type::eos:        os << "<end-of-file>"; break;
    case token_type::newline:    os << "<newline>"; break;
    case token_type::colon:      os << ":"; break;
    case token_type::lcbrace:    os << "{"; break;
    case token_type::rcbrace:    os << "}"; break;
    case token_type::equal:      os << "="; break;
    case token_type::plus_equal: os << "+="; break;
    case token_type::dollar:     os << "$"; break;
    case token_type::lparen:     os << "("; break;
    case token_type::rparen:     os << ")"; break;
    case token_type::name:       os << t.name (); break;
    }

    return os;
  }
}
