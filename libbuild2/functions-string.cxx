// file      : libbuild2/functions-string.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  string_functions (function_map& m)
  {
    function_family f (m, "string");

    f["string"] += [](string s)  {return s;};

    // @@ Shouldn't it concatenate elements into the single string?
    // @@ Doesn't seem to be used so far. Can consider removing.
    //
    // f["string"] += [](strings v) {return v;};

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

    // Trim.
    //
    f["trim"] += [](string s)
    {
      return trim (move (s));
    };

    f[".trim"] += [](names s)
    {
      return names {name (trim (convert<string> (move (s))))};
    };

    // Convert ASCII strings into lower/upper case.
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
    //
    // Return the number of elements in the sequence.
    //
    f["size"] += [] (strings v) {return v.size ();};

    // $sort(<strings> [, <flags>])
    //
    // Sort strings in ascending order.
    //
    // The following flags are supported:
    //
    //   icase - sort ignoring case
    //
    //   dedup - in addition to sorting also remove duplicates
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
            throw invalid_argument ("invalid flag '" + s + "'");
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

    // String-specific overloads from builtins.
    //
    function_family b (m, "builtin");

    b[".concat"] += [](string l, string r) {l += r; return l;};

    b[".concat"] += [](string l, names ur)
    {
      l += convert<string> (move (ur));
      return l;
    };

    b[".concat"] += [](names ul, string r)
    {
      string l (convert<string> (move (ul)));
      l += r;
      return l;
    };
  }
}
