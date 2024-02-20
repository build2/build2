// file      : libbuild2/json.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/json.hxx>

#include <limits>

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/parser.hxx>
#  include <libbutl/json/serializer.hxx>
#endif

namespace build2
{
  // json_event
  //
#ifndef BUILD2_BOOTSTRAP
  const char*
  to_string (json_event e)
  {
    switch (e)
    {
    case json_event::begin_object: return "beginning of object";
    case json_event::end_object:   return "end of object";
    case json_event::begin_array:  return "beginning of array";
    case json_event::end_array:    return "end of array";
    case json_event::name:         return "member name";
    case json_event::string:       return "string value";
    case json_event::number:       return "numeric value";
    case json_event::boolean:      return "boolean value";
    case json_event::null:         return "null value";
    }

    return "";
  }
#endif

  // json_type
  //
  const char*
  to_string (json_type t, bool dn) noexcept
  {
    using type = json_type;

    switch (t)
    {
    case type::null:               return "null";
    case type::boolean:            return "boolean";
    case type::signed_number:      return dn ?      "signed number" : "number";
    case type::unsigned_number:    return dn ?    "unsigned number" : "number";
    case type::hexadecimal_number: return dn ? "hexadecimal number" : "number";
    case type::string:             return "string";
    case type::array:              return "array";
    case type::object:             return "object";
    }
    return "";
  }

  // json_value
  //
  const json_value null_json_value (json_type::null);

  [[noreturn]] void
  json_as_throw (json_type t, json_type e)
  {
    string m;
    m = "expected ";
    m += to_string (e, true);
    m += " instead of ";
    m += to_string (t, true);
    throw invalid_argument (move (m));
  }

  [[noreturn]] static void
  at_throw (json_type t, json_type e, bool index)
  {
    string m;

    if (t != e && t != json_type::null)
    {
      m = "expected ";
      m += to_string (e, true);
      m += " instead of ";
      m += to_string (t, true);
      throw invalid_argument (move (m));
    }
    else
    {
      m = index ? "index" : "name";
      m += " out of range in ";
      m += to_string (e, true);
      throw std::out_of_range (move (m));
    }
  }

  const json_value& json_value::
  at (size_t index) const
  {
    if (type == json_type::array)
    {
      if (index < array.size ())
        return array[index];
    }

    at_throw (type, json_type::array, true);
  }

  json_value& json_value::
  at (size_t index)
  {
    if (type == json_type::array)
    {
      if (index < array.size ())
        return array[index];
    }

    at_throw (type, json_type::array, true);
  }

#if 0
  const json_value& json_value::
  operator[] (size_t index) const
  {
    if (type == json_type::null)
      return null_json_value;

    if (type == json_type::array)
      return index < array.size () ? array[index] : null_json_value;

    at_throw (type, json_type::array, true);
  }

  json_value& json_value::
  operator[] (size_t index)
  {
    if (type == json_type::null)
    {
      new (&array) array_type ();
      type = json_type::array;
    }

    if (type == json_type::array)
    {
      size_t n (array.size ());

      if (index < n)
        return array[index];

      // If there are missing elements in between, fill them with nulls.
      //
      if (index != n)
        array.resize (index, json_value ());

      array.push_back (json_value ());
      return array.back ();
    }

    at_throw (type, json_type::array, true);
  }
#endif

  const json_value& json_value::
  at (const char* name) const
  {
    if (type == json_type::object)
    {
      auto i (find_if (object.begin (), object.end (),
                       [name] (const json_member& m)
                       {
                         return m.name == name;
                       }));

      if (i != object.end ())
        return i->value;
    }

    at_throw (type, json_type::object, false);
  }

  json_value& json_value::
  at (const char* name)
  {
    if (type == json_type::object)
    {
      auto i (find_if (object.begin (), object.end (),
                       [name] (const json_member& m)
                       {
                         return m.name == name;
                       }));

      if (i != object.end ())
        return i->value;
    }

    at_throw (type, json_type::object, false);
  }

  const json_value* json_value::
  find (const char* name) const
  {
    if (type == json_type::object)
    {
      auto i (find_if (object.begin (), object.end (),
                       [name] (const json_member& m)
                       {
                         return m.name == name;
                       }));
      return i != object.end () ? &i->value : nullptr;
    }

    at_throw (type, json_type::object, false);
  }

  json_value* json_value::
  find (const char* name)
  {
    if (type == json_type::object)
    {
      auto i (find_if (object.begin (), object.end (),
                       [name] (const json_member& m)
                       {
                         return m.name == name;
                       }));

      return i != object.end () ? &i->value : nullptr;
    }

    at_throw (type, json_type::object, false);
  }

#if 0
  const json_value& json_value::
  operator[] (const char* name) const
  {
    if (type == json_type::null)
      return null_json_value;

    if (type == json_type::object)
    {
      auto i (find_if (object.begin (), object.end (),
                       [name] (const json_member& m)
                       {
                         return m.name == name;
                       }));


      return i != object.end () ? i->value : null_json_value;
    }

    at_throw (type, json_type::object, false);
  }

  json_value& json_value::
  operator[] (const char* name)
  {
    if (type == json_type::null)
    {
      new (&object) object_type ();
      type = json_type::object;
    }

    if (type == json_type::object)
    {
      auto i (find_if (object.begin (), object.end (),
                       [name] (const json_member& m)
                       {
                         return m.name == name;
                       }));

      if (i != object.end ())
        return i->value;

      object.push_back (json_member {name, json_value ()});
      return object.back ().value;
    }

    at_throw (type, json_type::object, false);
  }
#endif

  int json_value::
  compare (const json_value& v) const noexcept
  {
    int r (0);
    {
      // Note: we need to treat unsigned and hexadecimal the same.
      //
      json_type t (type == json_type::hexadecimal_number
                   ? json_type::unsigned_number
                   : type);

      json_type vt (v.type == json_type::hexadecimal_number
                    ? json_type::unsigned_number
                    : v.type);

      if (t != vt)
      {
        // Handle the special signed/unsigned number case here.
        //
        if (t == json_type::signed_number &&
            vt == json_type::unsigned_number)
        {
          if (signed_number < 0)
            r = -1;
          else
          {
            uint64_t u (static_cast<uint64_t> (signed_number));
            r = u < v.unsigned_number ? -1 : (u > v.unsigned_number ? 1 : 0);
          }
        }
        else if (t == json_type::unsigned_number &&
                 vt == json_type::signed_number)
        {
          if (v.signed_number < 0)
            r = 1;
          else
          {
            uint64_t u (static_cast<uint64_t> (v.signed_number));
            r = unsigned_number < u ? -1 : (unsigned_number > u ? 1 : 0);
          }
        }
        else
          r = (static_cast<uint8_t> (t) < static_cast<uint8_t> (vt) ? -1 : 1);
      }
    }

    if (r == 0)
    {
      switch (type)
      {
      case json_type::null:
        {
          r = 0;
          break;
        }
      case json_type::boolean:
        {
          r = boolean == v.boolean ? 0 : boolean ? 1 : -1;
          break;
        }
      case json_type::signed_number:
        {
          r = (signed_number < v.signed_number
               ? -1
               : (signed_number > v.signed_number ? 1 : 0));
          break;
        }
      case json_type::unsigned_number:
      case json_type::hexadecimal_number:
        {
          r = (unsigned_number < v.unsigned_number
               ? -1
               : (unsigned_number > v.unsigned_number ? 1 : 0));
          break;
        }
      case json_type::string:
        {
          r = string.compare (v.string);
          break;
        }
      case json_type::array:
        {
          auto i (array.begin ()), ie (array.end ());
          auto j (v.array.begin ()), je (v.array.end ());

          for (; i != ie && j != je; ++i, ++j)
          {
            if ((r = i->compare (*j)) != 0)
              break;
          }

          if (r == 0)
            r = i == ie ? (j == je ? 0 : -1) : 1; // More elements than other?

          break;
        }
      case json_type::object:
        {
          // We don't expect there to be a large number of members so it makes
          // sense to iterate in the lexicographical order without making any
          // copies.
          //
          auto next = [] (object_type::const_iterator p, // == e for first
                          object_type::const_iterator b,
                          object_type::const_iterator e)
          {
            // We need to find an element with the "smallest" name that is
            // greater than the previous entry.
            //
            auto n (e);

            for (auto i (b); i != e; ++i)
            {
              if (p == e || i->name > p->name)
              {
                int r;
                if (n == e || (r = n->name.compare (i->name)) > 0)
                  n = i;
                else
                  assert (r != 0); // No duplicates.
              }
            }

            return n;
          };

          auto ib (object.begin ()), ie (object.end ()), i (ie);
          auto jb (v.object.begin ()), je (v.object.end ()), j (je);

          for (;;)
          {
            // Note: we must call next() on both.
            //
            i = next (i, ib, ie);
            j = next (j, jb, je);

            if (i == ie || j == je)
              break;

            // Determine if both have this name and if not, which name comes
            // first.
            //
            int n (i->name.compare (j->name));

            r = (n < 0                   // If i's first, then i is greater.
                 ? -1
                 : (n > 0                // If j's first, then j is greater.
                    ? 1
                    : i->value.compare (j->value))); // Both have this name.

            if (r != 0)
              break;
          }

          if (r == 0)
            r = i == ie ? (j == je ? 0 : -1) : 1; // More members than other?

          break;
        }
      }
    }

    return r;
  }

  static void
  append_numbers (json_value& l, const json_value& r) noexcept
  {
    auto append = [&l] (uint64_t u, int64_t s, bool hex = false)
    {
      if (s < 0)
      {
        // The absolute value of a minimum signed intereger is not
        // representable in the 2s complement integers. So handle this
        // specially for completeness.
        //
        uint64_t a (
          s != std::numeric_limits<int64_t>::min ()
          ? static_cast<uint64_t> (-s)
          : static_cast<uint64_t> (std::numeric_limits<int64_t>::max ()) + 1);

        if (u >= a)
        {
          l.unsigned_number = u - a;
          l.type = (hex
                    ? json_type::hexadecimal_number
                    : json_type::unsigned_number);
        }
        else
        {
          l.signed_number = -static_cast<int64_t> (a - u);
          l.type = json_type::signed_number;
        }
      }
      else
      {
        l.unsigned_number = u + static_cast<uint64_t> (s);
        l.type = (hex
                  ? json_type::hexadecimal_number
                  : json_type::unsigned_number);
      }
    };

    // We try to keep LHS hex if possible.
    //
    if (l.type == json_type::signed_number)
    {
      if (r.type == json_type::signed_number)
      {
        // Deal with non-negative signed numbers for completeness.
        //
        if (l.signed_number >= 0)
          append (static_cast <uint64_t> (l.signed_number), r.signed_number);
        else if (r.signed_number >= 0)
          append (static_cast <uint64_t> (r.signed_number), l.signed_number);
        else
          l.signed_number += r.signed_number;
      }
      else
        append (r.unsigned_number, l.signed_number);
    }
    else
    {
      if (r.type == json_type::signed_number)
        append (l.unsigned_number,
                r.signed_number,
                l.type == json_type::hexadecimal_number);
      else
        l.unsigned_number += r.unsigned_number;
    }
  }

  void json_value::
  append (json_value&& v, bool override)
  {
    if (type == json_type::null)
    {
      *this = move (v);
      return;
    }
    else if (type == json_type::array)
    {
      if (v.type == json_type::array)
      {
        if (array.empty ())
          array = move (v.array);
        else
          array.insert (array.end (),
                        make_move_iterator (v.array.begin ()),
                        make_move_iterator (v.array.end ()));
      }
      else
        array.push_back (move (v));

      return;
    }
    else
    {
      switch (v.type)
      {
      case json_type::null: return;
      case json_type::boolean:
        {
          if (type != json_type::boolean)
            break;

          boolean = boolean || v.boolean;
          return;
        }
      case json_type::signed_number:
      case json_type::unsigned_number:
      case json_type::hexadecimal_number:
        {
          if (type != json_type::signed_number   &&
              type != json_type::unsigned_number &&
              type != json_type::hexadecimal_number)
            break;

          append_numbers (*this, v);
          return;
        }
      case json_type::string:
        {
          if (type != json_type::string)
            break;

          string += v.string;
          return;
        }
      case json_type::array: break;
      case json_type::object:
        {
          if (type != json_type::object)
            break;

          if (object.empty ())
            object = move (v.object);
          else
          {
            for (json_member& m: v.object)
            {
              auto i (find_if (object.begin (), object.end (),
                               [&m] (const json_member& o)
                               {
                                 return m.name == o.name;
                               }));
              if (i == object.end ())
                object.push_back (move (m));
              else if (override)
                i->value = move (m.value);
            }
          }

          return;
        }
      }
    }

    throw invalid_argument (
      string_type ("unable to append ") + to_string (v.type) + " to " +
      to_string (type));
  }

  void json_value::
  prepend (json_value&& v, bool override)
  {
    if (type == json_type::null)
    {
      *this = move (v);
      return;
    }
    else if (type == json_type::array)
    {
      if (v.type == json_type::array)
      {
        if (array.empty ())
          array = move (v.array);
        else
          array.insert (array.begin (),
                        make_move_iterator (v.array.begin ()),
                        make_move_iterator (v.array.end ()));
      }
      else
        array.insert (array.begin (), move (v));

      return;
    }
    else
    {
      switch (v.type)
      {
      case json_type::null: return;
      case json_type::boolean:
        {
          if (type != json_type::boolean)
            break;

          boolean = boolean || v.boolean;
          return;
        }
      case json_type::signed_number:
      case json_type::unsigned_number:
      case json_type::hexadecimal_number:
        {
          if (type != json_type::signed_number   &&
              type != json_type::unsigned_number &&
              type != json_type::hexadecimal_number)
            break;

          append_numbers (*this, v);
          return;
        }
      case json_type::string:
        {
          if (type != json_type::string)
            break;

          string.insert (0, v.string);
          return;
        }
      case json_type::array: break;
      case json_type::object:
        {
          if (type != json_type::object)
            break;

          if (object.empty ())
            object = move (v.object);
          else
          {
            for (json_member& m: v.object)
            {
              auto i (find_if (object.begin (), object.end (),
                               [&m] (const json_member& o)
                               {
                                 return m.name == o.name;
                               }));
              if (i == object.end ())
                object.insert (object.begin (), move (m));
              else if (override)
                i->value = move (m.value);
            }
          }

          return;
        }
      }
    }

    throw invalid_argument (
      string_type ("unable to prepend ") + to_string (v.type) + " to " +
      to_string (type));
  }

#ifndef BUILD2_BOOTSTRAP
  json_value::
  json_value (json_parser& p, optional<json_type> et)
  {
    using namespace butl::json;

    // A JSON input text cannot be empty.
    //
    // Once we have JSON5 support we will be able to distinguish hexadecimal
    // numbers.
    //
    json_type t (json_type::null);
    switch (*p.next ())
    {
    case event::begin_object: t = json_type::object;  break;
    case event::begin_array:  t = json_type::array;   break;
    case event::string:       t = json_type::string;  break;
    case event::number:       t = (p.value ()[0] == '-'
                                   ? json_type::signed_number
                                   : json_type::unsigned_number); break;
    case event::boolean:      t = json_type::boolean; break;
    case event::null:         t = json_type::null;    break;
    case event::name:
    case event::end_array:
    case event::end_object:
      {
        assert (false);
        type = json_type::null;
        return;
      }
    }

    if (et && *et != t)
    {
      throw invalid_json_input (
        p.input_name != nullptr ? p.input_name : "",
        p.line (),
        p.column (),
        p.position (),
        string_type ("expected ") + to_string (*et, true) + " instead of " +
        to_string (t, true));
    }

    switch (t)
    {
    case json_type::object:
      {
        object_type o; // For exception safety.
        while (*p.next () != event::end_object)
        {
          string_type n (p.name ());

          // Check for duplicates. For now we fail but in the future we may
          // provide a mode (via a flag) to override instead.
          //
          if (find_if (o.begin (), o.end (),
                       [&n] (const json_member& m)
                       {
                         return m.name == n;
                       }) != o.end ())
          {
            throw invalid_json_input (
              p.input_name != nullptr ? p.input_name : "",
              p.line (),
              p.column (),
              p.position (),
              "duplicate object member '" + n + '\'');
          }

          o.push_back (json_member {move (n), json_value (p)});
        }

        new (&object) object_type (move (o));
        type = t;
        break;
      }
    case json_type::array:
      {
        array_type c; // For exception safety.
        while (*p.peek () != event::end_array)
          c.push_back (json_value (p));
        p.next (); // Consume end_array.

        new (&array) array_type (move (c));
        type = t;
        break;
      }
    case json_type::string:
      {
        string_type& s (p.value ());

        // Don't move if small string optimized.
        //
        if (s.size () > 15)
          new (&string) string_type (move (s));
        else
          new (&string) string_type (s);

        type = t;
        break;
      }
    case json_type::signed_number:
      {
        signed_number = p.value<int64_t> ();
        type = t;
        break;
      }
    case json_type::unsigned_number:
    case json_type::hexadecimal_number:
      {
        unsigned_number = p.value<uint64_t> ();
        type = t;
        break;
      }
    case json_type::boolean:
      {
        boolean = p.value<bool> ();
        type = t;
        break;
      }
    case json_type::null:
      {
        type = t;
        break;
      }
    }
  }

  void json_value::
  serialize (json_buffer_serializer& s, optional<json_type> et) const
  {
    using namespace butl::json;

    if (et && *et != type)
    {
      throw invalid_json_output (
        nullopt,
        invalid_json_output::error_code::invalid_value,
        string_type ("expected ") + to_string (*et, true) + " instead of " +
        to_string (type, true));
    }

    switch (type)
    {
    case json_type::null:
      {
        s.value (nullptr);
        break;
      }
    case json_type::boolean:
      {
        s.value (boolean);
        break;
      }
    case json_type::signed_number:
      {
        s.value (signed_number);
        break;
      }
    case json_type::unsigned_number:
    case json_type::hexadecimal_number:
      {
        // When we have JSON5 support, we will be able to serialize
        // hexadecimal properly.
        //
        s.value (unsigned_number);
        break;
      }
    case json_type::string:
      {
        s.value (string);
        break;
      }
    case json_type::array:
      {
        s.begin_array ();
        for (const json_value& e: array)
          e.serialize (s);
        s.end_array ();
        break;
      }
    case json_type::object:
      {
        s.begin_object ();
        for (const json_member& m: object)
        {
          s.member_name (m.name);
          m.value.serialize (s);
        }
        s.end_object ();
        break;
      }
    }
  }

#else
  json_value::
  json_value (json_parser&, optional<json_type>)
  {
    assert (false);
    type = json_type::null;
  }

  void json_value::
  serialize (json_buffer_serializer&, optional<json_type>) const
  {
    assert (false);
  }
#endif
}
