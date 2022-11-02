// file      : libbuild2/utility.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  template <typename I, typename F>
  void
  append_option_values (cstrings& args, const char* o, I b, I e, F&& get)
  {
    if (b != e)
    {
      args.reserve (args.size () + (e - b));

      for (; b != e; ++b)
      {
        args.push_back (o);
        args.push_back (get (*b));
      }
    }
  }

  template <typename I, typename F>
  void
  append_option_values (sha256& cs, const char* o, I b, I e, F&& get)
  {
    for (; b != e; ++b)
    {
      cs.append (o);
      cs.append (get (*b));
    }
  }

  template <typename K>
  basic_path<char, K>
  relative (const basic_path<char, K>& p)
  {
    using path = basic_path<char, K>;

    const dir_path& b (*relative_base);

    if (p.simple () || b.empty ())
      return p;

    if (p.sub (b))
      return p.leaf (b);

    if (p.root_directory () == b.root_directory ())
    {
      path r (p.relative (b));

      if (r.string ().size () < p.string ().size ())
        return r;
    }

    return p;
  }
}
