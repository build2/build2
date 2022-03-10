// file      : libbuild2/name.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/types.hxx> // Note: not <libbuild2/name.hxx>

#include <string.h> // strchr()

#include <libbuild2/diagnostics.hxx>

namespace build2
{
  const name empty_name;
  const names empty_names;

  void name::
  canonicalize ()
  {
    // We cannot assume the name part is a valid filesystem name so we will
    // have to do the splitting manually.
    //
    size_t p (path_traits::rfind_separator (value));

    if (p != string::npos)
    {
      if (p + 1 == value.size ())
        throw invalid_argument ("empty value");

      dir /= dir_path (value, p != 0 ? p : 1); // Special case: "/".

      value.erase (0, p + 1);
    }
  }

  string
  to_string (const name& n)
  {
    assert (!n.pattern);

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
  to_stream (ostream& os, const name& n, quote_mode q, char pair, bool escape)
  {
    using pattern_type = name::pattern_type;

    auto write_string = [&os, q, pair, escape] (
      const string& v,
      optional<pattern_type> pat = nullopt,
      bool curly = false)
    {
      // We don't expect the effective quoting mode to be specified for the
      // name patterns.
      //
      assert (q != quote_mode::effective || !pat);

      // Special characters, path pattern characters, and regex pattern
      // characters. The latter only need to be quoted in the first position
      // and if followed by a non-alphanumeric delimiter. If that's the only
      // special character, then we handle it with escaping rather than
      // quoting (see the parsing logic for rationale). Additionally, we
      // escape leading `+` in the curly braces which is also recognized as a
      // path pattern.
      //
      char nsc[] = {
        '{', '}', '[', ']', '$', '(', ')', // Token endings.
        ' ', '\t', '\n', '#',              // Spaces.
        '\\', '"',                         // Escaping and quoting.
        '%',                               // Project name separator.
        pair,                              // Pair separator, if any.
        '\0'};

      char pc[] = {
        '*', '?',                          // Path wildcard characters.
        '\0'};

      auto rc = [] (const string& v)
      {
        return (v[0] == '~' || v[0] == '^') && v[1] != '\0' && !alnum (v[1]);
      };

      char esc[] = {
        '{', '}', '$', '(',   // Token endings.
        ' ', '\t', '\n', '#', // Spaces.
        '"',                  // Quoting.
        pair,                 // Pair separator, if any.
        '\0'};

      auto ec = [&esc] (const string& v)
      {
        for (size_t i (0); i < v.size (); ++i)
        {
          char c (v[i]);

          if (strchr (esc, c) != nullptr || (c == '\\' && v[i + 1] == '\\'))
            return true;
        }

        return false;
      };

      if (pat)
      {
        switch (*pat)
        {
        case pattern_type::path:                          break;
        case pattern_type::regex_pattern:      os << '~'; break;
        case pattern_type::regex_substitution: os << '^'; break;
        }
      }

      if (q != quote_mode::none && v.find ('\'') != string::npos)
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
      //
      // Note that a regex pattern does not need to worry about special path
      // pattern character but not vice-verse. See the parsing logic for
      // details.
      //
      else if ((q == quote_mode::normal &&
                (v.find_first_of (nsc) != string::npos ||
                 (!pat && v.find_first_of (pc) != string::npos))) ||
               (q == quote_mode::effective && ec (v)))
      {
        if (escape) os << '\\';
        os << '\'';

        os << v;

        if (escape) os << '\\';
        os << '\'';
      }
      // Note that currently we do not preserve a leading `+` as a pattern
      // unless it has other wildcard characters (see the parsing code for
      // details). So we escape it both if it's not a pattern or is a path
      // pattern.
      //
      else if (q == quote_mode::normal              &&
               (!pat || *pat == pattern_type::path) &&
               ((v[0] == '+' && curly) || rc (v)))
      {
        if (escape) os << '\\';
        os << '\\' << v;
      }
      else
        os << v;
    };

    uint16_t dv (stream_verb (os).path); // Directory verbosity.

    auto write_dir = [&os, q, &write_string, dv] (
      const dir_path& d,
      optional<pattern_type> pat = nullopt,
      bool curly = false)
    {
      if (q != quote_mode::none)
        write_string (dv < 1 ? diag_relative (d) : d.representation (),
                      pat,
                      curly);
      else
        os << d;
    };

    // Note: similar to to_string() above.
    //

    // If quoted then print empty name as '' rather than {}.
    //
    if (q != quote_mode::none && n.empty ())
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

    bool curly;
    if ((curly = t || (!d && !v)))
    {
      if (t)
        write_string (n.type);

      os << '{';
    }

    if (v)
      write_string (n.value, n.pattern, curly);
    else if (d)
    {
      // A directory pattern cannot be regex.
      //
      assert (!n.pattern || *n.pattern == pattern_type::path);

      if (rd.empty ())
        write_string (dir_path (".").representation (), nullopt, curly);
      else if (!pd.empty ())
        write_string (rd.leaf ().representation (), n.pattern, curly);
      else
        write_dir (rd, n.pattern, curly);
    }

    if (curly)
      os << '}';

    return os;
  }

  ostream&
  to_stream (ostream& os,
             const names_view& ns,
             quote_mode q,
             char pair,
             bool escape)
  {
    for (auto i (ns.begin ()), e (ns.end ()); i != e; )
    {
      const name& n (*i);
      ++i;
      to_stream (os, n, q, pair, escape);

      if (n.pair)
        os << n.pair;
      else if (i != e)
        os << ' ';
    }

    return os;
  }
}
