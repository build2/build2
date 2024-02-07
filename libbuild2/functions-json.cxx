// file      : libbuild2/functions-json.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/parser.hxx>
#  include <libbutl/json/serializer.hxx>
#endif

using namespace std;

namespace build2
{
  static size_t
  find_index (const json_value& a, value v)
  {
    if (a.type != json_type::array)
      fail << "expected json array instead of " << to_string (a.type)
           << " as first argument";

    auto b (a.array.begin ()), e (a.array.end ());
    auto i (find (b, e, convert<json_value> (move (v))));
    return i != e ? i - b : a.array.size ();
  };

  void
  json_functions (function_map& m)
  {
    function_family f (m, "json");

    // $value_type(<json>[, <distinguish_numbers>])
    //
    // Return the type of a JSON value: `null`, `boolean`, `number`, `string`,
    // `array`, or `object`. If the <distinguish_numbers> argument is `true`,
    // then instead of `number` return `signed number`, `unsigned number`, or
    // `hexadecimal number`.
    //
    f["value_type"] += [] (json_value v, optional<value> distinguish_numbers)
    {
      bool dn (distinguish_numbers &&
               convert<bool> (move (*distinguish_numbers)));

      return to_string (v.type, dn);
    };

    // $member_name(<json>)
    //
    // Return the name of a JSON object member.
    //
    f["member_name"] += [] (json_value v)
    {
      // A member becomes an object with a single member (see json_reverse()
      // for details).
      //
      if (v.type == json_type::object && v.object.size () == 1)
        return move (v.object.front ().name);

      fail << "json object member expected instead of " << v.type << endf;
    };

    // $member_value(<json>)
    //
    // Return the value of a JSON object member.
    //
    f["member_value"] += [] (json_value v)
    {
      // A member becomes an object with a single member (see json_reverse()
      // for details).
      //
      if (v.type == json_type::object && v.object.size () == 1)
      {
        // Reverse simple JSON values to the corresponding fundamental type
        // values for consistency with subscript/iteration (see
        // json_subscript_impl() for background).
        //
        json_value& jr (v.object.front ().value);

        switch (jr.type)
        {
#if 0
        case json_type::null:               return value (names {});
#else
        case json_type::null:               return value ();
#endif
        case json_type::boolean:            return value (jr.boolean);
        case json_type::signed_number:      return value (jr.signed_number);
        case json_type::unsigned_number:
        case json_type::hexadecimal_number: return value (jr.unsigned_number);
        case json_type::string:             return value (move (jr.string));
        case json_type::array:
        case json_type::object:             return value (move (jr));
        }
      }

      fail << "json object member expected instead of " << v.type << endf;
    };

    // $size(<json>)
    //
    // Return the size of a JSON value.
    //
    // The size of a `null` value is `0`. The sizes of simple values
    // (`boolean`, `number`, and `string`) is `1`. The size of `array` and
    // `object` values is the number of elements and members, respectively.
    //
    // Note that the size of a `string` JSON value is not the length of the
    // string. To get the length call `$string.size()` instead by casting the
    // JSON value to the `string` value type.
    //
    f["size"] += [] (json_value v) -> size_t
    {
      // Note: should be consistent with value_traits<json_value>::empty(),
      //       json_subscript().
      //
      switch (v.type)
      {
      case json_type::null:               return 0;
      case json_type::boolean:
      case json_type::signed_number:
      case json_type::unsigned_number:
      case json_type::hexadecimal_number:
      case json_type::string:             break;
      case json_type::array:              return v.array.size ();
      case json_type::object:             return v.object.size ();
      }

      return 1;
    };

    // $find(<json-array>, <json>)
    //
    // Return true if the JSON array contains the specified JSON value.
    //
    f["find"] += [](json_value a, value v)
    {
      size_t i (find_index (a, move (v)));
      return i != a.array.size (); // We now know it's an array.
    };

    // $find_index(<json-array>, <json>)
    //
    // Return the index of the first element in the JSON array that is equal
    // to the specified JSON value or `$size(json-array)` if none is found.
    //
    f["find_index"] += [](json_value a, value v)
    {
      return find_index (a, move (v));
    };

#ifndef BUILD2_BOOTSTRAP

    // @@ Flag to support multi-value (returning it as JSON array)? Then
    //    probably also in $serialize().
    //
    // @@ Flag to override duplicates instead of failing?

    // $json.load(<path>)
    //
    // Parse the contents of the specified file as JSON input text and return
    // the result as a value of the `json` type.
    //
    // See also `$json.parse()`.
    //
    // Note that this function is not pure.
    //
    f.insert (".load", false) += [] (names xf)
    {
      path f (convert<path> (move (xf)));

      try
      {
        ifdstream is (f);
        json_parser p (is, f.string ());
        return json_value (p);
      }
      catch (const invalid_json_input& e)
      {
        fail (location (f, e.line, e.column)) << "invalid json input: " << e <<
          info << "byte offset " << e.position << endf;
      }
      catch (const io_error& e)
      {
        fail << "unable to read from " << f << ": " << e << endf;
      }
    };

    // $json.parse(<text>)
    //
    // Parse the specified JSON input text and return the result as a value of
    // the `json` type.
    //
    // See also `$json.load()` and `$json.serialize()`.
    //
    f[".parse"] += [] (names text)
    {
      string t (convert<string> (move (text)));

      try
      {
        json_parser p (t, nullptr /* name */);
        return json_value (p);
      }
      catch (const invalid_json_input& e)
      {
        fail << "invalid json input: " << e <<
          info << "line " << e.line
               << ", column " << e.column
               << ", byte offset " << e.position << endf;
      }
    };

    // $serialize(<json>[, <indentation>])
    //
    // Serialize the specified JSON value and return the resulting JSON output
    // text.
    //
    // The optional <indentation> argument specifies the number of indentation
    // spaces that should be used for pretty-printing. If `0` is passed, then
    // no pretty-printing is performed. The default is `2` spaces.
    //
    // See also `$json.parse()`.
    //
    f["serialize"] += [] (json_value v, optional<value> indentation)
    {
      uint64_t i (indentation ? convert<uint64_t> (*indentation) : 2);

      try
      {
        // For the diagnostics test.
        //
#if 0
        if (v.type == json_type::string && v.string == "deadbeef")
        {
          v.string[4] = 0xe0;
          v.string[5] = 0xe0;
        }
#endif

        string o;
        json_buffer_serializer s (o, i);
        v.serialize (s);
        return o;
      }
      catch (const invalid_json_output& e)
      {
        diag_record dr;
        dr << fail << "invalid json value: " << e;

        if (e.event)
          dr << info << "while serializing " << to_string (*e.event);

        if (e.offset != string::npos)
          dr << info << "offending byte offset " << e.offset;

        dr << endf;
      }
    };
#endif
  }
}
