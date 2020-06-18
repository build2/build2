// file      : libbuild2/lexer.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline auto lexer::
  get () -> xchar
  {
    xchar c (base::get (ebuf_));

    if (invalid (c))
      fail_char (c);

    return c;
  }

  inline void lexer::
  get (const xchar& peeked)
  {
    base::get (peeked);
  }

  inline auto lexer::
  peek () -> xchar
  {
    xchar c (base::peek (ebuf_));

    if (invalid (c))
      fail_char (c);

    return c;
  }
}
