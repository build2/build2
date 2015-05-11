// file      : build/path.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <vector>

namespace build
{
  template <typename C, typename K>
  basic_path<C, K> basic_path<C, K>::
  leaf () const
  {
    size_type p (traits::rfind_separator (this->path_));

    return p != string_type::npos
      ? basic_path (this->path_.c_str () + p + 1, this->path_.size () - p - 1)
      : *this;
  }

  template <typename C, typename K>
  typename basic_path<C, K>::dir_type basic_path<C, K>::
  directory () const
  {
    if (root ())
      return dir_type ();

    size_type p (traits::rfind_separator (this->path_));

    // Include the trailing slash so that we get correct behavior
    // if directory is root.
    //
    return p != string_type::npos
      ? dir_type (this->path_.c_str (), p + 1)
      : dir_type ();
  }

#ifdef _WIN32
  template <typename C, typename K>
  typename basic_path<C, K>::string_type basic_path<C, K>::
  posix_string () const
  {
    if (absolute ())
      throw invalid_basic_path<C> (this->path_);

    string_type r (this->path_);

    // Translate Windows-style separators to the POSIX ones.
    //
    for (size_type i (0), n (r.size ()); i != n; ++i)
      if (r[i] == '\\')
        r[i] = '/';

    return r;
  }
#endif

  template <typename C, typename K>
  basic_path<C, K>& basic_path<C, K>::
  operator/= (basic_path<C, K> const& r)
  {
    if (r.absolute () && !this->path_.empty ()) // Allow ('' / '/foo').
      throw invalid_basic_path<C> (r.path_);

    if (this->path_.empty () || r.path_.empty ())
    {
      this->path_ += r.path_;
      return *this;
    }

    if (!traits::is_separator (this->path_[this->path_.size () - 1]))
      this->path_ += traits::directory_separator;

    this->path_ += r.path_;
    return *this;
  }

  template <typename C, typename K>
  basic_path<C, K> basic_path<C, K>::
  leaf (basic_path<C, K> const& d) const
  {
    size_type n (d.path_.size ());

    if (n == 0)
      return *this;

    if (!sub (d))
      throw invalid_basic_path<C> (this->path_);

    size_type m (this->path_.size ());

    if (n != m
#ifndef _WIN32
        && !d.root ()
#endif
    )
      n++; // Skip the directory separator (unless it is POSIX root).

    return basic_path (this->path_.c_str () + n, m - n);
  }

  template <typename C, typename K>
  typename basic_path<C, K>::dir_type basic_path<C, K>::
  directory (basic_path<C, K> const& l) const
  {
    size_type n (l.path_.size ());

    if (n == 0)
      return dir_type (this->path_);

    if (!sup (l))
      throw invalid_basic_path<C> (this->path_);

    size_type m (this->path_.size ());

    if (n != m)
      n++; // Skip the directory separator.

    return dir_type (this->path_.c_str (), m - n);
  }

  template <typename C, typename K>
  basic_path<C, K> basic_path<C, K>::
  relative (basic_path<C, K> d) const
  {
    basic_path r;

    for (;; d = d.directory ())
    {
      if (sub (d))
        break;

      r /= basic_path ("..");

      // Roots of the paths do not match.
      //
      if (d.root ())
        throw invalid_basic_path<C> (this->path_);
    }

    return r / leaf (d);
  }

  template <typename C, typename K>
  basic_path<C, K>& basic_path<C, K>::
  normalize ()
  {
    if (empty ())
      return *this;

    bool abs (absolute ());

    typedef std::vector<string_type> paths;
    paths ps;

    for (size_type b (0), e (traits::find_separator (this->path_)),
           n (this->path_.size ());;
         e = traits::find_separator (this->path_, b))
    {
      string_type s (this->path_, b, e == string_type::npos ? e : e - b);
      ps.push_back (s);

      if (e == string_type::npos)
        break;

      ++e;

      while (e < n && traits::is_separator (this->path_[e]))
        ++e;

      if (e == n)
        break;

      b = e;
    }

    // First collapse '.' and '..'.
    //
    paths r;

    for (typename paths::const_iterator i (ps.begin ()), e (ps.end ());
         i != e; ++i)
    {
      string_type const& s (*i);
      size_type n (s.size ());

      if (n == 1 && s[0] == '.')
        continue;

      if (n == 2 && s[0] == '.' && s[1] == '.')
      {
        // Pop the last directory from r unless it is '..'.
        //
        if (!r.empty ())
        {
          string_type const& s1 (r.back ());

          if (!(s1.size () == 2 && s1[0] == '.' && s1[1] == '.'))
          {
            // Cannot go past the root directory.
            //
            if (abs && r.size () == 1)
              throw invalid_basic_path<C> (this->path_);

            r.pop_back ();
            continue;
          }
        }
      }

      r.push_back (s);
    }

    // Reassemble the path.
    //
    string_type p;

    for (typename paths::const_iterator i (r.begin ()), e (r.end ());
         i != e;)
    {
      p += *i;

      if (++i != e)
        p += traits::directory_separator;
    }

    if (p.empty () && !r.empty ())
      p += traits::directory_separator; // Root directory.

    this->path_.swap (p);
    return *this;
  }

  template <typename C, typename K>
  void basic_path<C, K>::
  current (basic_path const& p)
  {
    const string_type& s (p.string ());

    if (s.empty ())
      throw invalid_basic_path<char> (s);

    traits::current (s);
  }

  template <typename C, typename K>
  void basic_path<C, K>::
  init ()
  {
    // Strip trailing slashes except for the case where the single
    // slash represents the root directory.
    //
    size_type n (this->path_.size ());
    for (; n > 1 && traits::is_separator (this->path_[n - 1]); --n) ;
    this->path_.resize (n);
  }
}
