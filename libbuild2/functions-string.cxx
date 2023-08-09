// file      : libbuild2/functions-string.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
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
  };

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
    // $size(<string>)
    //
    // First form: return the number of elements in the sequence.
    //
    // Second form: return the number of characters (bytes) in the string.
    //
    f["size"] += [] (strings v) {return v.size ();};
    f["size"] += [] (string v) {return v.size ();};

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
