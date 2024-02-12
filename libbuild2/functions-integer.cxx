// file      : libbuild2/functions-integer.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  extern bool
  functions_sort_flags (optional<names>); // functions-builtin.cxx

  static string
  to_string (uint64_t i, optional<value> base, optional<value> width)
  {
    int b (base ?
           static_cast<int> (convert<uint64_t> (move (*base)))
           : 10);

    size_t w (width
              ? static_cast<size_t> (convert<uint64_t> (move (*width)))
              : 0);

    return (to_string (i, b, w));
  }

  void
  integer_functions (function_map& m)
  {
    function_family f (m, "integer");

    // $string(<int64>)
    // $string(<uint64>[, <base>[, <width>]])
    //
    // Convert an integer to a string. For unsigned integers we can specify
    // the desired base and width. For example:
    //
    //     x = [uint64] 0x0000ffff
    //
    //     c.poptions += "-DOFFSET=$x"                 # -DOFFSET=65535
    //     c.poptions += "-DOFFSET=$string($x, 16)"    # -DOFFSET=0xffff
    //     c.poptions += "-DOFFSET=$string($x, 16, 8)" # -DOFFSET=0x0000ffff
    //

    // Note that we don't handle NULL values for these type since they have no
    // empty representation.
    //
    f["string"] += [](int64_t i) {return to_string (i);};

    f["string"] += [](uint64_t i, optional<value> base, optional<value> width)
    {
      return to_string (i, move (base), move (width));
    };

    // $integer_sequence(<begin>, <end>[, <step>])
    //
    // Return the list of uint64 integers starting from <begin> (including) to
    // <end> (excluding) with the specified <step> or `1` if unspecified. If
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
    //     dedup - in addition to sorting also remove duplicates
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
    // equal to the specified integer or `$size(ints)` if none is found.
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
  }
}
