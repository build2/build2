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
    enum class event: uint8_t;
    class parser;
    class buffer_serializer;
    class stream_serializer;
    class invalid_json_input;
    class invalid_json_output;
  }
}

namespace build2
{
  using json_event = butl::json::event;
  using json_parser = butl::json::parser;
  using json_buffer_serializer = butl::json::buffer_serializer;
  using json_stream_serializer = butl::json::stream_serializer;
  using butl::json::invalid_json_input;
  using butl::json::invalid_json_output;

#ifndef BUILD2_BOOTSTRAP
  LIBBUILD2_SYMEXPORT const char*
  to_string (json_event);
#endif

  // @@ TODO:
  //
  // - provide swap().
  // - provide operator=(uint64_t), etc.
  // - provide std::hash specialization
  // - tighted at()/[] interface in json_array and json_object
  // - tighten noexcep where possible
  // - operator bool() - in a sense null is like nullopt.
  //

  // This JSON representation has one extensions compared to the standard JSON
  // model: it distinguishes between signed, unsigned, and hexadecimal
  // numbers.
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
    hexadecimal_number,
    string,
    array,
    object,
  };

  // Return the JSON type as string. If distinguish_numbers is true, then
  // distinguish between the singned, unsigned, and hexadecimal types.
  //
  LIBBUILD2_SYMEXPORT const char*
  to_string (json_type, bool distinguish_numbers = false) noexcept;

  inline ostream&
  operator<< (ostream& os, json_type t) {return os << to_string (t);}

  struct json_member;

  class LIBBUILD2_SYMEXPORT json_value
  {
  public:
    using string_type = build2::string;
    using array_type = vector<json_value>;
    using object_type = vector<json_member>;

    json_type type;

    // Unchecked value access.
    //
    union
    {
      bool           boolean;
      int64_t        signed_number;
      uint64_t       unsigned_number; // Also used for hexadecimal_number.
      string_type    string;
      array_type     array;
      object_type    object;
    };

    // Checked value access.
    //
    // If the type matches, return the corresponding member of the union.
    // Otherwise throw std::invalid_argument.
    //
    bool  as_bool () const;
    bool& as_bool ();

    int64_t  as_int64 () const;
    int64_t& as_int64 ();

    uint64_t  as_uint64 () const;
    uint64_t& as_uint64 ();

    const string_type& as_string () const;
    string_type&       as_string ();

    const array_type& as_array () const;
    array_type&       as_array ();

    const object_type& as_object () const;
    object_type&       as_object ();


    // Construction.
    //
    explicit
    json_value (json_type = json_type::null) noexcept;

    explicit
    json_value (std::nullptr_t) noexcept;

    explicit
    json_value (bool) noexcept;

    explicit
    json_value (int64_t) noexcept;

    explicit
    json_value (uint64_t, bool hexadecimal = false) noexcept;

    explicit
    json_value (string_type);

    // If the expected type is specfied, then fail if it does not match
    // parsed. Throws invalid_json_input.
    //
    explicit
    json_value (json_parser&, optional<json_type> expected = {});

    // If the expected type is specfied, then fail if it does not match the
    // value's. Throws invalid_json_output.
    //
    void
    serialize (json_buffer_serializer&,
               optional<json_type> expected = {}) const;

    // Note that values of different types are never equal, except for
    // signed/unsigned/hexadecimal numbers. Null is equal to null and is less
    // than any other value. Arrays are compared lexicographically. Object
    // members are considered in the lexicographically-compared name-ascending
    // order (see RFC8785). An absent member is less than a present member
    // (even if it's null).
    //
    int
    compare (const json_value&) const noexcept;

    // Append/prepend one JSON value to another. Throw invalid_argument if the
    // values are incompatible. Note that for numbers this can also lead to
    // the change of the value type.
    //
    // Append/prepend an array to an array splices in the array elements
    // rather than adding an element of the array type.
    //
    // By default, append to an object overrides existing members while
    // prepend does not. In a sense, whatever appears last is kept, which is
    // consistent with what we expect to happen when specifying the same name
    // repeatedly (provided it's not considered invalid) in a text
    // representation (e.g., {"a":1,"a":2}). Position-wise, both append and
    // prepend retain the positions of existing members with append inserting
    // new ones at the end while prepend -- at the beginning.
    //
    void
    append (json_value&&, bool override = true);

    void
    prepend (json_value&&, bool override = false);

    // Array element access.
    //
    // If the index is out of array bounds, the at() functions throw
    // std::out_of_range, the const operator[] returns null_json_value, and
    // the non-const operator[] inserts a new null value at the specified
    // position (filling any missing elements in between with nulls) and
    // returns that. All three functions throw std::invalid_argument if the
    // value is not an array or null with null treated as (missing) array
    // rather than wrong value type (and with at() functions throwing
    // out_of_range in this case).
    //
    // Note that non-const operator[] will not only insert a new element but
    // will also turn the value it is called upon into array if it is null.
    // This semantics allows you to string several subscripts to build up a
    // chain of values.
    //
    // Note also that while the operator[] interface is convenient for
    // accessing and modifying (or building up) values deep in the tree, it
    // can lead to inefficiencies or even undesirable semantics during
    // otherwise read-only access of a non-const object due to the potential
    // insertion of null values for missing array elements. As a result, it's
    // recommended to always use a const reference for read-only access (or
    // use the at() interface if this is deemed too easy to forget).
    //
    const json_value&
    at (size_t) const;

    json_value&
    at (size_t);

#if 0
    const json_value&
    operator[] (size_t) const;

    json_value&
    operator[] (size_t);
#endif


    // Object member access.
    //
    // If a member with the specified name is not found in the object, the
    // at() functions throw std::out_of_range, the find() function returns
    // NULL, the const operator[] returns null_json_value, and the non-const
    // operator[] adds a new member with the specified name and null value and
    // returns that value. All three functions throw std::invalid_argument if
    // the value is not an object or null with null treated as (missing)
    // object rather than wrong value type (and with at() functions throwing
    // out_of_range in this case).
    //
    // Note that non-const operator[] will not only insert a new member but
    // will also turn the value it is called upon into object if it is null.
    // This semantics allows you to string several subscripts to build up a
    // chain of values.
    //
    // Note also that while the operator[] interface is convenient for
    // accessing and modifying (or building up) values deep in the tree, it
    // can lead to inefficiencies or even undesirable semantics during
    // otherwise read-only access of a non-const object due to the potential
    // insertion of null values for missing object members. As a result, it's
    // recommended to always use a const reference for read-only access (or
    // use the at() interface if this is deemed too easy to forget).
    //
    const json_value&
    at (const char*) const;

    json_value&
    at (const char*);

    const json_value*
    find (const char*) const;

    json_value*
    find (const char*);

#if 0
    const json_value&
    operator[] (const char*) const;

    json_value&
    operator[] (const char*);
#endif

    const json_value&
    at (const string_type&) const;

    json_value&
    at (const string_type&);

    const json_value*
    find (const string_type&) const;

    json_value*
    find (const string_type&);

#if 0
    const json_value&
    operator[] (const string_type&) const;

    json_value&
    operator[] (const string_type&);
#endif

    // Note that the moved-from value becomes JSON null value.
    //
    json_value (json_value&&) noexcept;
    json_value (const json_value&);

    json_value& operator= (json_value&&) noexcept;
    json_value& operator= (const json_value&);

    ~json_value () noexcept;
  };

  LIBBUILD2_SYMEXPORT extern const json_value null_json_value;

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

  // A JSON object member.
  //
  struct json_member
  {
    // @@ TODO: add some convenience constructors?

    string     name;
    json_value value;
  };

  // A JSON value that can only be an array.
  //
  class /*LIBBUILD2_SYMEXPORT*/ json_array: public json_value
  {
  public:
    // Create empty array.
    //
    json_array () noexcept;

    explicit
    json_array (json_parser&);

    void
    serialize (json_buffer_serializer& s) const;
  };

  // A JSON value that can only be an object.
  //
  class /*LIBBUILD2_SYMEXPORT*/ json_object: public json_value
  {
  public:
    // Create empty object.
    //
    json_object () noexcept;

    explicit
    json_object (json_parser&);

    void
    serialize (json_buffer_serializer& s) const;
  };
}

#include <libbuild2/json.ixx>

#endif // LIBBUILD2_JSON_HXX
