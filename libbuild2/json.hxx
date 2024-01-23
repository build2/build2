// file      : libbuild2/json.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_JSON_HXX
#define LIBBUILD2_JSON_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace butl
{
  namespace json
  {
    class parser;
    class buffer_serializer;
  }
}

namespace build2
{
  // This JSON representation has two extensions compared to the standard JSON
  // model: it distinguishes between signed and unsigned numbers and
  // represents an object member as a JSON value rather than, say, a pair of a
  // string and value. The latter allows us to use the JSON value itself as an
  // element of a container.
  //
  // Note also that we don't assume that object members are in a sorted order
  // (but do assume there are no duplicates). However, we could add an
  // argument to signal that this is the case to speed up some functions, for
  // example, compare().
  //
  enum class json_type: uint8_t
  {
    null, // Note: keep first for comparison.
    boolean,
    signed_number,
    unsigned_number,
    string,
    array,
    object,
  };

  class LIBBUILD2_SYMEXPORT json_value
  {
  public:
    using string_type = build2::string;
    using container_type = vector<json_value>;

    json_type type;

    optional<string_type> name; // If present, then this is a member with value.

    union
    {
      bool           boolean;
      int64_t        signed_number;
      uint64_t       unsigned_number;
      string_type    string;
      container_type container; // arrary and object
    };

    // Throws invalid_json_input.
    //
    explicit
    json_value (butl::json::parser&);

    explicit
    json_value (json_type t = json_type::null)
        : type (t)
    {
      switch (type)
      {
      case json_type::null:                                          break;
      case json_type::boolean:         boolean = false;              break;
      case json_type::signed_number:   signed_number = 0;            break;
      case json_type::unsigned_number: unsigned_number = 0;          break;
      case json_type::string:          new (&string) string_type (); break;
      case json_type::array:
      case json_type::object:          new (&container) container_type (); break;
      }
    }

    json_value (string_type member_name, json_type t)
        : json_value (t) {name = move (member_name);}

    explicit
    json_value (std::nullptr_t)
        : type (json_type::null) {}

    json_value (string_type member_name, std::nullptr_t v)
        : json_value (v) {name = move (member_name);}

    explicit
    json_value (bool v)
        : type (json_type::boolean), boolean (v) {}

    json_value (string_type member_name, bool v)
        : json_value (v) {name = move (member_name);}

    explicit
    json_value (int64_t v)
        : type (json_type::signed_number), signed_number (v) {}

    json_value (string_type member_name, int64_t v)
        : json_value (v) {name = move (member_name);}

    explicit
    json_value (uint64_t v)
        : type (json_type::unsigned_number), unsigned_number (v) {}

    json_value (string_type member_name, uint64_t v)
        : json_value (v) {name = move (member_name);}

    explicit
    json_value (string_type v)
        : type (json_type::string), string (move (v)) {}

    json_value (string_type member_name, string_type v)
        : json_value (move (v)) {name = move (member_name);}

    explicit
    json_value (container_type v, json_type t)
        : type (t), container (move (v))
    {
#ifndef NDEBUG
      assert (t == json_type::array || t == json_type::object);

      for (const json_value& e: container)
        assert (e.name.has_value () == (t == json_type::object));
#endif
    }

    json_value (string_type member_name, container_type v, json_type t)
        : json_value (move (v), t) {name = move (member_name);}

    // Note that the moved-from value becomes null.
    //
    json_value (json_value&& v) noexcept
      : type (v.type), name (move (v.name))
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
        unsigned_number = v.unsigned_number;
        break;
      case json_type::string:
        new (&string) string_type (move (v.string));
        v.string.~string_type ();
        break;
      case json_type::array:
      case json_type::object:
        new (&container) container_type (move (v.container));
        v.container.~container_type ();
        break;
      }

      v.type = json_type::null;
      v.name = nullopt;
    }

    json_value& operator= (json_value&& v) noexcept
    {
      if (this != &v)
      {
        this->~json_value ();
        new (this) json_value (move (v));
      }
      return *this;
    }

    json_value (const json_value& v)
        : type (v.type), name (v.name)
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
        unsigned_number = v.unsigned_number;
        break;
      case json_type::string:
        new (&string) string_type (v.string);
        break;
      case json_type::array:
      case json_type::object:
        new (&container) container_type (v.container);
        break;
      }
    }

    json_value& operator= (const json_value& v)
    {
      if (this != &v)
      {
        this->~json_value ();
        new (this) json_value (v);
      }
      return *this;
    }

    ~json_value () noexcept
    {
      switch (type)
      {
      case json_type::null:
      case json_type::boolean:
      case json_type::signed_number:
      case json_type::unsigned_number:                               break;
      case json_type::string:          string.~string_type ();       break;
      case json_type::array:
      case json_type::object:          container.~container_type (); break;
      }
    }

    // Note that values of different types are never equal, except for
    // signed/unsigned numbers. Null is equal to null and is less than any
    // other value. Arrays are compared lexicographically. Object members are
    // considered in the lexicographically-compared name-ascending order (see
    // RFC8785). An absent member is less than a present member (even if it's
    // null).
    //
    // Note that while it doesn't make much sense to compare members to
    // non-members, we allow it with a non-member always being less than a
    // member (even if null), unless ignore_name is true, in which case member
    // names are ignored.
    //
    int
    compare (const json_value&, bool ignore_name = false) const;
  };

  // Throws invalid_json_output.
  //
  LIBBUILD2_SYMEXPORT void
  serialize (butl::json::buffer_serializer&, const json_value&);

  inline bool
  operator== (const json_value& x, const json_value& y) {return x.compare (y) == 0;}

  inline bool
  operator!= (const json_value& x, const json_value& y) {return !(x == y);}

  inline bool
  operator< (const json_value& x, const json_value& y) {return x.compare (y) < 0;}

  inline bool
  operator<= (const json_value& x, const json_value& y) {return x.compare (y) <= 0;}

  inline bool
  operator> (const json_value& x, const json_value& y) {return !(x <= y);}

  inline bool
  operator>= (const json_value& x, const json_value& y) {return !(x < y);}
}

#endif // LIBBUILD2_JSON_HXX
