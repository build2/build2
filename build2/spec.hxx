// file      : build2/spec.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_SPEC_HXX
#define BUILD2_SPEC_HXX

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/variable.hxx>

namespace build2
{
  class scope;

  struct targetspec
  {
    typedef build2::name name_type;

    explicit
    targetspec (name_type n): name (move (n)) {}
    targetspec (dir_path sb, name_type n)
        : src_base (move (sb)), name (move (n)) {}

    dir_path src_base;
    name_type name;

    // The rest is calculated and cached.
    //
    scope* root_scope = nullptr;
    dir_path out_base;
    path buildfile; // Empty if implied.
    bool forwarded = false;
  };

  struct opspec: vector<targetspec>
  {
    opspec () = default;
    opspec (string n): name (move (n)) {}

    string name;
    values params;
  };

  struct metaopspec: vector<opspec>
  {
    metaopspec () = default;
    metaopspec (string n): name (move (n)) {}

    string name;
    values params;
  };

  typedef vector<metaopspec> buildspec;

  ostream&
  operator<< (ostream&, const targetspec&);

  ostream&
  operator<< (ostream&, const opspec&);

  ostream&
  operator<< (ostream&, const metaopspec&);

  ostream&
  operator<< (ostream&, const buildspec&);
}

#endif // BUILD2_SPEC_HXX
