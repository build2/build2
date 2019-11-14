// file      : libbuild2/name.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/types.hxx> // Note: not <libbuild2/name.hxx>

#include <string.h> // strchr()

#include <libbuild2/diagnostics.hxx>

namespace build2
{
  const name empty_name;
  const names empty_names;

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
      r += n.proj->string ();
      r += '%';
    }

    // If the value is empty, then we want to put the last component of the
    // directory inside {}, e.g., dir{bar/}, not bar/dir{}.
    //
    bool v (!n.value.empty ());
    bool t (!n.type.empty ());

    const dir_path& pd (v ? n.dir              :
                        t ? n.dir.directory () :
                        dir_path ());

    if (!pd.empty ())
      r += pd.representation ();

    if (t)
    {
      r += n.type;
      r += '{';
    }

    if (v)
      r += n.value;
    else
      r += (pd.empty () ? n.dir : n.dir.leaf ()).representation ();

    if (t)
      r += '}';

    return r;
  }

  ostream&
  to_stream (ostream& os, const name& n, bool quote, char pair, bool escape)
  {
    auto write_string = [quote, pair, escape, &os](const string& v)
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
        if (escape) os << '\\';
        os << '"';

        for (auto c: v)
        {
          if (strchr ("\\$(\"", c) != nullptr) // Special inside double quotes.
            os << '\\';

          os << c;
        }

        if (escape) os << '\\';
        os << '"';
      }
      else if (quote && v.find_first_of (sc) != string::npos)
      {
        if (escape) os << '\\';
        os << '\'';

        os << v;

        if (escape) os << '\\';
        os << '\'';
      }
      else
        os << v;
    };

    uint16_t dv (stream_verb (os).path); // Directory verbosity.

    auto write_dir = [dv, quote, &os, &write_string] (const dir_path& d)
    {
      if (quote)
        write_string (dv < 1 ? diag_relative (d) : d.representation ());
      else
        os << d;
    };

    // Note: similar to to_string() below.
    //

    // If quoted then print empty name as '' rather than {}.
    //
    if (quote && n.empty ())
      return os << (escape ? "\\'\\'" : "''");

    if (n.proj)
    {
      write_string (n.proj->string ());
      os << '%';
    }

    // If the value is empty, then we want to print the last component of the
    // directory inside {}, e.g., dir{bar/}, not bar/dir{}. We also want to
    // print {} for an empty name (unless quoted, which is handled above).
    //
    bool d (!n.dir.empty ());
    bool v (!n.value.empty ());
    bool t (!n.type.empty ());

    // Note: relative() may return empty.
    //
    const dir_path& rd (dv < 1 ? relative (n.dir) : n.dir); // Relative.
    const dir_path& pd (v ? rd              :
                        t ? rd.directory () :
                        dir_path ());

    if (!pd.empty ())
      write_dir (pd);

    if (t || (!d && !v))
    {
      if (t)
        write_string (n.type);

      os << '{';
    }

    if (v)
      write_string (n.value);
    else if (d)
    {
      if (rd.empty ())
        write_string (dir_path (".").representation ());
      else if (!pd.empty ())
        write_string (rd.leaf ().representation ());
      else
        write_dir (rd);
    }

    if (t || (!d && !v))
      os << '}';

    return os;
  }

  ostream&
  to_stream (ostream& os,
             const names_view& ns,
             bool quote,
             char pair,
             bool escape)
  {
    for (auto i (ns.begin ()), e (ns.end ()); i != e; )
    {
      const name& n (*i);
      ++i;
      to_stream (os, n, quote, pair, escape);

      if (n.pair)
        os << n.pair;
      else if (i != e)
        os << ' ';
    }

    return os;
  }
}
