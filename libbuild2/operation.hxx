// file      : libbuild2/operation.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_OPERATION_HXX
#define LIBBUILD2_OPERATION_HXX

#include <libbutl/string-table.mxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/target-state.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  class location;
  class scope;
  class target;
  class target_key;
  class context;
  class include_type;
  struct prerequisite_member;

  struct opspec;

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
                    const path& buildfile,
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

    // End of operation and meta-operation batches.
    //
    void (*operation_post) (const values&, operation_id);
    void (*meta_operation_post) (const values&);

    // Optional prerequisite inclusion/exclusion override callback. See
    // include() for details.
    //
    include_type (*include) (action,
                             const target&,
                             const prerequisite_member&,
                             include_type);
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
  LIBBUILD2_SYMEXPORT void
  load (const values&,
        scope&,
        const path&,
        const dir_path&,
        const dir_path&,
        const location&);

  // Search and match the target. This is the default implementation
  // that does just that and adds a pointer to the target to the list.
  //
  LIBBUILD2_SYMEXPORT void
  search (const values&,
          const scope&,
          const scope&,
          const path&,
          const target_key&,
          const location&,
          action_targets&);

  LIBBUILD2_SYMEXPORT void
  match (const values&, action, action_targets&,
         uint16_t diag, bool prog);

  // Execute the action on the list of targets. This is the default
  // implementation that does just that while issuing appropriate
  // diagnostics (unless quiet).
  //
  LIBBUILD2_SYMEXPORT void
  execute (const values&, action, const action_targets&,
           uint16_t diag, bool prog);

  LIBBUILD2_SYMEXPORT extern const meta_operation_info mo_noop;
  LIBBUILD2_SYMEXPORT extern const meta_operation_info mo_perform;
  LIBBUILD2_SYMEXPORT extern const meta_operation_info mo_info;

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
  LIBBUILD2_SYMEXPORT extern const operation_info op_default;
  LIBBUILD2_SYMEXPORT extern const operation_info op_update;
  LIBBUILD2_SYMEXPORT extern const operation_info op_clean;

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
    using process_func = const string& (context&,
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

  inline ostream&
  operator<< (ostream& os, const meta_operation_data& d)
  {
    return os << d.name;
  }

  LIBBUILD2_SYMEXPORT extern butl::string_table<meta_operation_id,
                                                meta_operation_data>
  meta_operation_table;

  LIBBUILD2_SYMEXPORT extern butl::string_table<operation_id> operation_table;

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

#endif // LIBBUILD2_OPERATION_HXX
