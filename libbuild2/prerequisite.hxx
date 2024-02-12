// file      : libbuild2/prerequisite.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_PREREQUISITE_HXX
#define LIBBUILD2_PREREQUISITE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/action.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/target-key.hxx>
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/prerequisite-key.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Note that every data member except for the target is immutable (const).
  //
  class LIBBUILD2_SYMEXPORT prerequisite
  {
  public:
    using scope_type = build2::scope;
    using target_type = build2::target;
    using target_type_type = build2::target_type;

    // Note that unlike targets, for prerequisites an empty out directory
    // means undetermined rather than being definitely in the out tree (but
    // maybe we should make this explicit via optional<>; see the from-target
    // constructor).
    //
    // It might seem natural to keep the reference to the owner target instead
    // of to the scope. But that's not the semantics that we have, consider:
    //
    // foo/obj{x}: bar/cxx{y}
    //
    // bar/ here is relative to the scope, not to foo/. Plus, bar/ can resolve
    // to either src or out.
    //
    const optional<project_name> proj;
    const target_type_type& type;
    const dir_path dir;         // Normalized absolute or relative (to scope).
    const dir_path out;         // Empty, normalized absolute, or relative.
    const string name;
    const optional<string> ext; // Absent if unspecified.
    const scope_type& scope;

    // NULL if not yet resolved. Note that this should always be the "primary
    // target", not a member of a target group.
    //
    // While normally only a matching rule should change this, if the
    // prerequisite comes from the group, then it's possible that several
    // rules will try to update it simultaneously. Thus the atomic.
    //
    mutable atomic<const target_type*> target {nullptr};

    // Prerequisite-specific variables.
    //
    // Note that the lookup is often ad hoc (see bin.whole as an example).
    // But see also parser::lookup_variable() if adding something here.
    //
    // @@ PERF: redo as vector so can make move constructor noexcept.
    //
  public:
    variable_map vars;

    // Return a value suitable for assignment. See target for details.
    //
    value&
    assign (const variable& var) {return vars.assign (var);}

    // Return a value suitable for appending. See target for details. Note
    // that we have to explicitly pass the target that this prerequisite
    // belongs to.
    //
    value&
    append (const variable&, const target_type&);

  public:
    prerequisite (optional<project_name> p,
                  const target_type_type& t,
                  dir_path d,
                  dir_path o,
                  string n,
                  optional<string> e,
                  const scope_type& s)
        : proj (move (p)),
          type (t),
          dir (move (d)),
          out (move (o)),
          name (move (n)),
          ext (move (e)),
          scope (s),
          vars (*this, false /* shared */) {}

    prerequisite (const target_type_type& t,
                  dir_path d,
                  dir_path o,
                  string n,
                  optional<string> e,
                  const scope_type& s)
        : type (t),
          dir (move (d)),
          out (move (o)),
          name (move (n)),
          ext (move (e)),
          scope (s),
          vars (*this, false /* shared */) {}

    // Make a prerequisite from a target. If the second argument is true,
    // assume the targets mutex is locked (see ext_locked()/key_locked()
    // for background).
    //
    explicit
    prerequisite (const target_type&, bool locked = false);

    // Note that the returned key "tracks" the prerequisite; that is, any
    // updates to the prerequisite's members will be reflected in the key.
    //
    prerequisite_key
    key () const
    {
      return prerequisite_key {proj, {&type, &dir, &out, &name, ext}, &scope};
    }

    // As above but remap the target type to the specified.
    //
    prerequisite_key
    key (const target_type_type& tt) const
    {
      return prerequisite_key {proj, {&tt, &dir, &out, &name, ext}, &scope};
    }

    // Return true if this prerequisite instance (physically) belongs to the
    // target's prerequisite list. Note that this test only works if you use
    // references to the container elements and the container hasn't been
    // resized since such a reference was obtained. Normally this function is
    // used when iterating over a combined prerequisites range to detect if
    // the prerequisite came from the group (see group_prerequisites).
    //
    bool
    belongs (const target_type&) const;

    // Prerequisite (target) type.
    //
  public:
    template <typename T>
    bool
    is_a () const {return type.is_a<T> ();}

    bool
    is_a (const target_type_type& tt) const {return type.is_a (tt);}

  public:
    // Note that we have the noexcept specification even though vars
    // (std::map) could potentially throw.
    //
    prerequisite (prerequisite&& x) noexcept
        : proj (move (x.proj)),
          type (x.type),
          dir (move (x.dir)),
          out (move (x.out)),
          name (move (x.name)),
          ext (move (x.ext)),
          scope (x.scope),
          target (x.target.load (memory_order_relaxed)),
          vars (move (x.vars), *this, false /* shared */)
          {}

    prerequisite (const prerequisite& x, memory_order o = memory_order_consume)
        : proj (x.proj),
          type (x.type),
          dir (x.dir),
          out (x.out),
          name (x.name),
          ext (x.ext),
          scope (x.scope),
          target (x.target.load (o)),
          vars (x.vars, *this, false /* shared */) {}
  };

  inline ostream&
  operator<< (ostream& os, const prerequisite& p)
  {
    return os << p.key ();
  }

  using prerequisites = vector<prerequisite>;
}

#endif // LIBBUILD2_PREREQUISITE_HXX
