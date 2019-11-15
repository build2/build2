// file      : libbuild2/types.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  // Note that in the constructors we cannot pass the file data member to the
  // base class constructor as it is not initialized yet (and so its base
  // path/name pointers are not initialized). Thus, we initialize the path
  // name view in the constructor body.
  //
  inline location_value::
  location_value ()
  {
    location::file = file;
  }

  inline location_value::
  location_value (const location& l)
      : location (l.line, l.column), file (l.file)
  {
    location::file = file;
  }

  inline location_value::
  location_value (location_value&& l)
      : location (l.line, l.column),
        file (std::move (l.file))
  {
    location::file = file;
  }

  inline location_value::
  location_value (const location_value& l)
      : location (l.line, l.column), file (l.file)
  {
    location::file = file;
  }

  inline location_value& location_value::
  operator= (location_value&& l)
  {
    if (this != &l)
    {
      file = std::move (l.file);
      line = l.line;
      column = l.column;
    }

    return *this;
  }

  inline location_value& location_value::
  operator= (const location_value& l)
  {
    if (this != &l)
    {
      file = l.file;
      line = l.line;
      column = l.column;
    }

    return *this;
  }
}
