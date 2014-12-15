// file      : build/parser.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/parser>

#include <iostream>

#include <build/token>
#include <build/lexer>

using namespace std;

namespace build
{
  // Output the token type and value in a format suitable for diagnostics.
  //
  ostream&
  operator<< (ostream&, const token&);

  typedef token_type type;

  void parser::
  parse (istream& is, const path& p)
  {
    lexer l (is, p.string (), diag_);
    lexer_ = &l;
    path_ = &p;

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

      names ns (parse_names (t, tt));

      if (tt == type::colon)
      {
        next (t, tt);

        // Dependency declaration.
        //
        if (tt == type::name || tt == type::lcbrace)
        {
          names ns (parse_names (t, tt));

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
            // things can appear inside depending on which it is.
            //
            bool dir (false);
            for (const auto& n: ns)
            {
              if (n.back () == '/')
              {
                if (ns.size () != 1)
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
              // A directory scope can contain anything that a top level can.
              //
              parse_clause (t, tt);
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
  parse_names (token& t, type& tt, names& ns)
  {
    for (bool first (true);; first = false)
    {
      // Untyped name group, e.g., '{foo bar}'.
      //
      if (tt == type::lcbrace)
      {
        next (t, tt);
        parse_names (t, tt, ns);

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
        string name (t.name ());

        // See if this is a type name, that is, it is followed by '{'.
        //
        if (next (t, tt) == type::lcbrace)
        {
          //cout << "type: " << name << endl;

          //@@ TODO:
          //
          //   - detect nested typed name groups, e.g., 'cxx{hxx{foo}}'.
          //
          next (t, tt);
          parse_names (t, tt, ns);

          if (tt != type::rcbrace)
          {
            error (t) << "expected '}' instead of " << t << endl;
            throw parser_error ();
          }

          next (t, tt);
          continue;
        }

        // This is a target, directory, or variable name.
        //cout << "name: " << name << endl;
        ns.push_back (name);
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
