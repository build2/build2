// file      : libbuild2/rule-map.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_RULE_MAP_HXX
#define LIBBUILD2_RULE_MAP_HXX

#include <libbutl/prefix-map.mxx>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>

namespace build2
{
  using hint_rule_map =
    butl::prefix_map<string, reference_wrapper<const rule>, '.'>;

  using target_type_rule_map = map<const target_type*, hint_rule_map>;

  // This is an "indexed map" with operation_id being the index. Entry
  // with id 0 is a wildcard.
  //
  // Note that while we may resize some vectors during non-serial load, this
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
            string hint,
            const rule& r)
    {
      // 3 is the number of builtin operations.
      //
      if (oid >= map_.size ())
        map_.resize ((oid < 3 ? 3 : oid) + 1);

      return map_[oid][&tt].emplace (move (hint), r).second;
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
            string hint,
            const rule& r)
    {
      return insert (a >> 4, a & 0x0F, tt, move (hint), r);
    }

    template <typename T>
    bool
    insert (action_id a, string hint, const rule& r)
    {
      return insert (a, T::static_type, move (hint), r);
    }

    // 0 oid is a wildcard.
    //
    bool
    insert (meta_operation_id mid,
            operation_id oid,
            const target_type& tt,
            string hint,
            const rule& r)
    {
      if (mid_ == mid)
        return map_.insert (oid, tt, move (hint), r);
      else
      {
        if (next_ == nullptr)
          next_.reset (new rule_map (mid));

        return next_->insert (mid, oid, tt, move (hint), r);
      }
    }

    template <typename T>
    bool
    insert (meta_operation_id mid,
            operation_id oid,
            string hint,
            const rule& r)
    {
      return insert (mid, oid, T::static_type, move (hint), r);
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
