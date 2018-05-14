// file      : build2/operation.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_OPERATION_HXX
#define BUILD2_OPERATION_HXX

#include <libbutl/string-table.mxx>

#include <build2/types.hxx>
#include <build2/utility.hxx>

#include <build2/variable.hxx>
#include <build2/target-state.hxx>

namespace build2
{
  class location;
  class scope;
  class target_key;
  class target;

  struct opspec;

  // While we are using uint8_t for the meta/operation ids, we assume
  // that each is limited to 4 bits (max 128 entries) so that we can
  // store the combined action id in uint8_t as well. This makes our
  // life easier when it comes to defining switch labels for action
  // ids (no need to mess with endian-ness).
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
  // (e.g., because of other inner rules). A typical implementation of an
  // outer rule either returns noop or delegates to the inner rule. In
  // particular, it should not replace or override the inner's logic.
  //
  // While most of the relevant target state is duplicated, certain things are
  // shared among the inner/outer rules, such as the target data pad and the
  // group state. In particular, it is assumed the group state is always
  // determined by the inner rule (see resolve_members()).
  //
  // Normally, an outer rule will be responsible for any additional, outer
  // operation-specific work. Sometimes, however, the inner rule needs to
  // customize its behavior. In this case the outer and inner rules must
  // communicate this explicitly (normally via the target's data pad) and
  // there is a number of restrictions to this approach. See
  // cc::{link,install}_rule for details.
  //
  struct action
  {
    action (): inner_id (0), outer_id (0) {} // Invalid action.

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

  bool operator>  (action, action) = delete;
  bool operator<  (action, action) = delete;
  bool operator>= (action, action) = delete;
  bool operator<= (action, action) = delete;

  ostream&
  operator<< (ostream&, action);

  // Inner/outer operation state container.
  //
  template <typename T>
  struct action_state
  {
    T states[2]; // [0] -- inner, [1] -- outer.

    T&       operator[] (action a)       {return states[a.inner () ? 0 : 1];}
    const T& operator[] (action a) const {return states[a.inner () ? 0 : 1];}
  };

  // Id constants for build-in and pre-defined meta/operations.
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
  const operation_id default_id            = 1; // Shall be first.
  const operation_id update_id             = 2; // Shall be second.
  const operation_id clean_id              = 3;

  const operation_id test_id               = 4;
  const operation_id update_for_test_id    = 5; // update(for test) alias.

  const operation_id install_id            = 6;
  const operation_id uninstall_id          = 7;
  const operation_id update_for_install_id = 8; // update(for install) alias.

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

  // Meta-operation info.
  //

  // Normally a list of resolved and matched targets to execute. But can be
  // something else, depending on the meta-operation.
  //
  // The state is used to print structured result state. If it is not unknown,
  // then this is assumed to be a target.
  //
  struct action_target
  {
    using target_type = build2::target;

    const void* target = nullptr;
    target_state state = target_state::unknown;

    action_target () = default;
    action_target (const void* t): target (t) {}

    const target_type&
    as_target () const {return *static_cast<const target_type*> (target);}
  };

  class action_targets: public vector<action_target>
  {
  public:
    using vector<action_target>::vector;

    void
    reset () {for (auto& x: *this) x.state = target_state::unknown;}
  };

  struct meta_operation_info
  {
    const meta_operation_id id;
    const string name;

    // Name derivatives for diagnostics. If empty, then the meta-
    // operation need not be mentioned.
    //
    const string name_do;           // E.g., [to] 'configure'.
    const string name_doing;        // E.g., [while] 'configuring'.
    const string name_did;          // E.g., 'configured'.
    const string name_done;         // E.g., 'is configured'.

    // Whether to bootstrap outer projects. If load() below calls load_root(),
    // then this must be true. Note that this happens before
    // meta_operation_pre() is called.
    //
    const bool bootstrap_outer;

    // The first argument in all the callback is the meta-operation
    // parameters.
    //
    // If the meta-operation expects parameters, then it should have a
    // non-NULL meta_operation_pre(). Failed that, any parameters will be
    // diagnosed as unexpected.

    // Start of meta-operation and operation batches.
    //
    // If operation_pre() is not NULL, then it may translate default_id
    // (and only default_id) to some other operation. If not translated,
    // then default_id is used. If, however, operation_pre() is NULL,
    // then default_id is translated to update_id.
    //
    void (*meta_operation_pre) (const values&, const location&);
    operation_id (*operation_pre) (const values&, operation_id);

    // Meta-operation-specific logic to load the buildfile, search and match
    // the targets, and execute the action on the targets.
    //
    void (*load) (const values&,
                  scope& root,
                  const path& buildfile,
                  const dir_path& out_base,
                  const dir_path& src_base,
                  const location&);

    void (*search) (const values&,
                    const scope& root,
                    const scope& base,
                    const target_key&,
                    const location&,
                    action_targets&);

    // Diagnostics levels:
    //
    // 0 - none           (for structured result).
    // 1 - failures only  (for pre-operations).
    // 2 - all            (for normal operations).
    //
    // The false progress argument can be used to suppress progress. If it is
    // true, then whether the progress is shown is meta operation-specific (in
    // other words, you can suppress it but not force it).
    //
    void (*match) (const values&, action, action_targets&,
                   uint16_t diag, bool progress);

    void (*execute) (const values&, action, action_targets&,
                     uint16_t diag, bool progress);

    // Start of operation and meta-operation batches.
    //
    void (*operation_post) (const values&, operation_id);
    void (*meta_operation_post) (const values&);
  };

  // Built-in meta-operations.
  //

  // perform
  //

  // Load the buildfile. This is the default implementation that first
  // calls root_pre(), then creates the scope for out_base, and, finally,
  // loads the buildfile unless it has already been loaded for the root
  // scope.
  //
  void
  load (const values&,
        scope&,
        const path&,
        const dir_path&,
        const dir_path&,
        const location&);

  // Search and match the target. This is the default implementation
  // that does just that and adds a pointer to the target to the list.
  //
  void
  search (const values&,
          const scope&,
          const scope&,
          const target_key&,
          const location&,
          action_targets&);

  void
  match (const values&, action, action_targets&,
         uint16_t diag, bool prog);

  // Execute the action on the list of targets. This is the default
  // implementation that does just that while issuing appropriate
  // diagnostics (unless quiet).
  //
  void
  execute (const values&, action, const action_targets&,
           uint16_t diag, bool prog);

  extern const meta_operation_info mo_noop;
  extern const meta_operation_info mo_perform;
  extern const meta_operation_info mo_info;

  // Operation info.
  //
  // NOTE: keep POD-like to ensure can be constant-initialized in order to
  //       sidestep static initialization order (relied upon in operation
  //       aliasing).
  //
  struct operation_info
  {
    // If outer_id is not 0, then use that as the outer part of the
    // action.
    //
    const operation_id id;
    const operation_id outer_id;
    const char* name;

    // Name derivatives for diagnostics. Note that unlike meta-operations,
    // these can only be empty for the default operation (id 1), And
    // meta-operations that make use of the default operation shall not
    // have empty derivatives (failed which only target name will be
    // printed).
    //
    const char* name_do;     // E.g., [to] 'update'.
    const char* name_doing;  // E.g., [while] 'updating'.
    const char* name_did;    // E.g., [not] 'updated'.
    const char* name_done;   // E.g., 'is up to date'.

    const execution_mode mode;

    // This is the operation's concurrency multiplier. 0 means run serially,
    // 1 means run at hardware concurrency (unless overridden by the user).
    //
    const size_t concurrency;

    // The first argument in all the callback is the operation parameters.
    //
    // If the operation expects parameters, then it should have a non-NULL
    // pre(). Failed that, any parameters will be diagnosed as unexpected.

    // If the returned operation_id's are not 0, then they are injected
    // as pre/post operations for this operation. Can be NULL if unused.
    // The returned operation_id shall not be default_id.
    //
    operation_id (*pre) (const values&, meta_operation_id, const location&);
    operation_id (*post) (const values&, meta_operation_id);
  };

  // Built-in operations.
  //
  extern const operation_info op_default;
  extern const operation_info op_update;
  extern const operation_info op_clean;

  // Global meta/operation tables. Each registered meta/operation
  // is assigned an id which is used as an index in the per-project
  // registered meta/operation lists.
  //
  // We have three types of meta/operations: built-in (e.g., perform,
  // update), pre-defined (e.g., configure, test), and dynamically-
  // defined. For built-in ones, both the id and implementation are
  // part of the build2 core. For pre-defined, the id is registered
  // as part of the core but the implementation is loaded as part of
  // a module. The idea with pre-defined operations is that they have
  // common, well-established semantics but could still be optional.
  // Another aspect of pre-defined operations is that often rules
  // across multiple modules need to know their ids. Finally,
  // dynamically-defined meta/operations have their ids registered
  // as part of a module load. In this case, the meta/operation is
  // normally (but not necessarily) fully implemented by this module.
  //
  // Note also that the name of a meta/operation in a sense defines
  // its semantics. It would be strange to have an operation called
  // test that does two very different things in different projects.
  //
  // A built-in/pre-defined meta-operation can also provide a pre-processor
  // callback that will be called for operation-specs before any project
  // discovery/bootstrap is performed.
  //
  struct meta_operation_data
  {
    // The processor may modify the parameters, opspec, and change the
    // meta-operation by returning a different name.
    //
    // If lifted is true then the operation name in opspec is bogus (has
    // been lifted) and the default/empty name should be assumed instead.
    //
    using process_func = const string& (const variable_overrides&,
                                        values&,
                                        vector_view<opspec>&,
                                        bool lifted,
                                        const location&);

    meta_operation_data () = default;
    meta_operation_data (const char* n, process_func p = nullptr)
        : name (n), process (p) {}

    string name;
    process_func* process;
  };

  extern butl::string_table<meta_operation_id,
                            meta_operation_data> meta_operation_table;
  extern butl::string_table<operation_id> operation_table;

  // These are "sparse" in the sense that we may have "holes" that
  // are represented as NULL pointers. Also, lookup out of bounds
  // is treated as a hole.
  //
  template <typename T>
  struct sparse_vector
  {
    using base_type = vector<T*>;
    using size_type = typename base_type::size_type;

    void
    insert (size_type i, T& x)
    {
      size_type n (v_.size ());

      if (i < n)
        v_[i] = &x;
      else
      {
        if (n != i)
          v_.resize (i, nullptr); // Add holes.
        v_.push_back (&x);
      }
    }

    T*
    operator[] (size_type i) const
    {
      return i < v_.size () ? v_[i] : nullptr;
    }

    bool
    empty () const {return v_.empty ();}

    // Note that this is more of a "max index" rather than size.
    //
    size_type
    size () const {return v_.size ();}

  private:
    base_type v_;
  };

  using meta_operations = sparse_vector<const meta_operation_info>;
  using operations = sparse_vector<const operation_info>;
}

namespace butl
{
  template <>
  struct string_table_traits<build2::meta_operation_data>
  {
    static const std::string&
    key (const build2::meta_operation_data& d) {return d.name;}
  };
}

#endif // BUILD2_OPERATION_HXX
