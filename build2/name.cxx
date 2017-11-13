// file      : build2/name.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/types.hxx> // Note: not <build2/names>

#include <string.h> // strchr()

#include <sstream>

#include <build2/diagnostics.hxx>

namespace build2
{
  string
  to_string (const name& n)
  {
    string r;

    // Note: similar to to_stream() below.
    //
    if (n.empty ())
      return r;

    if (n.proj)
    {
      r += *n.proj;
      r += '%';
    }

    // If the value is empty, then we want to put the directory inside {},
    // e.g., dir{bar/}, not bar/dir{}.
    //
    bool d (!n.dir.empty ());
    bool v (!n.value.empty ());
    bool t (!n.type.empty ());

    if (v && d)
      r += n.dir.representation ();

    if (t)
    {
      r += n.type;
      r += '{';
    }

    if (v)
      r += n.value;
    else
      r += n.dir.representation ();

    if (t)
      r += '}';

    return r;
  }

  ostream&
  to_stream (ostream& os, const name& n, bool quote, char pair)
  {
    auto write_string = [quote, pair, &os](const string& v)
    {
      char sc[] = {
        '{', '}', '[', ']', '$', '(', ')', // Token endings.
        ' ', '\t', '\n', '#',              // Spaces.
        '\\', '"',                         // Escaping and quoting.
        '%',                               // Project name separator.
        '*', '?',                          // Wildcard characters.
        pair,                              // Pair separator, if any.
        '\0'};

      if (quote && v.find ('\'') != string::npos)
      {
        // Quote the string with the double quotes rather than with the single
        // one. Escape some of the special characters.
        //
        os << '"';

        for (auto c: v)
        {
          if (strchr ("\\$(\"", c) != nullptr) // Special inside double quotes.
            os << '\\';

          os << c;
        }

        os << '"';
      }
      else if (quote && v.find_first_of (sc) != string::npos)
        os << "'" << v << "'";
      else
        os << v;
    };

    auto write_dir = [quote, &os, &write_string](const dir_path& d)
    {
      if (quote)
      {
        std::ostringstream s;
        stream_verb (s, stream_verb (os));
        s << d;

        write_string (s.str ());
      }
      else
        os << d;
    };

    // Note: similar to to_string() below.
    //

    // If quoted then print empty name as '' rather than {}.
    //
    if (quote && n.empty ())
      return os << "''";

    if (n.proj)
    {
      write_string (*n.proj);
      os << '%';
    }

    // If the value is empty, then we want to print the directory inside {},
    // e.g., dir{bar/}, not bar/dir{}. We also want to print {} for an empty
    // name (unless quoted).
    //
    bool d (!n.dir.empty ());
    bool v (!n.value.empty ());
    bool t (!n.type.empty () || (!d && !v));

    if (v)
      write_dir (n.dir);

    if (t)
    {
      write_string (n.type);
      os << '{';
    }

    if (v)
      write_string (n.value);
    else
      write_dir (n.dir);

    if (t)
      os << '}';

    return os;
  }

  ostream&
  to_stream (ostream& os, const names_view& ns, bool quote, char pair)
  {
    for (auto i (ns.begin ()), e (ns.end ()); i != e; )
    {
      const name& n (*i);
      ++i;
      to_stream (os, n, quote, pair);

      if (n.pair)
        os << n.pair;
      else if (i != e)
        os << ' ';
    }

    return os;
  }
}
