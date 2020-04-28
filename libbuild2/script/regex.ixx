// file      : libbuild2/script/regex.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace script
  {
    namespace regex
    {
      inline char_flags
      operator&= (char_flags& x, char_flags y)
      {
        return x = static_cast<char_flags> (
          static_cast<uint16_t> (x) & static_cast<uint16_t> (y));
      }

      inline char_flags
      operator|= (char_flags& x, char_flags y)
      {
        return x = static_cast<char_flags> (
          static_cast<uint16_t> (x) | static_cast<uint16_t> (y));
      }

      inline char_flags
      operator& (char_flags x, char_flags y) {return x &= y;}

      inline char_flags
      operator| (char_flags x, char_flags y) {return x |= y;}
    }
  }
}
