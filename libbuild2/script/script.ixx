// file      : libbuild2/script/script.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace script
  {
    inline command_to_stream
    operator&= (command_to_stream& x, command_to_stream y)
    {
      return x = static_cast<command_to_stream> (
        static_cast<uint16_t> (x) & static_cast<uint16_t> (y));
    }

    inline command_to_stream
    operator|= (command_to_stream& x, command_to_stream y)
    {
      return x = static_cast<command_to_stream> (
        static_cast<uint16_t> (x) | static_cast<uint16_t> (y));
    }

    inline command_to_stream
    operator& (command_to_stream x, command_to_stream y) {return x &= y;}

    inline command_to_stream
    operator| (command_to_stream x, command_to_stream y) {return x |= y;}

    // command
    //
    inline ostream&
    operator<< (ostream& o, const command& c)
    {
      to_stream (o, c, command_to_stream::all);
      return o;
    }

    // command_pipe
    //
    inline ostream&
    operator<< (ostream& o, const command_pipe& p)
    {
      to_stream (o, p, command_to_stream::all);
      return o;
    }

    // command_expr
    //
    inline ostream&
    operator<< (ostream& o, const command_expr& e)
    {
      to_stream (o, e, command_to_stream::all);
      return o;
    }

    // deadline
    //
    inline bool
    operator< (const deadline& x, const deadline& y)
    {
      if (x.value != y.value)
        return x.value < y.value;

      return x.success < y.success;
    }

    inline optional<deadline>
    to_deadline (const optional<timestamp>& ts, bool success)
    {
      return ts ? deadline (*ts, success) : optional<deadline> ();
    }

    // timeout
    //
    inline bool
    operator< (const timeout& x, const timeout& y)
    {
      if (x.value != y.value)
        return x.value < y.value;

      return x.success < y.success;
    }

    inline optional<timeout>
    to_timeout (const optional<duration>& d, bool success)
    {
      return d ? timeout (*d, success) : optional<timeout> ();
    }
  }
}
