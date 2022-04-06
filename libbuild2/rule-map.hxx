// file      : libbuild2/rule-map.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_RULE_MAP_HXX
#define LIBBUILD2_RULE_MAP_HXX

#include <libbutl/prefix-map.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>

namespace build2
{
  // A rule name is used both for diagnostics as well as to match rule hints
  // (see rule_hints). A rule hint is a potentially partial rule name.
  //
  // The recommended rule naming scheme is to start with the module name, for
  // example: cxx.compile, cxx.link. This way a rule hint can be just the
  // module name, for example [rule_hint=cxx]. If a module can only possibly
  // have a single rule, then the rule name can be just the module name (e.g.,
  // `in`; though make doubly sure there is unlikely to be a need for another
  // rule, for example, for documentation generation, in the future).
  //
  // The two common choices of names for the second component in a rule name
  // is an action (e.g., cxx.compile, cxx.link) or a target type (e.g.,
  // bin.def, bin.lib). The latter is a good choice when the action is
  // inherent to the target type (e.g., "generate def file", "see through lib
  // group"). Also note that a rule for compensating operations (e.g.,
  // update/clean, install/uninstall) is customarily registered with the same
  // name.
  //
  struct name_rule_map: butl::prefix_map<string,
                                         reference_wrapper<const rule>,
                                         '.'>
  {
    // Return true if the rule name matches a rule hint.
    //
    static bool
    sub (const string& hint, const string& name)
    {
      return compare_type ('.').prefix (hint, name);
    }
  };

  using target_type_rule_map = map<const target_type*, name_rule_map>;

  // This is an "indexed map" with operation_id being the index. Entry
  // with id 0 is a wildcard.
  //
  // Note that while we may resize some vectors during non-initial load, this
  // is MT-safe since we never cache any references to their elements.
  //
  class operation_rule_map
  {
  public:
    // Return false in case of a duplicate.
    //
    bool
    insert (operation_id oid,
            const target_type& tt,
            string name,
            const rule& r)
    {
      // 3 is the number of builtin operations.
      //
      if (oid >= map_.size ())
        map_.resize ((oid < 3 ? 3 : oid) + 1);

      return map_[oid][&tt].emplace (move (name), r).second;
    }

    // Return NULL if not found.
    //
    const target_type_rule_map*
    operator[] (operation_id oid) const
    {
      return map_.size () > oid ? &map_[oid] : nullptr;
    }

    bool
    empty () const {return map_.empty ();}

  private:
    vector<target_type_rule_map> map_;
  };

  // This is another indexed map but this time meta_operation_id is the
  // index. The implementation is different, however: here we use a linked
  // list with the first, statically-allocated node corresponding to the
  // perform meta-operation. The idea is to try and get away with a dynamic
  // allocation for the common cases since most rules will be registered
  // for perform, at least on non-root scopes.
  //
  // Note: duplicate insertions (e.g., to global scope rule map) are ignored.
  //
  // @@ Redo using small_vector?
  //
  class rule_map
  {
  public:
    // Return false in case of a duplicate.
    //
    bool
    insert (action_id a,
            const target_type& tt,
            string name,
            const rule& r)
    {
      return insert (a >> 4, a & 0x0F, tt, move (name), r);
    }

    template <typename T>
    bool
    insert (action_id a, string name, const rule& r)
    {
      return insert (a, T::static_type, move (name), r);
    }

    // 0 oid is a wildcard.
    //
    bool
    insert (meta_operation_id mid,
            operation_id oid,
            const target_type& tt,
            string name,
            const rule& r)
    {
      if (mid_ == mid)
        return map_.insert (oid, tt, move (name), r);
      else
      {
        if (next_ == nullptr)
          next_.reset (new rule_map (mid));

        return next_->insert (mid, oid, tt, move (name), r);
      }
    }

    template <typename T>
    bool
    insert (meta_operation_id mid,
            operation_id oid,
            string name,
            const rule& r)
    {
      return insert (mid, oid, T::static_type, move (name), r);
    }

    // Return NULL if not found.
    //
    const operation_rule_map*
    operator[] (meta_operation_id mid) const
    {
      return mid == mid_ ? &map_ : next_ == nullptr ? nullptr : (*next_)[mid];
    }

    explicit
    rule_map (meta_operation_id mid = perform_id): mid_ (mid) {}

    bool
    empty () const {return map_.empty () && next_ == nullptr;}

  private:
    meta_operation_id mid_;
    operation_rule_map map_;
    unique_ptr<rule_map> next_;
  };
}

#endif // LIBBUILD2_RULE_MAP_HXX
