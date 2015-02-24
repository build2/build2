// file      : build/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/parser>

#include <memory>   // unique_ptr
#include <fstream>
#include <utility>  // move()
#include <iostream>

#include <build/token>
#include <build/lexer>

#include <build/scope>
#include <build/target>
#include <build/prerequisite>
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
  // into account extensions, trailing '/', or anything else that might
  // be relevant.
  //
  static const char*
  find_target_type (const string& n, const string* e)
  {
    // Empty name or a name ending with a directory separator
    // signifies a directory.
    //
    if (n.empty () || path::traits::is_separator (n.back ()))
      return "dir";

    //@@ TODO: derive type from extension.
    //
    return "file";
  }

  void parser::
  parse (istream& is, const path& p, scope& s)
  {
    string rw (diag_relative_work (p));
    path_ = &rw;

    lexer l (is, p.string ());
    lexer_ = &l;
    scope_ = &s;

    token t (type::eos, 0, 0);
    type tt;
    next (t, tt);

    clause (t, tt);

    if (tt != type::eos)
      fail (t) << "unexpected " << t;
  }

  void parser::
  clause (token& t, token_type& tt)
  {
    tracer trace ("parser::clause", &path_);

    while (tt != type::eos)
    {
      // We always start with one or more names.
      //
      if (tt != type::name && tt != type::lcbrace && tt != type::colon)
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
      names_type tns (tt != type::colon
                      ? names (t, tt)
                      : names_type ({name_type ("", path (), "")}));

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
            for (const auto& n: tns)
            {
              if (n.type.empty () && n.name.back () == '/')
              {
                if (tns.size () != 1)
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

              // On Win32 translate the root path to the special empty path.
              // Search for root_scope for details.
              //
#ifdef _WIN32
              path p (tns[0].name != "/" ? path (tns[0].name) : path ());
#else
              path p (tns[0].name);
#endif
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
              fail (t) << "expected '}' instead of " << t;

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
            tt == type::newline ||
            tt == type::eos)
        {
          names_type pns (tt != type::newline && tt != type::eos
                          ? names (t, tt)
                          : names_type ());

          // Prepare the prerequisite list.
          //
          target::prerequisites_type ps;
          ps.reserve (pns.size ());

          for (auto& pn: pns)
          {
            // We need to split the path into its directory part (if any)
            // the name part, and the extension (if any). We cannot assume
            // the name part is a valid filesystem name so we will have
            // to do the splitting manually.
            //
            path d (pn.dir);
            string n;
            const string* e (nullptr);

            {
              path::size_type i (path::traits::rfind_separator (pn.name));

              if (i == string::npos)
                n = move (pn.name); // NOTE: steal!
              else
              {
                d /= path (pn.name, i != 0 ? i : 1); // Special case: "/".
                n.assign (pn.name, i + 1, string::npos);
              }

              // Handle '.' and '..'.
              //
              if (n == ".")
                n.clear ();
              else if (n == "..")
              {
                d /= path (n);
                n.clear ();
              }

              d.normalize ();

              // Extract extension.
              //
              string::size_type j (path::traits::find_extension (n));

              if (j != string::npos)
              {
                e = &extension_pool.find (n.c_str () + j);
                n.resize (j - 1);
              }
            }

            // Resolve prerequisite type.
            //
            const char* tt (pn.type.empty ()
                            ? find_target_type (n, e)
                            : pn.type.c_str ());

            auto i (target_types.find (tt));

            if (i == target_types.end ())
            {
              //@@ TODO name (or better yet, type) location

              fail (t) << "unknown prerequisite type " << tt;
            }

            const target_type& ti (i->second);

            // Find or insert.
            //
            prerequisite& p (
              scope_->prerequisites.insert (
                ti, move (d), move (n), e, *scope_, trace).first);

            ps.push_back (p);
          }

          for (auto& tn: tns)
          {
            path d (tn.dir);
            string n;
            const string* e (nullptr);

            // The same deal as in handling prerequisites above.
            //
            {
              path::size_type i (path::traits::rfind_separator (tn.name));

              if (i == string::npos)
                n = move (tn.name); // NOTE: steal!
              else
              {
                d /= path (tn.name, i != 0 ? i : 1); // Special case: "/".
                n.assign (tn.name, i + 1, string::npos);
              }

              // Handle '.' and '..'.
              //
              if (n == ".")
                n.clear ();
              else if (n == "..")
              {
                d /= path (n);
                n.clear ();
              }

              if (d.empty ())
                d = scope_->path (); // Already normalized.
              else
              {
                if (d.relative ())
                  d = scope_->path () / d;

                d.normalize ();
              }

              // Extract extension.
              //
              string::size_type j (path::traits::find_extension (n));

              if (j != string::npos)
              {
                e = &extension_pool.find (n.c_str () + j);
                n.resize (j - 1);
              }
            }

            // Resolve target type.
            //
            const char* tt (tn.type.empty ()
                            ? find_target_type (n, e)
                            : tn.type.c_str ());

            auto i (target_types.find (tt));

            if (i == target_types.end ())
            {
              //@@ TODO name (or better yet, type) location

              fail (t) << "unknown target type " << tt;
            }

            const target_type& ti (i->second);

            // Find or insert.
            //
            target& t (
              targets.insert (
                ti, move (d), move (n), e, trace).first);

            t.prerequisites = ps; //@@ OPT: move if last target.

            if (default_target == nullptr)
              default_target = &t;
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

      fail (t) << "unexpected " << t;
    }
  }

  void parser::
  source (token& t, token_type& tt)
  {
    tracer trace ("parser::source", &path_);

    // The rest should be a list of paths to buildfiles.
    //
    for (; tt != type::newline && tt != type::eos; next (t, tt))
    {
      if (tt != type::name)
        fail (t) << "expected buildfile to source instead of " << t;

      path p (t.name ());

      // If the path is relative then use the src directory corresponding
      // to the current directory scope.
      //
      if (p.relative ())
        p = src_out (scope_->path ()) / p;

      ifstream ifs (p.string ());

      if (!ifs.is_open ())
        fail (t) << "unable to open " << p;

      ifs.exceptions (ifstream::failbit | ifstream::badbit);

      level4 ([&]{trace (t) << "entering " << p;});

      string rw (diag_relative_work (p));
      const string* op (path_);
      path_ = &rw;

      lexer l (ifs, p.string ());
      lexer* ol (lexer_);
      lexer_ = &l;

      next (t, tt);
      clause (t, tt);

      if (tt != type::eos)
        fail (t) << "unexpected " << t;

      level4 ([&]{trace (t) << "leaving " << p;});

      lexer_ = ol;
      path_ = op;
    }

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
  }

  void parser::
  include (token& t, token_type& tt)
  {
    tracer trace ("parser::include", &path_);

    // The rest should be a list of paths to buildfiles.
    //
    for (; tt != type::newline && tt != type::eos; next (t, tt))
    {
      if (tt != type::name)
        fail (t) << "expected buildfile to include instead of " << t;

      path p (t.name ());
      bool in_out (false);

      if (p.absolute ())
      {
        p.normalize ();

        // Make sure the path is in this project. Include is only meant
        // to be used for intra-project inclusion.
        //
        if (!p.sub (src_root) && !(in_out = p.sub (out_root)))
          fail (t) << "out of project include " << p;
      }
      else
      {
        // Use the src directory corresponding to the current directory scope.
        //
        p = src_out (scope_->path ()) / p;
        p.normalize ();
      }

      if (!include_.insert (p).second)
      {
        level4 ([&]{trace (t) << "skipping already included " << p;});
        continue;
      }

      ifstream ifs (p.string ());

      if (!ifs.is_open ())
        fail (t) << "unable to open " << p;

      ifs.exceptions (ifstream::failbit | ifstream::badbit);

      level4 ([&]{trace (t) << "entering " << p;});

      string rw (diag_relative_work (p));
      const string* op (path_);
      path_ = &rw;

      lexer l (ifs, p.string ());
      lexer* ol (lexer_);
      lexer_ = &l;

      scope* os (scope_);
      scope_ = &scopes[(in_out ? p : out_src (p)).directory ()];

      next (t, tt);
      clause (t, tt);

      if (tt != type::eos)
        fail (t) << "unexpected " << t;

      level4 ([&]{trace (t) << "leaving " << p;});

      scope_ = os;
      lexer_ = ol;
      path_ = op;
    }

    if (tt != type::eos)
      next (t, tt); // Swallow newline.
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
    for (bool first (true);; first = false)
    {
      // Untyped name group without a directory prefix, e.g., '{foo bar}'.
      //
      if (tt == type::lcbrace)
      {
        next (t, tt);
        names (t, tt, ns, dp, tp);

        if (tt != type::rcbrace)
          fail (t) << "expected '}' instead of " << t;

        next (t, tt);
        continue;
      }

      // Name.
      //
      if (tt == type::name)
      {
        string name (t.name ()); //@@ move?

        // See if this is a type name, directory prefix, or both. That is,
        // it is followed by '{'.
        //
        if (next (t, tt) == type::lcbrace)
        {
          string::size_type p (name.rfind ('/')), n (name.size () - 1);

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
            fail (t) << "expected '}' instead of " << t;

          next (t, tt);
          continue;
        }

        ns.emplace_back ((tp != nullptr ? *tp : string ()),
                         (dp != nullptr ? *dp : path ()),
                         move (name));
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
    case token_type::eos:     os << "<end-of-stream>"; break;
    case token_type::newline: os << "<newline>"; break;
    case token_type::colon:   os << ":"; break;
    case token_type::lcbrace: os << "{"; break;
    case token_type::rcbrace: os << "}"; break;
    case token_type::name:    os << t.name (); break;
    }

    return os;
  }
}
