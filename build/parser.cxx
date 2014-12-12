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
  typedef token_punctuation punc;

  void parser::
  parse (istream& is, const path& p)
  {
    lexer l (is, p.string (), diag_);
    lexer_ = &l;
    path_ = &p;

    token t (0, 0); // eos
    type tt;

    for (next (t, tt); tt != type::eos; )
    {
      // We always start with one or more names.
      //
      names (t, tt);

      if (t.is (punc::colon))
      {
        next (t, tt);

        if (tt == type::name || t.is (punc::lcbrace))
          names (t, tt);

        if (t.is (punc::newline))
          next (t, tt);
        else if (tt != type::eos)
        {
          error (t) << "expected newline insetad of " << t << endl;
          throw parser_error ();
        }

        continue;
      }

      error (t) << "unexpected " << t << endl;
      throw parser_error ();
    }
  }

  void parser::
  names (token& t, type& tt)
  {
    for (bool first (true);; first = false)
    {
      // Untyped name group, e.g., '{foo bar}'.
      //
      if (t.is (punc::lcbrace))
      {
        next (t, tt);
        names (t, tt);

        if (!t.is (punc::rcbrace))
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
        next (t, tt);

        if (t.is (punc::lcbrace))
        {
          //cout << "type: " << name << endl;

          //@@ TODO:
          //
          //   - detect nested typed name groups, e.g., 'cxx{hxx{foo}}'.
          //
          next (t, tt);
          names (t, tt);

          if (!t.is (punc::rcbrace))
          {
            error (t) << "expected '}' instead of " << t << endl;
            throw parser_error ();
          }

          next (t, tt);
          continue;
        }

        // This is a target, directory, or variable name.
        //cout << "name: " << name << endl;
        continue;
      }

      if (!first)
        break;

      error (t) << "expected name instead of " << t << endl;
      throw parser_error ();
    }
  }

  void parser::
  next (token& t, token_type& tt)
  {
    t = lexer_->next ();
    tt = t.type ();
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
    case token_type::eos: os << "<end-of-stream>"; break;
    case token_type::punctuation:
      {
        switch (t.punctuation ())
        {
        case token_punctuation::newline: os << "<newline>"; break;
        case token_punctuation::colon:   os << "':'"; break;
        case token_punctuation::lcbrace: os << "'{'"; break;
        case token_punctuation::rcbrace: os << "'}'"; break;
        }
        break;
      }
    case token_type::name: os << '\'' << t.name () << '\''; break;
    }

    return os;
  }
}
