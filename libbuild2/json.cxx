// file      : libbuild2/json.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/json.hxx>

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/parser.hxx>
#  include <libbutl/json/serializer.hxx>
#endif

using namespace butl::json;

namespace build2
{
#ifndef BUILD2_BOOTSTRAP
  json_value::
  json_value (parser& p)
  {
    // A JSON input text cannot be empty.
    //
    event e (*p.next ());

    switch (e)
    {
    case event::begin_object:
      {
        container_type c; // For exception safety.
        while (*p.next () != event::end_object)
        {
          string_type n (p.name ());
          json_value v (p);
          v.name = move (n);
          c.push_back (move (v));
        }

        new (&container) container_type (move (c));
        type = json_type::object;
        break;
      }
    case event::begin_array:
      {
        container_type c; // For exception safety.
        while (*p.peek () != event::end_array)
          c.push_back (json_value (p));
        p.next (); // Consume end_array.

        new (&container) container_type (move (c));
        type = json_type::array;
        break;
      }
    case event::string:
      {
        string_type& s (p.value ());

        // Don't move if small string optimized.
        //
        if (s.size () > 15)
          new (&string) string_type (move (s));
        else
          new (&string) string_type (s);

        type = json_type::string;
        break;
      }
    case event::number:
      {
        string_type& s (p.value ());

        if (s[0] == '-')
        {
          signed_number = p.value<int64_t> ();
          type = json_type::signed_number;
        }
        else
        {
          unsigned_number = p.value<uint64_t> ();
          type = json_type::unsigned_number;
        }

        break;
      }
    case event::boolean:
      {
        boolean = p.value<bool> ();
        type = json_type::boolean;
        break;
      }
    case event::null:
      {
        type = json_type::null;
        break;
      }
    case event::name:
    case event::end_array:
    case event::end_object:
      {
        assert (false);
        type = json_type::null;
        break;
      }
    }
  }

  void
  serialize (buffer_serializer& s, const json_value& v)
  {
    if (v.name)
      s.member_name (*v.name);

    switch (v.type)
    {
    case json_type::null:
      {
        s.value (nullptr);
        break;
      }
    case json_type::boolean:
      {
        s.value (v.boolean);
        break;
      }
    case json_type::signed_number:
      {
        s.value (v.signed_number);
        break;
      }
    case json_type::unsigned_number:
      {
        s.value (v.unsigned_number);
        break;
      }
    case json_type::string:
      {
        s.value (v.string);
        break;
      }
    case json_type::array:
      {
        s.begin_array ();
        for (const json_value& e: v.container)
          serialize (s, e);
        s.end_array ();
        break;
      }
    case json_type::object:
      {
        s.begin_object ();
        for (const json_value& m: v.container)
          serialize (s, m);
        s.end_object ();
        break;
      }
    }
  }
#else
  json_value::
  json_value (parser&)
  {
    assert (false);
    type = json_type::null;
  }

  void
  serialize (buffer_serializer&, const json_value&)
  {
    assert (false);
  }
#endif
}
