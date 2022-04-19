// file      : libbuild2/action.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_ACTION_HXX
#define LIBBUILD2_ACTION_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // While we are using uint8_t for the meta/operation ids, we assume that
  // each is limited to 4 bits (max 15 entries @@ this is probably too low) so
  // that we can store the combined action id in uint8_t as well. This makes
  // our life easier when it comes to defining switch labels for action ids
  // (no need to mess with endian-ness).
  //
  // Note that 0 is not a valid meta/operation/action id.
  //
  using meta_operation_id = uint8_t;
  using operation_id = uint8_t;
  using action_id = uint8_t;

  // Meta-operations and operations are not the end of the story. We also have
  // operation nesting (currently only one level deep) which is used to
  // implement pre/post operations (currently, but may be useful for other
  // things). Here is the idea: the test operation needs to make sure that the
  // targets that it needs to test are up-to-date. So it runs update as its
  // pre-operation. It is almost like an ordinary update except that it has
  // test as its outer operation (the meta-operations are always the same).
  // This way a rule can recognize that this is "update for test" and do
  // something differently. For example, if an executable is not a test, then
  // there is no use updating it. At the same time, most rules will ignore the
  // fact that this is a nested update and for them it is "update as usual".
  //
  // This inner/outer operation support is implemented by maintaining two
  // independent "target states" (see target::state; initially we tried to do
  // it via rule/recipe override but that didn't end up well, to put it
  // mildly). While the outer operation normally "directs" the inner, inner
  // rules can still be matched/executed directly, without outer's involvement
  // (e.g., because of dependencies in other inner rules). A typical
  // implementation of an outer rule either returns noop or delegates to the
  // inner rule. In particular, it should not replace or override the inner's
  // logic.
  //
  // While most of the action-specific target state is duplicated (see
  // target::opstate), certain things are shared among the inner/outer rules,
  // such as the path, mtime, and group state. In particular, it is assumed
  // the group state is always determined by the inner rule (see
  // resolve_members()).
  //
  // Normally, an outer rule will be responsible for any additional, outer
  // operation-specific work. Sometimes, however, the inner rule needs to
  // customize its behavior. In this case the outer and inner rules must
  // communicate this explicitly (normally via the target's auxiliary data
  // storage) and there is a number of restrictions to this approach. See
  // cc::{link,install}_rule for details.
  //
  struct action
  {
    action (): inner_id (0), outer_id (0) {} // Invalid action.

    action (action_id a): action (a >> 4, a & 0xF) {}

    // If this is not a nested operation, then outer should be 0.
    //
    action (meta_operation_id m, operation_id inner, operation_id outer = 0)
        : inner_id ((m << 4) | inner),
          outer_id (outer == 0 ? 0 : (m << 4) | outer) {}

    meta_operation_id
    meta_operation () const {return inner_id >> 4;}

    operation_id
    operation () const {return inner_id & 0xF;}

    operation_id
    outer_operation () const {return outer_id & 0xF;}

    bool inner () const {return outer_id == 0;}
    bool outer () const {return outer_id != 0;}

    action
    inner_action () const
    {
      return action (meta_operation (), operation ());
    }

    // Implicit conversion operator to action_id for the switch() statement,
    // etc. Most places only care about the inner operation.
    //
    operator action_id () const {return inner_id;}

    action_id inner_id;
    action_id outer_id;
  };

  inline bool
  operator== (action x, action y)
  {
    return x.inner_id == y.inner_id && x.outer_id == y.outer_id;
  }

  inline bool
  operator!= (action x, action y) {return !(x == y);}

  inline bool operator== (action x, action_id y) {return x == action (y);}
  inline bool operator!= (action x, action_id y) {return x != action (y);}
  inline bool operator== (action_id x, action y) {return action (x) == y;}
  inline bool operator!= (action_id x, action y) {return action (x) == y;}

  bool operator>  (action, action) = delete;
  bool operator<  (action, action) = delete;
  bool operator>= (action, action) = delete;
  bool operator<= (action, action) = delete;

  // Note: prints in numeric form (mostly used in tracing).
  //
  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, action); // operation.cxx

  // Inner/outer operation state container.
  //
  template <typename T>
  struct action_state
  {
    T inner;
    T outer;

    T&       operator[] (action a)       {return a.inner () ? inner : outer;}
    const T& operator[] (action a) const {return a.inner () ? inner : outer;}

    action_state () = default;

  // Miscompiled by VC14.
  //
#if !defined(_MSC_VER) || _MSC_VER > 1900
    template <typename... A>
    explicit
    action_state (A&&... a)
        : inner (forward<A> (a)...), outer (forward<A> (a)...) {}
#else
    template <typename A>
    explicit
    action_state (A& a): inner (a), outer (a) {}
#endif
  };

  // Id constants for build-in and pre-defined meta/operations.
  //
  // Note: currently max 15 (see above).
  // Note: update small_vector in meta_operations if adding more.
  //
  const meta_operation_id noop_id      = 1; // nomop?
  const meta_operation_id perform_id   = 2;
  const meta_operation_id configure_id = 3;
  const meta_operation_id disfigure_id = 4;
  const meta_operation_id create_id    = 5;
  const meta_operation_id dist_id      = 6;
  const meta_operation_id info_id      = 7;

  // The default operation is a special marker that can be used to indicate
  // that no operation was explicitly specified by the user. If adding
  // something here remember to update the man page.
  //
  // Note: currently max 15 (see above).
  // Note: update small_vector in operations if adding more.
  //
  const operation_id default_id            = 1; // Shall be first.
  const operation_id update_id             = 2; // Shall be second.
  const operation_id clean_id              = 3;

  const operation_id test_id               = 4;
  const operation_id update_for_test_id    = 5; // update(for test) alias.

  const operation_id install_id            = 6;
  const operation_id uninstall_id          = 7;
  const operation_id update_for_install_id = 8; // update(for install) alias.

  // Commonly-used action ids.
  //
  const action_id perform_update_id     = (perform_id << 4) | update_id;
  const action_id perform_clean_id      = (perform_id << 4) | clean_id;
  const action_id perform_test_id       = (perform_id << 4) | test_id;
  const action_id perform_install_id    = (perform_id << 4) | install_id;
  const action_id perform_uninstall_id  = (perform_id << 4) | uninstall_id;

  const action_id configure_update_id   = (configure_id << 4) | update_id;

  // Recipe execution mode.
  //
  // When a target is a prerequisite of another target, its recipe can be
  // executed before the dependent's recipe (the normal case) or after.
  // We will call these "front" and "back" execution modes, respectively
  // (think "the prerequisite is 'front-running' the dependent").
  //
  // There could also be several dependent targets and the prerequisite's
  // recipe can be execute as part of the first dependent (the normal
  // case) or last (or for all/some of them; see the recipe execution
  // protocol in <target>). We will call these "first" and "last"
  // execution modes, respectively.
  //
  // Now you may be having a hard time imagining where a mode other than
  // the normal one (first/front) could be useful. An the answer is,
  // compensating or inverse operations such as clean, uninstall, etc.
  // If we use the last/back mode for, say, clean, then we will remove
  // targets in the order inverse to the way they were updated. While
  // this sounds like an elegant idea, are there any practical benefits
  // of doing it this way? As it turns out there is (at least) one: when
  // we are removing a directory (see fsdir{}), we want to do it after
  // all the targets that depend on it (such as files, sub-directories)
  // were removed. If we do it before, then the directory won't be empty
  // yet.
  //
  // It appears that this execution mode is dictated by the essence of
  // the operation. Constructive operations (those that "do") seem to
  // naturally use the first/front mode. That is, we need to "do" the
  // prerequisite first before we can "do" the dependent. While the
  // destructive ones (those that "undo") seem to need last/back. That
  // is, we need to "undo" all the dependents before we can "undo" the
  // prerequisite (say, we need to remove all the files before we can
  // remove their directory).
  //
  // If you noticed the parallel with the way C++ construction and
  // destruction works for base/derived object then you earned a gold
  // star!
  //
  // Note that the front/back mode is realized in the dependen's recipe
  // (which is another indication that it is a property of the operation).
  //
  enum class execution_mode {first, last};
}

#endif // LIBBUILD2_ACTION_HXX
