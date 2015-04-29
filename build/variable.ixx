// file      : build/variable.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

namespace build
{
  inline const value_proxy& value_proxy::
  operator= (value_ptr v) const
  {
    *p = std::move (v);
    return *this;
  }

  inline const value_proxy& value_proxy::
  operator= (const value_proxy& v) const
  {
    if (this != &v)
    {
      if (v)
        *p = v.as<const value&> ().clone ();
      else
        p->reset ();
    }

    return *this;
  }

  inline const value_proxy& value_proxy::
  operator= (list_value v) const
  {
    if (*p == nullptr)
      p->reset (new list_value (std::move (v)));
    else
      //@@ Assuming it is a list_value.
      //
      as<list_value&> () = std::move (v);

    return *this;
  }

  inline const value_proxy& value_proxy::
  operator= (std::string v) const
  {
    // In most cases this is used to initialize a new variable, so
    // don't bother trying to optimize for the case where p is not
    // NULL.
    //
    p->reset (new list_value (std::move (v)));
    return *this;
  }

  inline const value_proxy& value_proxy::
  operator= (dir_path v) const
  {
    p->reset (new list_value (std::move (v)));
    return *this;
  }

  inline const value_proxy& value_proxy::
  operator= (nullptr_t) const
  {
    p->reset ();
    return *this;
  }

  inline const value_proxy& value_proxy::
  operator+= (const value_proxy& v) const
  {
    if (v && this != &v)
    {
      if (*p == nullptr)
        *this = v;
      else
        //@@ Assuming it is a list_value.
        //
        *this += v.as<const list_value&> ();
    }

    return *this;
  }

  inline const value_proxy& value_proxy::
  operator+= (const list_value& v) const
  {
    if (*p == nullptr)
      *this = v;
    else
    {
      list_value& lv (as<list_value&> ());
      lv.insert (lv.end (), v.begin (), v.end ());
    }

    return *this;
  }

  inline const value_proxy& value_proxy::
  operator+= (std::string v) const
  {
    if (*p == nullptr)
      *this = v;
    else
      as<list_value&> ().emplace_back (std::move (v));

    return *this;
  }
}
