// file      : libbuild2/buildspec.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_BUILDSPEC_HXX
#define LIBBUILD2_BUILDSPEC_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/variable.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  struct targetspec
  {
    using name_type = build2::name;

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

  struct opspec: small_vector<targetspec, 1>
  {
    opspec () = default;
    opspec (string n): name (move (n)) {}

    string name;
    values params;
  };

  struct metaopspec: small_vector<opspec, 1>
  {
    metaopspec () = default;
    metaopspec (string n): name (move (n)) {}

    string name;
    values params;
  };

  using buildspec = small_vector<metaopspec, 1>;

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const targetspec&);

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const opspec&);

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const metaopspec&);

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const buildspec&);
}

#endif // LIBBUILD2_BUILDSPEC_HXX
