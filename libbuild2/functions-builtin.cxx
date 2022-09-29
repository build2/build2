// file      : libbuild2/functions-builtin.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <sstream>

#include <libbuild2/scope.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  // Note: not static since used by type-specific sort() implementations.
  //
  bool
  functions_sort_flags (optional<names> fs)
  {
    bool r (false);
    if (fs)
    {
      for (name& f: *fs)
      {
        string s (convert<string> (move (f)));

        if (s == "dedup")
          r = true;
        else
          throw invalid_argument ("invalid flag '" + s + "'");
      }
    }
    return r;
  };

  static const char hex_digits[] = "0123456789abcdef";

  static string
  to_string (uint64_t i, optional<value> base, optional<value> width)
  {
    uint64_t b (base ? convert<uint64_t> (move (*base)) : 10);
    size_t w (width
              ? static_cast<size_t> (convert<uint64_t> (move (*width)))
              : 0);

    // One day we can switch to C++17 std::to_chars().
    //
    string r;
    switch (b)
    {
    case 10:
      {
        r = to_string (i);
        if (w > r.size ())
          r.insert (0, w - r.size (), '0');
        break;
      }
    case 16:
      {
        r.reserve (18);
        r += "0x";

        for (size_t j (64); j != 0; )
        {
          j -= 4;
          size_t d ((i >> j) & 0x0f);

          // Omit leading zeros but watch out for the i==0 corner case.
          //
          if (d != 0 || r.size () != 2 || j == 0)
            r += hex_digits[d];
        }

        if (w > r.size () - 2)
          r.insert (2, w - (r.size () - 2), '0');

        break;
      }
    default:
      throw invalid_argument ("unsupported base");
    }

    return r;
  }

  void
  builtin_functions (function_map& m)
  {
    function_family f (m, "builtin");

    // Note that we may want to extend the scope argument to a more general
    // notion of "lookup context" (scope, target, prerequisite).
    //
    // Note that this function is not pure.
    //
    f.insert ("defined", false) += [](const scope* s, names name)
    {
      if (s == nullptr)
        fail << "defined() called out of scope" << endf;

      return (*s)[convert<string> (move (name))].defined ();
    };

    // Return variable visibility if it has been entered and NULL otherwise.
    //
    // Note that this function is not pure.
    //
    f.insert ("visibility", false) += [](const scope* s, names name)
    {
      if (s == nullptr)
        fail << "visibility() called out of scope" << endf;

      const variable* var (
        s->ctx.var_pool.find (convert<string> (move (name))));

      return (var != nullptr
              ? optional<string> (to_string (var->visibility))
              : nullopt);
    };

    f["type"] += [](value* v) {return v->type != nullptr ? v->type->name : "";};
    f["null"] += [](value* v) {return v->null;};
    f["empty"] += [](value* v)  {return v->null || v->empty ();};

    f["identity"] += [](value* v) {return move (*v);};

    // $integer_sequence(<begin>, <end>[, <step>])
    //
    // Return the list of uint64 integers starting from <begin> (including) to
    // <end> (excluding) with the specified <step> or 1 if unspecified. If
    // <begin> is greater than <end>, empty list is returned.
    //
    // Note that currently negative numbers are not supported but this could
    // be handled if required (e.g., by returning int64s in this case).
    //
    // Note also that we could improve this by adding a shortcut to get the
    // indexes of a list (for example, $indexes(<list>) plus potentially a
    // similar $keys() function for maps).
    //
    f["integer_sequence"] += [](value begin, value end, optional<value> step)
    {
      uint64_t b (convert<uint64_t> (move (begin)));
      uint64_t e (convert<uint64_t> (move (end)));
      uint64_t s (step ? convert<uint64_t> (move (*step)) : 1);

      uint64s r;
      if (b < e)
      {
        r.reserve (static_cast<size_t> ((e - b) / s + 1));

        for (; b < e; b += s)
          r.push_back (static_cast<size_t> (b));
      }

      return r;
    };

    // string
    //
    f["string"] += [](bool b) {return b ? "true" : "false";};
    f["string"] += [](int64_t i) {return to_string (i);};
    f["string"] += [](uint64_t i, optional<value> base, optional<value> width)
    {
      return to_string (i, move (base), move (width));
    };

    // Quote a value returning its string representation. If escape is true,
    // then also escape (with a backslash) the quote characters being added
    // (this is useful if the result will be re-parsed, for example as a
    // Testscript command line).
    //
    f["quote"] += [](value* v, optional<value> escape)
    {
      if (v->null)
        return string ();

      untypify (*v); // Reverse to names.

      ostringstream os;
      to_stream (os,
                 v->as<names> (),
                 quote_mode::normal,
                 '@'  /* pair */,
                 escape && convert<bool> (move (*escape)));
      return os.str ();
    };

    // $size(<ints>)
    //
    // Return the number of elements in the sequence.
    //
    f["size"] += [] (int64s v) {return v.size ();};
    f["size"] += [] (uint64s v) {return v.size ();};

    // $sort(<ints> [, <flags>])
    //
    // Sort integers in ascending order.
    //
    // The following flags are supported:
    //
    //   dedup - in addition to sorting also remove duplicates
    //
    f["sort"] += [](int64s v, optional<names> fs)
    {
      sort (v.begin (), v.end ());

      if (functions_sort_flags (move (fs)))
        v.erase (unique (v.begin(), v.end()), v.end ());

      return v;
    };

    f["sort"] += [](uint64s v, optional<names> fs)
    {
      sort (v.begin (), v.end ());

      if (functions_sort_flags (move (fs)))
        v.erase (unique (v.begin(), v.end()), v.end ());

      return v;
    };

    // $find(<ints>, <int>)
    //
    // Return true if the integer sequence contains the specified integer.
    //
    f["find"] += [](int64s vs, value v)
    {
      return find (vs.begin (), vs.end (),
                   convert<int64_t> (move (v))) != vs.end ();
    };

    f["find"] += [](uint64s vs, value v)
    {
      return find (vs.begin (), vs.end (),
                   convert<uint64_t> (move (v))) != vs.end ();
    };

    // $find_index(<ints>, <int>)
    //
    // Return the index of the first element in the integer sequence that is
    // equal to the specified integer or $size(<ints>) if none is found.
    //
    f["find_index"] += [](int64s vs, value v)
    {
      auto i (find (vs.begin (), vs.end (), convert<int64_t> (move (v))));
      return i != vs.end () ? i - vs.begin () : vs.size ();
    };

    f["find_index"] += [](uint64s vs, value v)
    {
      auto i (find (vs.begin (), vs.end (), convert<uint64_t> (move (v))));
      return i != vs.end () ? i - vs.begin () : vs.size ();
    };

    // getenv
    //
    // Return NULL if the environment variable is not set, untyped value
    // otherwise.
    //
    // Note that if the build result can be affected by the variable being
    // queried, then it should be reported with the config.environment
    // directive.
    //
    // Note that this function is not pure.
    //
    f.insert ("getenv", false) += [](names name)
    {
      optional<string> v (getenv (convert<string> (move (name))));

      if (!v)
        return value ();

      names r;
      r.emplace_back (to_name (move (*v)));
      return value (move (r));
    };
  }
}
