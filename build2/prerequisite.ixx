// file      : build2/prerequisite.ixx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file
namespace build2
{
  include_type
  include_impl (action,
                const target&,
                const string&,
                const prerequisite&,
                const target*);

  extern const variable* var_include; // context.cxx

  inline include_type
  include (action a, const target& t, const prerequisite& p, const target* m)
  {
    // Most of the time this variable will not be specified, so let's optimize
    // for that.
    //
    if (p.vars.empty ())
      return true;

    const string* v (cast_null<string> (p.vars[var_include]));

    if (v == nullptr)
      return true;

    return include_impl (a, t, *v, p, m);
  }
}
