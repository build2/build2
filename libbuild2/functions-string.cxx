// file      : libbuild2/functions-string.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  // Look for the substring forwards in the [p, n) range.
  //
  static inline size_t
  find (const string& s, size_t p, const string& ss, bool ic)
  {
    size_t sn (ss.size ());

    for (size_t n (s.size ()); p != n; ++p)
    {
      if (n - p >= sn &&
          (ic
           ? icasecmp (ss, s.c_str () + p, sn)
           : s.compare (p, sn, ss)) == 0)
        return p;
    }

    return string::npos;
  }

  // Look for the substring backwards in the [0, n) range.
  //
  static inline size_t
  rfind (const string& s, size_t n, const string& ss, bool ic)
  {
    size_t sn (ss.size ());

    if (n >= sn)
    {
      n -= sn; // Don't consider characters out of range.

      for (size_t p (n);; )
      {
        if ((ic
             ? icasecmp (ss, s.c_str () + p, sn)
             : s.compare (p, sn, ss)) == 0)
          return p;

        if (--p == 0)
          break;
      }
    }

    return string::npos;
  }

  static bool
  contains (const string& s, value&& ssv, optional<names>&& fs)
  {
    bool ic (false), once (false);
    if (fs)
    {
      for (name& f: *fs)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          ic = true;
        else if (s == "once")
          once = true;
        else
          throw invalid_argument ("invalid flag '" + s + '\'');
      }
    }

    const string ss (convert<string> (move (ssv)));

    if (ss.empty ())
      throw invalid_argument ("empty substring");

    size_t p (find (s, 0, ss, ic));

    if (once && p != string::npos && p != rfind (s, s.size (), ss, ic))
      p = string::npos;

    return p != string::npos;
  }

  static bool
  starts_with (const string& s, value&& pfv, optional<names>&& fs)
  {
    bool ic (false);
    if (fs)
    {
      for (name& f: *fs)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          ic = true;
        else
          throw invalid_argument ("invalid flag '" + s + '\'');
      }
    }

    const string pf (convert<string> (move (pfv)));

    if (pf.empty ())
      throw invalid_argument ("empty prefix");

    return find (s, 0, pf, ic) == 0;
  }

  static bool
  ends_with (const string& s, value&& sfv, optional<names>&& fs)
  {
    bool ic (false);
    if (fs)
    {
      for (name& f: *fs)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          ic = true;
        else
          throw invalid_argument ("invalid flag '" + s + '\'');
      }
    }

    const string sf (convert<string> (move (sfv)));

    if (sf.empty ())
      throw invalid_argument ("empty suffix");

    size_t n (s.size ());
    size_t p (rfind (s, n, sf, ic));

    return p != string::npos && p + sf.size () == n;
  }

  static string
  replace (string&& s, value&& fv, value&& tv, optional<names>&& fs)
  {
    bool ic (false), fo (false), lo (false);
    if (fs)
    {
      for (name& f: *fs)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          ic = true;
        else if (s == "first_only")
          fo = true;
        else if (s == "last_only")
          lo = true;
        else
          throw invalid_argument ("invalid flag '" + s + '\'');
      }
    }

    string f (convert<string> (move (fv)));
    string t (convert<string> (move (tv)));

    if (f.empty ())
      throw invalid_argument ("empty <from> substring");

    if (!s.empty ())
    {
      // Note that we don't cache s.size () since the string size will be
      // changing as we are replacing. In fact, we may end up with an empty
      // string after a replacement.

      size_t fn (f.size ());

      if (fo || lo)
      {
        size_t p (lo ? rfind (s, s.size (), f, ic) : find (s, 0, f, ic));

        if (fo && lo && p != string::npos)
        {
          if (p != find (s, 0, f, ic))
            p = string::npos;
        }

        if (p != string::npos)
          s.replace (p, fn, t);
      }
      else
      {
        size_t tn (t.size ());

        for (size_t p (0); (p = find (s, p, f, ic)) != string::npos; p += tn)
          s.replace (p, fn, t);
      }
    }

    return move (s);
  }

  static size_t
  find_index (const strings& vs, value&& v, optional<names>&& fs)
  {
    bool ic (false);
    if (fs)
    {
      for (name& f: *fs)
      {
        string s (convert<string> (move (f)));

        if (s == "icase")
          ic = true;
        else
          throw invalid_argument ("invalid flag '" + s + '\'');
      }
    }

    auto i (find_if (vs.begin (), vs.end (),
                     [ic, y = convert<string> (move (v))] (const string& x)
                     {
                       return (ic ? icasecmp (x, y) : x.compare (y)) == 0;
                     }));

    return i != vs.end () ? i - vs.begin () : vs.size ();
  }

  void
  string_functions (function_map& m)
  {
    function_family f (m, "string");

    // Note: leave undocumented since there is no good reason for the user to
    // call this function (which would be converting string to string).
    //
    // Note that we must handle NULL values (relied upon by the parser
    // to provide conversion semantics consistent with untyped values).
    //
    f["string"] += [](string* s)
    {
      return s != nullptr ? move (*s) : string ();
    };

    // $string.icasecmp(<untyped>, <untyped>)
    // $icasecmp(<string>, <string>)
    //
    // Compare ASCII strings ignoring case and returning the boolean value.
    //
    f["icasecmp"] += [](string x, string y)
    {
      return icasecmp (x, y) == 0;
    };

    f["icasecmp"] += [](string x, names y)
    {
      return icasecmp (x, convert<string> (move (y))) == 0;
    };

    f["icasecmp"] += [](names x, string y)
    {
      return icasecmp (convert<string> (move (x)), y) == 0;
    };

    f[".icasecmp"] += [](names x, names y)
    {
      return icasecmp (convert<string> (move (x)),
                       convert<string> (move (y))) == 0;
    };

    // $string.contains(<untyped>, <untyped>[, <flags>])
    // $contains(<string>, <string>[, <flags>])
    //
    // Check if the string (first argument) contains the given substring
    // (second argument). The substring must not be empty.
    //
    // The following flags are supported:
    //
    //     icase  - compare ignoring case
    //
    //     once   - check if the substring occurs exactly once
    //
    // See also `$string.starts_with()`, `$string.ends_with()`,
    // `$regex.search()`.
    //
    f["contains"] += [](string s, value ss, optional<names> fs)
    {
      return contains (move (s), move (ss), move (fs));
    };

    f[".contains"] += [](names s, value ss, optional<names> fs)
    {
      return contains (convert<string> (move (s)), move (ss), move (fs));
    };

    // $string.starts_with(<untyped>, <untyped>[, <flags>])
    // $starts_with(<string>, <string>[, <flags>])
    //
    // Check if the string (first argument) begins with the given prefix
    // (second argument). The prefix must not be empty.
    //
    // The following flags are supported:
    //
    //     icase  - compare ignoring case
    //
    // See also `$string.contains()`.
    //
    f["starts_with"] += [](string s, value pf, optional<names> fs)
    {
      return starts_with (move (s), move (pf), move (fs));
    };

    f[".starts_with"] += [](names s, value pf, optional<names> fs)
    {
      return starts_with (convert<string> (move (s)), move (pf), move (fs));
    };

    // $string.ends_with(<untyped>, <untyped>[, <flags>])
    // $ends_with(<string>, <string>[, <flags>])
    //
    // Check if the string (first argument) ends with the given suffix (second
    // argument). The suffix must not be empty.
    //
    // The following flags are supported:
    //
    //     icase  - compare ignoring case
    //
    // See also `$string.contains()`.
    //
    f["ends_with"] += [](string s, value sf, optional<names> fs)
    {
      return ends_with (move (s), move (sf), move (fs));
    };

    f[".ends_with"] += [](names s, value sf, optional<names> fs)
    {
      return ends_with (convert<string> (move (s)), move (sf), move (fs));
    };

    // $string.replace(<untyped>, <from>, <to> [, <flags>])
    // $replace(<string>, <from>, <to> [, <flags>])
    //
    // Replace occurences of substring <from> with <to> in a string. The
    // <from> substring must not be empty.
    //
    // The following flags are supported:
    //
    //     icase       - compare ignoring case
    //
    //     first_only  - only replace the first match
    //
    //     last_only   - only replace the last match
    //
    //
    // If both `first_only` and `last_only` flags are specified, then <from>
    // is replaced only if it occurs in the string once.
    //
    // See also `$regex.replace()`.
    //
    f["replace"] += [](string s, value f, value t, optional<names> fs)
    {
      return replace (move (s), move (f), move (t), move (fs));
    };

    f[".replace"] += [](names s, value f, value t, optional<names> fs)
    {
      return names {
        name (
          replace (
            convert<string> (move (s)), move (f), move (t), move (fs)))};
    };

    // $string.trim(<untyped>)
    // $trim(<string>)
    //
    // Trim leading and trailing whitespaces in a string.
    //
    f["trim"] += [](string s)
    {
      return trim (move (s));
    };

    f[".trim"] += [](names s)
    {
      return names {name (trim (convert<string> (move (s))))};
    };

    // $string.lcase(<untyped>)
    // $string.ucase(<untyped>)
    // $lcase(<string>)
    // $ucase(<string>)
    //
    // Convert ASCII string into lower/upper case.
    //
    f["lcase"] += [](string s)
    {
      return lcase (move (s));
    };

    f[".lcase"] += [](names s)
    {
      return names {name (lcase (convert<string> (move (s))))};
    };

    f["ucase"] += [](string s)
    {
      return ucase (move (s));
    };

    f[".ucase"] += [](names s)
    {
      return names {name (ucase (convert<string> (move (s))))};
    };

    // $size(<strings>)
    // $size(<string-set>)
    // $size(<string-map>)
    // $size(<string>)
    //
    // First three forms: return the number of elements in the sequence.
    //
    // Fourth form: return the number of characters (bytes) in the string.
    //
    f["size"] += [] (strings v)             {return v.size ();};
    f["size"] += [] (set<string> v)         {return v.size ();};
    f["size"] += [] (map<string, string> v) {return v.size ();};
    f["size"] += [] (string v)              {return v.size ();};

    // $sort(<strings> [, <flags>])
    //
    // Sort strings in ascending order.
    //
    // The following flags are supported:
    //
    //     icase - sort ignoring case
    //
    //     dedup - in addition to sorting also remove duplicates
    //
    f["sort"] += [](strings v, optional<names> fs)
    {
      bool ic (false);
      bool dd (false);
      if (fs)
      {
        for (name& f: *fs)
        {
          string s (convert<string> (move (f)));

          if (s == "icase")
            ic = true;
          else if (s == "dedup")
            dd = true;
          else
            throw invalid_argument ("invalid flag '" + s + '\'');
        }
      }

      sort (v.begin (), v.end (),
            [ic] (const string& x, const string& y)
            {
              return (ic ? icasecmp (x, y) : x.compare (y)) < 0;
            });

      if (dd)
        v.erase (unique (v.begin(), v.end(),
                         [ic] (const string& x, const string& y)
                         {
                           return (ic ? icasecmp (x, y) : x.compare (y)) == 0;
                         }),
                 v.end ());

      return v;
    };

    // $find(<strings>, <string>[, <flags>])
    //
    // Return true if the string sequence contains the specified string.
    //
    // The following flags are supported:
    //
    //     icase - compare ignoring case
    //
    // See also `$regex.find_match()` and `$regex.find_search()`.
    //
    f["find"] += [](strings vs, value v, optional<names> fs)
    {
      return find_index (vs, move (v), move (fs)) != vs.size ();
    };

    // $find_index(<strings>, <string>[, <flags>])
    //
    // Return the index of the first element in the string sequence that
    // is equal to the specified string or `$size(strings)` if none is
    // found.
    //
    // The following flags are supported:
    //
    //     icase - compare ignoring case
    //
    f["find_index"] += [](strings vs, value v, optional<names> fs)
    {
      return find_index (vs, move (v), move (fs));
    };

    // $keys(<string-map>)
    //
    // Return the list of keys in a string map.
    //
    // Note that the result is sorted in ascending order.
    //
    f["keys"] += [](map<string, string> v)
    {
      strings r;
      r.reserve (v.size ());
      for (pair<const string, string>& p: v)
        r.push_back (p.first); // @@ PERF: use C++17 map::extract() to steal.
      return r;
    };

    // String-specific overloads from builtins.
    //
    function_family b (m, "builtin");

    // Note that we must handle NULL values (relied upon by the parser to
    // provide concatenation semantics consistent with untyped values).
    //
    b[".concat"] += [](string* l, string* r)
    {
      return l != nullptr
        ? r != nullptr ? move (*l += *r) : move (*l)
        : r != nullptr ? move (*r) : string ();
    };

    b[".concat"] += [](string* l, names* ur)
    {
      string r (ur != nullptr ? convert<string> (move (*ur)) : string ());
      return l != nullptr ? move (*l += r) : move (r);
    };

    b[".concat"] += [](names* ul, string* r)
    {
      string l (ul != nullptr ? convert<string> (move (*ul)) : string ());
      return r != nullptr ? move (l += *r) : move (l);
    };
  }
}
