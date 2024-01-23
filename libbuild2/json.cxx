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
  int json_value::
  compare (const json_value& v, bool ignore_name) const
  {
    int r (0);

    if (!ignore_name)
      r = name < v.name ? -1 : (name > v.name ? 1 : 0);

    if (r == 0)
    {
      if (type != v.type)
      {
        // Handle the special signed/unsigned number case here.
        //
        if (type == json_type::signed_number &&
            v.type == json_type::unsigned_number)
        {
          if (signed_number < 0)
            r = -1;
          else
          {
            uint64_t u (static_cast<uint64_t> (signed_number));
            r = u < v.unsigned_number ? -1 : (u > v.unsigned_number ? 1 : 0);
          }
        }
        else if (type == json_type::unsigned_number &&
                 v.type == json_type::signed_number)
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
          r = (static_cast<uint8_t> (type) < static_cast<uint8_t> (v.type)
               ? -1
               : 1);
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
          auto i (container.begin ()), ie (container.end ());
          auto j (v.container.begin ()), je (v.container.end ());

          for (; i != ie && j != je; ++i, ++j)
          {
            if ((r = i->compare (*j)) != 0)
              break;
          }

          if (r == 0)
            r = i == ie ? -1 : (j == je ? 1 : 0); // More elements than other?

          break;
        }
      case json_type::object:
        {
          // We don't expect there to be a large number of members so it makes
          // sense to iterate in the lexicographical order without making any
          // copies.
          //
          auto next = [] (container_type::const_iterator p, // == e for first
                          container_type::const_iterator b,
                          container_type::const_iterator e)
          {
            // We need to find an element with the "smallest" name that is
            // greater than the previous entry.
            //
            auto n (e);

            for (auto i (b); i != e; ++i)
            {
              if (p == e || *i->name > *p->name)
              {
                int r;
                if (n == e || (r = n->name->compare (*i->name)) > 0)
                  n = i;
                else
                  assert (r != 0); // No duplicates.
              }
            }

            return n;
          };

          auto ib (container.begin ()), ie (container.end ()), i (ie);
          auto jb (v.container.begin ()), je (v.container.end ()), j (je);

          while ((i = next (i, ib, ie)) != ie &&
                 (j = next (j, jb, je)) != je)
          {
            // Determine if both have this name and if not, which name comes
            // first.
            //
            int n (i->name->compare (*j->name));

            r = (n < 0                  // If i's first, then i is greater.
                 ? -1
                 : (n > 0               // If j's first, then j is greater.
                    ? 1
                    : i->compare (*j, true))); // Both have name, compare value.

            if (r != 0)
              break;
          }

          if (r == 0)
            r = i == ie ? -1 : (j == je ? 1 : 0); // More members than other?

          break;
        }
      }
    }

    return r;
  }

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
          // @@ Override duplicates or fail?

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
