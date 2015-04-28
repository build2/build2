// file      : build/path.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#ifdef _WIN32
#  include <cctype>  // std::tolower
#  include <cwctype> // std::towlower
#endif

namespace build
{
#ifdef _WIN32
  template <>
  inline char path_traits<char>::
  tolower (char c)
  {
    return std::tolower (c);
  }

  template <>
  inline wchar_t path_traits<wchar_t>::
  tolower (wchar_t c)
  {
    return std::towlower (c);
  }
#endif

  template <typename C, typename K>
  inline bool basic_path<C, K>::
  absolute () const
  {
#ifdef _WIN32
    return this->path_.size () > 1 && this->path_[1] == ':';
#else
    return !this->path_.empty () && traits::is_separator (this->path_[0]);
#endif
  }

  template <typename C, typename K>
  inline bool basic_path<C, K>::
  root () const
  {
#ifdef _WIN32
    return this->path_.size () == 2 && this->path_[1] == ':';
#else
    return this->path_.size () == 1 && traits::is_separator (this->path_[0]);
#endif
  }

  template <typename C, typename K>
  inline bool basic_path<C, K>::
  sub (const basic_path& p) const
  {
    size_type n (p.path_.size ());

    if (n == 0)
      return true;

    size_type m (this->path_.size ());

    // The second condition guards against the /foo-bar vs /foo case.
    //
    return m >= n && this->path_.compare (0, n, p.path_) == 0 &&
      (traits::is_separator (p.path_.back ()) || // p ends with a separator
       m == n                                 || // *this == p
       traits::is_separator (this->path_[n]));   // next char is a separator
  }

  template <typename C, typename K>
  inline bool basic_path<C, K>::
  sup (const basic_path& p) const
  {
    size_type n (p.path_.size ());

    if (n == 0)
      return true;

    size_type m (this->path_.size ());

    // The second condition guards against the /foo-bar vs bar case.
    //
    return m >= n && this->path_.compare (m - n, n, p.path_) == 0 &&
      (m == n                                   ||     // *this == p
       traits::is_separator (this->path_[m - n - 1])); // prev char separator
  }

  template <typename C, typename K>
  inline auto basic_path<C, K>::
  begin () const -> iterator
  {
    size_type b, e;

    if (this->path_.empty ())
      b = e = string_type::npos;

#ifndef _WIN32
    else if (root ())
    {
      // We want to return a single empty component. Here we return
      // the begin position one past the end. Not sure if this legal.
      //
      b = 1;
      e = string_type::npos;
    }
#endif
    else
    {
      b = 0;
      e = traits::find_separator (this->path_);
    }

    return iterator (this->path_, b, e);
  }

  template <typename C, typename K>
  inline auto basic_path<C, K>::
  end () const -> iterator
  {
    return iterator (this->path_, string_type::npos, string_type::npos);
  }

  template <typename C, typename K>
  inline basic_path<C, K>& basic_path<C, K>::
  complete ()
  {
    if (relative ())
      *this = current () / *this;

    return *this;
  }

  template <typename C, typename K>
  inline typename basic_path<C, K>::dir_type basic_path<C, K>::
  root_directory () const
  {
    return absolute ()
#ifdef _WIN32
      ? dir_type (this->path_, 2)
#else
      ? dir_type ("/")
#endif
      : dir_type ();
  }

  template <typename C, typename K>
  inline basic_path<C, K> basic_path<C, K>::
  base () const
  {
    size_type p (traits::find_extension (this->path_));
    return p != string_type::npos
      ? basic_path (this->path_.c_str (), p)
      : *this;
  }

  template <typename C, typename K>
  inline const C* basic_path<C, K>::
  extension () const
  {
    size_type p (traits::find_extension (this->path_));
    return p != string_type::npos ? this->path_.c_str () + p + 1 : nullptr;
  }

#ifndef _WIN32
  template <typename C, typename K>
  inline typename basic_path<C, K>::string_type basic_path<C, K>::
  posix_string () const
  {
    return string ();
  }
#endif
}
