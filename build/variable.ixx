// file      : build/variable.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

namespace build
{
  inline const value_proxy& value_proxy::
  operator= (value_ptr v) const
  {
    assert (v == nullptr || &v->scope == s);
    *p = std::move (v);
    return *this;
  }

  inline const value_proxy& value_proxy::
  operator= (const value_proxy& v) const
  {
    if (this != &v)
    {
      if (v)
        *p = v.as<const value&> ().clone (*s);
      else
        p->reset ();
    }

    return *this;
  }

  inline const value_proxy& value_proxy::
  operator= (std::string v) const
  {
    // In most cases this is used to initialize a new variable, so
    // don't bother trying to optimize for the case where p is not
    // NULL.
    //
    p->reset (new list_value (*s, std::move (v)));
    return *this;
  }

  inline const value_proxy& value_proxy::
  operator= (path v) const
  {
    p->reset (new list_value (*s, std::move (v)));
    return *this;
  }
}
