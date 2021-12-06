// file      : libbuild2/make-parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/make-parser.hxx>

#include <libbuild2/diagnostics.hxx>

namespace build2
{
  auto make_parser::
  next (const string& l,
        size_t& p,
        const location& ll,
        bool strict) -> pair<type, path>
  {
    assert (state != end);

    pair<string, bool> r (
      next (l, p, !strict ? state == prereqs : optional<bool> ()));

    type t (state == prereqs ? type::prereq : type::target);

    // Deal with the end.
    //
    if (r.second)
    {
      if (state == begin && r.first.empty ())
        ; // Skip leading blank line.
      else
      {
        if (state != prereqs)
          fail (ll) << "end of make dependency declaration before ':'";

        state = end;
      }
    }
    // Deal with the first target.
    //
    else if (state == begin && !r.first.empty ())
      state = targets;

    // Deal with `:`.
    //
    if (p != l.size () && l[p] == ':')
    {
      switch (state)
      {
      case begin:   fail (ll) << "':' before make target";      break;
      case targets: state = prereqs;                            break;
      case prereqs: fail (ll) << "':' after make prerequisite"; break;
      case end:                                                 break;
      }

      if (++p == l.size ())
        state = end; // Not a mere optimization: the caller will get next line.
    }

    try
    {
      return pair<type, path> (t, path (move (r.first)));
    }
    catch (const invalid_path& e)
    {
      fail (ll) << "invalid make "
                << (t == type::prereq ? "prerequisite" : "target")
                << " path '" << e.path << "'" << endf;
    }
  }

  pair<string, bool> make_parser::
  next (const string& l, size_t& p, optional<bool> prereq)
  {
    size_t n (l.size ());

    // Skip leading spaces.
    //
    for (; p != n && l[p] == ' '; p++) ;

    // Lines containing multiple targets/prerequisites are customarily 80
    // characters max.
    //
    string r;
    r.reserve (n - p);

    // Scan the next target/prerequisite while watching out for escape
    // sequences.
    //
    // @@ Can't we do better for the (common) case where nothing is escaped?
    //
    for (char c, q (prereq && *prereq ? '\0' : ':');
         p != n && (c = l[p]) != ' ' && c != q; )
    {
      // If we have another character, then handle the escapes.
      //
      if (++p != n)
      {
        if (c == '\\')
        {
          // This may or may not be an escape sequence depending on whether
          // what follows is "escapable".
          //
          switch (c = l[p])
          {
          case '\\':
          case ' ':
          case ':': ++p; break;
          default: c = '\\'; // Restore.
          }
        }
        else if (c == '$')
        {
          // Got to be another (escaped) '$'.
          //
          if (l[p] == '$')
            ++p;
        }
      }
      // Note that the newline escape is not necessarily separated with space.
      //
      else if (c == '\\')
      {
        --p;
        break;
      }

      r += c;
    }

    // Skip trailing spaces.
    //
    for (; p != n && l[p] == ' '; p++) ;

    // Skip final '\' and determine if this is the end.
    //
    bool e (false);
    if (p == n - 1)
    {
      if (l[p] == '\\')
        p++;
    }
    else if (p == n)
      e = true;

    return pair<string, bool> (move (r), e);
  }
}
