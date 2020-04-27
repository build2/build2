// file      : libbuild2/prerequisite-key.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_PREREQUISITE_KEY_HXX
#define LIBBUILD2_PREREQUISITE_KEY_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target-key.hxx>
#include <libbuild2/target-type.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Light-weight (by being shallow-pointing) prerequisite key, similar
  // to (and based on) target key.
  //
  // Note that unlike prerequisite, the key is not (necessarily) owned by a
  // target. So for the key we instead have the base scope of the target that
  // (would) own it. Note that we assume keys to be ephemeral enough for the
  // base scope to remain unchanged.
  //
  class prerequisite_key
  {
  public:
    using scope_type = build2::scope;

    const optional<project_name>& proj;
    target_key tk;                // The .dir and .out members can be relative.
    const scope_type* scope;      // Can be NULL if tk.dir is absolute.

    template <typename T>
    bool is_a () const {return tk.is_a<T> ();}
    bool is_a (const target_type& tt) const {return tk.is_a (tt);}
  };

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const prerequisite_key&);
}

#endif // LIBBUILD2_PREREQUISITE_KEY_HXX
