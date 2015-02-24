// file      : build/path.txx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <vector>

namespace build
{
  template <typename C>
  basic_path<C> basic_path<C>::
  leaf () const
  {
    size_type p (traits::rfind_separator (path_));

    return p != string_type::npos
      ? basic_path (path_.c_str () + p + 1, path_.size () - p - 1)
      : *this;
  }

  template <typename C>
  basic_path<C> basic_path<C>::
  directory () const
  {
    if (root ())
      return basic_path ();

    size_type p (traits::rfind_separator (path_));

    // Include the trailing slash so that we get correct behavior
    // if directory is root.
    //
    return p != string_type::npos
      ? basic_path (path_.c_str (), p + 1)
      : basic_path ();
  }

#ifdef _WIN32
  template <typename C>
  typename basic_path<C>::string_type basic_path<C>::
  posix_string () const
  {
    if (absolute ())
      throw invalid_basic_path<C> (path_);

    string_type r (path_);

    // Translate Windows-style separators to the POSIX ones.
    //
    for (size_type i (0), n (r.size ()); i != n; ++i)
      if (r[i] == '\\')
        r[i] = '/';

    return r;
  }
#endif

  template <typename C>
  basic_path<C>& basic_path<C>::
  operator/= (basic_path<C> const& r)
  {
    if (r.absolute () && !path_.empty ()) // Allow ('' / '/foo').
      throw invalid_basic_path<C> (r.path_);

    if (path_.empty () || r.path_.empty ())
    {
      path_ += r.path_;
      return *this;
    }

    if (!traits::is_separator (path_[path_.size () - 1]))
      path_ += traits::directory_separator;

    path_ += r.path_;

    return *this;
  }

  template <typename C>
  basic_path<C> basic_path<C>::
  leaf (basic_path<C> const& d) const
  {
    size_type n (d.path_.size ());

    if (n == 0)
      return *this;

    size_type m (path_.size ());

    if (m < n || path_.compare (0, n, d.path_) != 0)
      throw invalid_basic_path<C> (path_);

    if (n != m
#ifndef _WIN32
        && !d.root ()
#endif
    )
      n++; // Skip the directory separator (unless it is POSIX root).

    return basic_path (path_.c_str () + n, m - n);
  }

  template <typename C>
  basic_path<C> basic_path<C>::
  directory (basic_path<C> const& l) const
  {
    size_type n (l.path_.size ());

    if (n == 0)
      return *this;

    size_type m (path_.size ());

    if (m < n || path_.compare (m - n, n, l.path_) != 0)
      throw invalid_basic_path<C> (path_);

    if (n != m)
      n++; // Skip the directory separator.

    return basic_path (path_.c_str (), m - n);
  }

  template <typename C>
  basic_path<C> basic_path<C>::
  relative (basic_path<C> d) const
  {
    basic_path r;

    for (;; d = d.directory ())
    {
      if (sub (d))
        break;

      r /= path ("..");

      // Roots of the paths do not match.
      //
      if (d.root ())
        throw invalid_basic_path<C> (path_);
    }

    return r / leaf (d);
  }

  template <typename C>
  basic_path<C>& basic_path<C>::
  normalize ()
  {
    if (empty ())
      return *this;

    bool abs (absolute ());

    typedef std::vector<string_type> paths;
    paths ps;

    for (size_type b (0), e (traits::find_separator (path_)),
           n (path_.size ());;
         e = traits::find_separator (path_, b))
    {
      string_type s (path_, b, e == string_type::npos ? e : e - b);
      ps.push_back (s);

      if (e == string_type::npos)
        break;

      ++e;

      while (e < n && traits::is_separator (path_[e]))
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
              throw invalid_basic_path<C> (path_);

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

    path_.swap (p);
    return *this;
  }

  template <typename C>
  void basic_path<C>::
  init ()
  {
    // Strip trailing slashes except for the case where the single
    // slash represents the root directory.
    //
    size_type n (path_.size ());
    for (; n > 1 && traits::is_separator (path_[n - 1]); --n) ;
    path_.resize (n);
  }
}
