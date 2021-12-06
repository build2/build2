// file      : libbuild2/make-parser.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/make-parser.hxx>

#include <cstring> // strchr()

#include <libbuild2/diagnostics.hxx>

namespace build2
{
  auto make_parser::
  next (const string& l, size_t& p, const location& ll) -> pair<type, path>
  {
    assert (state != end);

    type t (state == prereqs ? type::prereq : type::target);

    pair<string, bool> r (next (l, p, t));

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

  // Note: backslash must be first.
  //
  // Note also that, at least in GNU make 4.1, `%` seems to be unescapable
  // if appears in a target and literal if in a prerequisite.
  //
  static const char escapable[] = "\\ :#";

  pair<string, bool> make_parser::
  next (const string& l, size_t& p, type)
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
#ifdef _WIN32
    size_t b (p);
#endif

    for (char c; p != n && (c = l[p]) != ' '; r += c)
    {
      if (c == ':')
      {
#ifdef _WIN32
        // See if this colon is part of the driver letter component in an
        // absolute Windows path.
        //
        // Note that here we assume we are not dealing with directories (in
        // which case c: would be a valid path) and thus an absolute path is
        // at least 4 characters long (e.g., c:\x).
        //
        if (p == b + 1   &&       // Colon is second character.
            alpha (l[b]) &&       // First is drive letter.
            p + 2 < n    &&       // At least two more characters after colon.
            ((l[p + 1] == '/') || // Next is directory separator.
             (l[p + 1] == '\\' && // But not part of a non-\\ escape sequence.
              strchr (escapable + 1, l[p + 2]) == nullptr)))
        {
          ++p;
          continue;
        }
#endif
        break;
      }

      // If we have another character, then handle the escapes.
      //
      if (++p != n)
      {
        if (c == '\\')
        {
          // This may or may not be an escape sequence depending on whether
          // what follows is "escapable".
          //
          if (strchr (escapable, l[p]) != nullptr)
            c = l[p++];
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
