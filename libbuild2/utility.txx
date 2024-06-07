// file      : libbuild2/utility.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  template <typename I, typename F>
  void
  append_option_values (cstrings& ss, const char* o, I b, I e, F&& get)
  {
    if (b != e)
    {
      ss.reserve (ss.size () + (e - b));

      for (; b != e; ++b)
      {
        ss.push_back (o);
        ss.push_back (get (*b));
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

  template <typename I, typename F>
  void
  append_combined_option_values (strings& ss, const char* o, I b, I e, F&& get)
  {
    if (b != e)
    {
      ss.reserve (ss.size () + (e - b));

      for (; b != e; ++b)
        ss.push_back (string (o) += get (*b));
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
