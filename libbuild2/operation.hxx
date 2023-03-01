// file      : libbuild2/operation.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_OPERATION_HXX
#define LIBBUILD2_OPERATION_HXX

#include <libbutl/string-table.hxx>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>
#include <libbuild2/recipe.hxx>
#include <libbuild2/target-state.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
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
    const void* target = nullptr;
    target_state state = target_state::unknown;

    action_target () = default;
    action_target (const void* t): target (t) {}

    template <typename T>
    const T&
    as () const {return *static_cast<const T*> (target);}
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

    // The first argument in all the callbacks is the meta-operation
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
    void (*meta_operation_pre) (context&, const values&, const location&);
    operation_id (*operation_pre) (context&, const values&, operation_id);

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
    // Note: not called in case any of the earlier callbacks failed.
    //
    void (*operation_post) (context&, const values&, operation_id);
    void (*meta_operation_post) (context&, const values&);

    // Optional prerequisite exclusion override callback. See include() for
    // details. Note that it's not called for include_type::normal without
    // operation-specific override.
    //
    include_type (*include) (action,
                             const target&,
                             const prerequisite_member&,
                             include_type,
                             lookup&);
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
  perform_load (const values&,
                scope&,
                const path&,
                const dir_path&,
                const dir_path&,
                const location&);

  // Search and match the target. This is the default implementation
  // that does just that and adds a pointer to the target to the list.
  //
  LIBBUILD2_SYMEXPORT void
  perform_search (const values&,
                  const scope&,
                  const scope&,
                  const path&,
                  const target_key&,
                  const location&,
                  action_targets&);

  LIBBUILD2_SYMEXPORT void
  perform_match (const values&, action, action_targets&,
                 uint16_t diag, bool prog);

  // Execute the action on the list of targets. This is the default
  // implementation that does just that while issuing appropriate
  // diagnostics (unless quiet).
  //
  LIBBUILD2_SYMEXPORT void
  perform_execute (const values&, action, const action_targets&,
                   uint16_t diag, bool prog);

  LIBBUILD2_SYMEXPORT extern const meta_operation_info mo_noop;
  LIBBUILD2_SYMEXPORT extern const meta_operation_info mo_perform;
  LIBBUILD2_SYMEXPORT extern const meta_operation_info mo_info;

  // Return true if params does not contain no_subprojects.
  //
  LIBBUILD2_SYMEXPORT bool
  info_subprojects (const values& params);

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

    // This is the operation's concurrency multiplier. 0 means run serially, 1
    // means run at hardware concurrency (or the concurrency specified by the
    // user).
    //
    // Note: 0 and 1 are currently the only valid values.
    //
    const size_t concurrency;

    // The values argument in the callbacks is the operation parameters. If
    // the operation expects parameters, then it should have a non-NULL
    // operation_pre() callback. Failed that, any parameters will be diagnosed
    // as unexpected.
    //
    // Note also that if the specified operation has outer (for example,
    // update-for-install), then parameters belong to outer (for example,
    // install; this is done in order to be consistent with the case when
    // update is performed as a pre-operation of install).

    // Pre/post operations for this operation. Note that these callbacks are
    // called before this operation becomes current.
    //
    // If the returned by pre/post_*() operation_id's are not 0, then they are
    // injected as pre/post operations for this operation. Can be NULL if
    // unused. The returned operation_id shall not be default_id.
    //
    operation_id (*pre_operation) (
      context&, const values&, meta_operation_id, const location&);

    operation_id (*post_operation) (
      context&, const values&, meta_operation_id);

    // Called immediately after/before this operation becomes/ceases to be
    // current operation for the specified context. Can be used to
    // initialize/finalize operation-specific data (context::current_*_odata).
    // Can be NULL if unused.
    //
    void (*operation_pre) (
      context&, const values&, bool inner, const location&);
    void (*operation_post) (
      context&, const values&, bool inner);

    // Operation-specific ad hoc rule callbacks. Essentially, if not NULL,
    // then every ad hoc rule match and apply call for this operation is
    // proxied through these functions.
    //
    bool (*adhoc_match) (const adhoc_rule&,
                         action, target&, const string&, match_extra&);

    recipe (*adhoc_apply) (const adhoc_rule&, action, target&, match_extra&);
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

  using meta_operation_table = butl::string_table<meta_operation_id,
                                                  meta_operation_data>;

  using operation_table = butl::string_table<operation_id>;

  // This is a "sparse" vector in the sense that we may have "holes" that are
  // represented as default-initialized empty instances (for example, NULL if
  // T is a pointer). Also, lookup out of bounds is treated as a hole.
  //
  template <typename T, size_t N>
  struct sparse_vector
  {
    using base_type = small_vector<T, N>;
    using size_type = typename base_type::size_type;

    void
    insert (size_type i, T x)
    {
      size_type n (v_.size ());

      if (i < n)
        v_[i] = x;
      else
      {
        if (n != i)
          v_.resize (i, T ()); // Add holes.

        v_.push_back (move (x));
      }
    }

    T
    operator[] (size_type i) const
    {
      return i < v_.size () ? v_[i] : T ();
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

  // For operations we keep both the pointer to its description as well
  // as to its operation variable (see var_include) which may belong to
  // the project-private variable pool.
  //
  struct project_operation_info
  {
    const operation_info* info = nullptr;
    const variable*       ovar = nullptr; // Operation variable.

    // Allow treating it as pointer to operation_info in most contexts.
    //
    operator const operation_info*() const {return info;}
    bool operator== (nullptr_t) {return info == nullptr;}
    bool operator!= (nullptr_t) {return info != nullptr;}

    project_operation_info (const operation_info* i = nullptr, // VC14
                            const variable* v = nullptr)
        : info (i), ovar (v) {}
  };

  using meta_operations = sparse_vector<const meta_operation_info*, 8>;
  using operations = sparse_vector<project_operation_info, 10>;
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
