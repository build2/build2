// file      : libbuild2/json.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  [[noreturn]] LIBBUILD2_SYMEXPORT void
  json_as_throw (json_type actual, json_type expected);

  inline bool json_value::
  as_bool () const
  {
    if (type == json_type::boolean)
      return boolean;

    json_as_throw (type, json_type::boolean);
  }

  inline bool& json_value::
  as_bool ()
  {
    if (type == json_type::boolean)
      return boolean;

    json_as_throw (type, json_type::boolean);
  }

  inline int64_t json_value::
  as_int64 () const
  {
    if (type == json_type::signed_number)
      return signed_number;

    json_as_throw (type, json_type::signed_number);
  }

  inline int64_t& json_value::
  as_int64 ()
  {
    if (type == json_type::signed_number)
      return signed_number;

    json_as_throw (type, json_type::signed_number);
  }

  inline uint64_t json_value::
  as_uint64 () const
  {
    if (type == json_type::unsigned_number ||
        type == json_type::hexadecimal_number)
      return unsigned_number;

    json_as_throw (type, json_type::unsigned_number);
  }

  inline uint64_t& json_value::
  as_uint64 ()
  {
    if (type == json_type::unsigned_number ||
        type == json_type::hexadecimal_number)
      return unsigned_number;

    json_as_throw (type, json_type::unsigned_number);
  }

  inline const string& json_value::
  as_string () const
  {
    if (type == json_type::string)
      return string;

    json_as_throw (type, json_type::string);
  }

  inline string& json_value::
  as_string ()
  {
    if (type == json_type::string)
      return string;

    json_as_throw (type, json_type::string);
  }

  inline const json_value::array_type& json_value::
  as_array () const
  {
    if (type == json_type::array)
      return array;

    json_as_throw (type, json_type::array);
  }

  inline json_value::array_type& json_value::
  as_array ()
  {
    if (type == json_type::array)
      return array;

    json_as_throw (type, json_type::array);
  }

  inline const json_value::object_type& json_value::
  as_object () const
  {
    if (type == json_type::object)
      return object;

    json_as_throw (type, json_type::object);
  }

  inline json_value::object_type& json_value::
  as_object ()
  {
    if (type == json_type::object)
      return object;

    json_as_throw (type, json_type::object);
  }

  inline json_value::
  ~json_value () noexcept
  {
    switch (type)
    {
    case json_type::null:
    case json_type::boolean:
    case json_type::signed_number:
    case json_type::unsigned_number:
    case json_type::hexadecimal_number:                         break;
    case json_type::string:             string.~string_type (); break;
    case json_type::array:              array.~array_type ();   break;
    case json_type::object:             object.~object_type (); break;
    }
  }

  inline json_value::
  json_value (json_type t) noexcept
      : type (t)
  {
    switch (type)
    {
    case json_type::null:                                             break;
    case json_type::boolean:            boolean = false;              break;
    case json_type::signed_number:      signed_number = 0;            break;
    case json_type::unsigned_number:
    case json_type::hexadecimal_number: unsigned_number = 0;          break;
    case json_type::string:             new (&string) string_type (); break;
    case json_type::array:              new (&array)  array_type ();  break;
    case json_type::object:             new (&object) object_type (); break;
    }
  }

  inline json_value::
  json_value (std::nullptr_t) noexcept
      : type (json_type::null)
  {
  }

  inline json_value::
  json_value (bool v) noexcept
      : type (json_type::boolean), boolean (v)
  {
  }

  inline json_value::
  json_value (int64_t v) noexcept
      : type (json_type::signed_number), signed_number (v)
  {
  }

  inline json_value::
  json_value (uint64_t v, bool hex) noexcept
      : type (hex
              ? json_type::hexadecimal_number
              : json_type::unsigned_number),
        unsigned_number (v)
  {
  }

  inline json_value::
  json_value (string_type v)
      : type (json_type::string), string (move (v))
  {
  }

  inline const json_value& json_value::
  at (const string_type& n) const
  {
    return at (n.c_str ());
  }

  inline json_value& json_value::
  at (const string_type& n)
  {
    return at (n.c_str ());
  }

  inline const json_value* json_value::
  find (const string_type& n) const
  {
    return find (n.c_str ());
  }

  inline json_value* json_value::
  find (const string_type& n)
  {
    return find (n.c_str ());
  }

#if 0
  inline const json_value& json_value::
  operator[] (const string_type& n) const
  {
    return operator[] (n.c_str ());
  }

  inline json_value& json_value::
  operator[] (const string_type& n)
  {
    return operator[] (n.c_str ());
  }
#endif

  inline json_value::
  json_value (json_value&& v) noexcept
      : type (v.type)
  {
    switch (type)
    {
    case json_type::null:
      break;
    case json_type::boolean:
      boolean = v.boolean;
      break;
    case json_type::signed_number:
      signed_number = v.signed_number;
      break;
    case json_type::unsigned_number:
    case json_type::hexadecimal_number:
      unsigned_number = v.unsigned_number;
      break;
    case json_type::string:
      new (&string) string_type (move (v.string));
      v.string.~string_type ();
      break;
    case json_type::array:
      new (&array) array_type (move (v.array));
      v.array.~array_type ();
      break;
    case json_type::object:
      new (&object) object_type (move (v.object));
      v.object.~object_type ();
      break;
    }

    v.type = json_type::null;
  }

  inline json_value::
  json_value (const json_value& v)
      : type (v.type)
  {
    switch (type)
    {
    case json_type::null:
      break;
    case json_type::boolean:
      boolean = v.boolean;
      break;
    case json_type::signed_number:
      signed_number = v.signed_number;
      break;
    case json_type::unsigned_number:
    case json_type::hexadecimal_number:
      unsigned_number = v.unsigned_number;
      break;
    case json_type::string:
      new (&string) string_type (v.string);
      break;
    case json_type::array:
      new (&array) array_type (v.array);
      break;
    case json_type::object:
      new (&object) object_type (v.object);
      break;
    }
  }

  inline json_value& json_value::
  operator= (json_value&& v) noexcept
  {
    if (this != &v)
    {
      this->~json_value ();
      new (this) json_value (move (v));
    }
    return *this;
  }

  inline json_value& json_value::
  operator= (const json_value& v)
  {
    if (this != &v)
    {
      this->~json_value ();
      new (this) json_value (v);
    }
    return *this;
  }

  // json_array
  //
  inline json_array::
  json_array () noexcept
      : json_value (json_type::array)
  {
  }

  inline json_array::
  json_array (json_parser& p)
      : json_value (p, json_type::array)
  {
  }

  inline void json_array::
  serialize (json_buffer_serializer& s) const
  {
    json_value::serialize (s, json_type::array);
  }

  // json_object
  //
  inline json_object::
  json_object () noexcept
      : json_value (json_type::object)
  {
  }

  inline json_object::
  json_object (json_parser& p)
      : json_value (p, json_type::object)
  {
  }

  inline void json_object::
  serialize (json_buffer_serializer& s) const
  {
    json_value::serialize (s, json_type::object);
  }
}
