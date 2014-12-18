// file      : build/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/parser>

#include <memory>   // unique_ptr
#include <iostream>

#include <build/token>
#include <build/lexer>

#include <build/scope>
#include <build/target>
#include <build/prerequisite>

using namespace std;

namespace build
{
  // Output the token type and value in a format suitable for diagnostics.
  //
  ostream&
  operator<< (ostream&, const token&);

  typedef token_type type;

  void parser::
  parse (istream& is, const path& p, scope& s)
  {
    lexer l (is, p.string (), diag_);
    lexer_ = &l;
    path_ = &p;
    scope_ = &s;

    token t (type::eos, 0, 0);
    type tt;
    next (t, tt);

    parse_clause (t, tt);

    if (tt != type::eos)
    {
      error (t) << "unexpected " << t << endl;
      throw parser_error ();
    }
  }

  void parser::
  parse_clause (token& t, token_type& tt)
  {
    while (tt != type::eos)
    {
      // We always start with one or more names.
      //
      if (tt != type::name && tt != type::lcbrace)
        break; // Something else. Let our caller handle that.

      names tns (parse_names (t, tt));

      if (tt == type::colon)
      {
        next (t, tt);

        // Dependency declaration.
        //
        if (tt == type::name || tt == type::lcbrace)
        {
          names pns (parse_names (t, tt));

          // Prepare the prerequisite list.
          //
          target::prerequisites_type ps;
          ps.reserve (pns.size ());

          for (auto& pn: pns)
          {
            // Resolve prerequisite type.
            //
            //@@ TODO: derive type from extension, factor to common function
            //
            const char* tt (pn.type.empty () ? "file" : pn.type.c_str ());

            auto i (target_types.find (tt));

            if (i == target_types.end ())
            {
              //@@ TODO name (or better yet, type) location

              error (t) << "unknown prerequisite type '" << tt << "'" << endl;
              throw parser_error ();
            }

            const target_type& ti (i->second);

            // We need to split the name into its directory part (if any)
            // and the name part. We cannot assume the name part is a
            // valid filesystem name so we will have to do the splitting
            // manually.
            //
            path d;
            string n;

            {
              path::size_type i (path::traits::rfind_separator (pn.name));

              if (i == string::npos)
                n = move (pn.name); // NOTE: steal!
              else
              {
                d = path (pn.name, i);
                n.assign (pn.name, i + 1, string::npos);
                d.normalize ();
              }
            }

            //cout << "prerequisite " << tt << " " << n << " " << d << endl;

            // Find or insert.
            //
            auto r (scope_->prerequisites.emplace (
                      ti, move (n), move (d), *scope_));

            ps.push_back (const_cast<prerequisite&> (*r.first));
          }

          for (auto& tn: tns)
          {
            path d;
            string n;

            // The same deal as in handling prerequisites above.
            //
            {
              path::size_type i (path::traits::rfind_separator (tn.name));

              if (i == string::npos)
              {
                d = scope_->path (); // Already normalized.
                n = move (tn.name); // NOTE: steal!
              }
              else
              {
                d = path (tn.name, i);
                n.assign (tn.name, i + 1, string::npos);

                if (d.relative ())
                  d = scope_->path () / d;

                d.normalize ();
              }
            }

            // Resolve target type.
            //
            //@@ TODO: derive type from extension
            //
            const char* tt (tn.type.empty () ? "file" : tn.type.c_str ());

            auto i (target_types.find (tt));

            if (i == target_types.end ())
            {
              //@@ TODO name (or better yet, type) location

              error (t) << "unknown target type '" << tt << "'" << endl;
              throw parser_error ();
            }

            const target_type& ti (i->second);

            //@@ TODO would be nice to first check if this target is
            //   already in the set before allocating a new instance.

            //cout << "target " << tt << " " << n << " " << d << endl;

            // Find or insert.
            //
            auto r (
              targets.emplace (
                unique_ptr<target> (ti.factory (move (n), move (d)))));

            target& t (**r.first);

            t.prerequisites = ps; //@@ TODO: move is last target.

            if (default_target == nullptr)
              default_target = &t;
          }

          if (tt == type::newline)
            next (t, tt);
          else if (tt != type::eos)
          {
            error (t) << "expected newline instead of " << t << endl;
            throw parser_error ();
          }

          continue;
        }

        if (tt == type::newline)
        {
          // See if we have a directory/target scope.
          //
          if (next (t, tt) == type::lcbrace)
          {
            // Should be on its own line.
            //
            if (next (t, tt) != type::newline)
            {
              error (t) << "expected newline after '{'" << endl;
              throw parser_error ();
            }

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
                  error (t) << "multiple names in directory scope" << endl;
                  throw parser_error ();
                }

                dir = true;
              }
            }

            next (t, tt);

            if (dir)
            {
              scope& prev (*scope_);
              path p (tns[0].name);

              if (p.relative ())
                p = prev.path () / p;

              scope_ = &scopes[p];

              // A directory scope can contain anything that a top level can.
              //
              parse_clause (t, tt);

              scope_ = &prev;
            }
            else
            {
              // @@ TODO: target scope.
            }

            if (tt != type::rcbrace)
            {
              error (t) << "expected '}' instead of " << t << endl;
              throw parser_error ();
            }

            // Should be on its own line.
            //
            if (next (t, tt) == type::newline)
              next (t, tt);
            else if (tt != type::eos)
            {
              error (t) << "expected newline after '}'" << endl;
              throw parser_error ();
            }
          }

          continue;
        }

        if (tt == type::eos)
          continue;

        error (t) << "expected newline insetad of " << t << endl;
        throw parser_error ();
      }

      error (t) << "unexpected " << t << endl;
      throw parser_error ();
    }
  }

  void parser::
  parse_names (token& t, type& tt, names& ns, const string* tp)
  {
    for (bool first (true);; first = false)
    {
      // Untyped name group, e.g., '{foo bar}'.
      //
      if (tt == type::lcbrace)
      {
        next (t, tt);
        parse_names (t, tt, ns, tp);

        if (tt != type::rcbrace)
        {
          error (t) << "expected '}' instead of " << t << endl;
          throw parser_error ();
        }

        next (t, tt);
        continue;
      }

      // Name.
      //
      if (tt == type::name)
      {
        string name (t.name ()); //@@ move?

        // See if this is a type name, that is, it is followed by '{'.
        //
        if (next (t, tt) == type::lcbrace)
        {
          if (tp != nullptr)
          {
            error (t) << "nested type name '" << name << "'" << endl;
            throw parser_error ();
          }

          next (t, tt);
          parse_names (t, tt, ns, &name);

          if (tt != type::rcbrace)
          {
            error (t) << "expected '}' instead of " << t << endl;
            throw parser_error ();
          }

          next (t, tt);
          continue;
        }

        ns.emplace_back ((tp != nullptr ? *tp : string ()), move (name));
        continue;
      }

      if (!first)
        break;

      error (t) << "expected name instead of " << t << endl;
      throw parser_error ();
    }
  }

  token_type parser::
  next (token& t, token_type& tt)
  {
    t = lexer_->next ();
    tt = t.type ();
    return tt;
  }

  ostream& parser::
  error (const token& t)
  {
    return diag_ << path_->string () << ':' << t.line () << ':' <<
      t.column () << ": error: ";
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
    case token_type::colon:   os << "':'"; break;
    case token_type::lcbrace: os << "'{'"; break;
    case token_type::rcbrace: os << "'}'"; break;
    case token_type::name:    os << '\'' << t.name () << '\''; break;
    }

    return os;
  }
}
