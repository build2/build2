// file      : libbuild2/scope.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCOPE_HXX
#define LIBBUILD2_SCOPE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/module.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/rule-map.hxx>
#include <libbuild2/operation.hxx>
#include <libbuild2/target-key.hxx>
#include <libbuild2/target-type.hxx>
#include <libbuild2/target-state.hxx>
#include <libbuild2/prerequisite-key.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  class dir;

  using subprojects = map<project_name, dir_path>;

  // Print as name@dir sequence.
  //
  // Note: trailing slash is not printed for the directory path.
  //
  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const subprojects&);

  class LIBBUILD2_SYMEXPORT scope
  {
  public:
    // Context this scope belongs to.
    //
    context& ctx;

    // Absolute and normalized.
    //
    const dir_path& out_path () const {return *out_path_;}
    const dir_path& src_path () const {return *src_path_;}

    bool out_eq_src () const {return out_path_ == src_path_;}

    // These are pointers to the keys in scope_map. The second can be NULL
    // during bootstrap until initialized.
    //
    const dir_path* out_path_ = nullptr;
    const dir_path* src_path_ = nullptr;

    bool
    root () const;

    // Note that the *_scope() functions reaturn "logical" parent/root/etc
    // scopes, taking into account the project's var_amalgamation value.

    scope*       parent_scope ();
    const scope* parent_scope () const;

    // Root scope of this scope or NULL if this scope is not (yet) in any
    // (known) project. Note that if the scope itself is root, then this
    // function return this. To get to the outer root, query the root scope of
    // the parent.
    //
    scope*       root_scope ();
    const scope* root_scope () const;

    // Root scope of the outermost "strong" (source-based) amalgamation of
    // this scope that has a project name or NULL if this scope is not (yet)
    // in any (known) project. If there is no bundle amalgamation, then this
    // function returns the root scope of the project (in other words, in this
    // case a project is treated as its own bundle, even if it's unnamed).
    //
    scope*       bundle_scope ();
    const scope* bundle_scope () const;

    // Root scope of the outermost "strong" (source-based) amalgamation of
    // this scope or NULL if this scope is not (yet) in any (known) project.
    // If there is no strong amalgamation, then this function returns the root
    // scope of the project (in other words, in this case a project is treated
    // as its own strong amalgamation).
    //
    scope*       strong_scope ();
    const scope* strong_scope () const;

    // Root scope of the outermost amalgamation or NULL if this scope is not
    // (yet) in any (known) project. If there is no amalgamation, then this
    // function returns the root scope of the project (in other words, in this
    // case a project is treated as its own amalgamation).
    //
    scope*       weak_scope ();
    const scope* weak_scope () const;

    // Global scope.
    //
    scope&       global_scope () {return const_cast<scope&> (ctx.global_scope);}
    const scope& global_scope () const {return ctx.global_scope;}

    // Return true if the specified root scope is a sub-scope of (but not the
    // same as) this root scope. Note that both scopes must be root.
    //
    bool
    sub_root (const scope&) const;

    // Variables.
    //
  public:
    variable_map vars;

    // Lookup, including in outer scopes. If you only want to lookup in this
    // scope, do it on the the variables map directly (and note that there
    // will be no overrides).
    //
    using lookup_type = build2::lookup;

    lookup_type
    operator[] (const variable& var) const
    {
      return lookup (var).first;
    }

    lookup_type
    operator[] (const variable* var) const // For cached variables.
    {
      assert (var != nullptr);
      return operator[] (*var);
    }

    lookup_type
    operator[] (const string& name) const
    {
      const variable* var (var_pool ().find (name));
      return var != nullptr ? operator[] (*var) : lookup_type ();
    }

    // As above, but include target type/pattern-specific variables.
    //
    lookup_type
    lookup (const variable& var, const target_key& tk) const
    {
      return lookup (var, &tk).first;
    }

    lookup_type
    lookup (const variable& var,
            const target_key& tk,
            const target_key& gk) const
    {
      return lookup (var, &tk, &gk).first;
    }

    // Note for dir{} and fsdir{} target name is the directory leaf (without
    // the trailing slash). Also, if extension is to be matched (for this
    // target type), then it should be included in the name.
    //
    lookup_type
    lookup (const variable& var, const target_type& tt, const string& tn) const
    {
      return lookup (var, target_key {&tt, nullptr, nullptr, &tn, nullopt});
    }

    lookup_type
    lookup (const variable& var,
            const target_type& tt, const string& tn,
            const target_type& gt, const string& gn) const
    {
      return lookup (var,
                     target_key {&tt, nullptr, nullptr, &tn, nullopt},
                     target_key {&gt, nullptr, nullptr, &gn, nullopt});
    }

    // Note that target keys may be incomplete (only type and name must be
    // present plus dir for dir{} and fsdir{} targets if name is empty).
    //
    pair<lookup_type, size_t>
    lookup (const variable& var,
            const target_key* tk = nullptr,
            const target_key* gk = nullptr) const
    {
      auto p (lookup_original (var, tk, gk));
      return var.overrides == nullptr ? p : lookup_override (var, move (p));
    }

    // Implementation details (used by scope target lookup). The start_depth
    // can be used to skip a number of initial lookups.
    //
    pair<lookup_type, size_t>
    lookup_original (const variable&,
                     const target_key* tk = nullptr,
                     const target_key* g1k = nullptr,
                     const target_key* g2k = nullptr,
                     size_t start_depth = 1) const;

    pair<lookup_type, size_t>
    lookup_override (const variable& var,
                     pair<lookup_type, size_t> original,
                     bool target = false,
                     bool rule = false) const
    {
      return lookup_override_info (var, original, target, rule).lookup;
    }

    // As above but also return an indication of whether the resulting value
    // is/is based (e.g., via append/prepend overrides) on the original or an
    // "outright" override. Note that it will always be false if there is no
    // original.
    //
    struct override_info
    {
      pair<lookup_type, size_t> lookup;
      bool original;
    };

    override_info
    lookup_override_info (const variable&,
                          pair<lookup_type, size_t> original,
                          bool target = false,
                          bool rule = false) const;

    // Return a value suitable for assignment (or append if you only want to
    // append to the value from this scope). If the value does not exist in
    // this scope's map, then a new one with the NULL value is added and
    // returned. Otherwise the existing value is returned.
    //
    value&
    assign (const variable& var) {return vars.assign (var);}

    value&
    assign (const variable* var) {return vars.assign (var);} // For cached.

    template <typename T>
    T&
    assign (const variable& var, T&& val)
    {
      value& v (assign (var) = forward<T> (val));
      return v.as<T> ();
    }

    template <typename T>
    T&
    assign (const variable* var, T&& val)
    {
      value& v (assign (var) = forward<T> (val));
      return v.as<T> ();
    }

    // Assign an untyped non-overridable variable with project visibility.
    //
    value&
    assign (string name)
    {
      return assign (var_pool ().insert (move (name)));
    }

    // As above, but assign a typed variable (note: variable type must be
    // specified explicitly).
    //
    template <typename V>
    value&
    assign (string name)
    {
      return vars.assign (var_pool ().insert<V> (move (name)));
    }

    template <typename V, typename T>
    V&
    assign (string name, T&& val)
    {
      value& v (assign<V> (move (name)) = forward<T> (val));
      return v.as<V> ();
    }

    // Return a value suitable for appending. If the variable does not exist
    // in this scope's map, then outer scopes are searched for the same
    // variable. If found then a new variable with the found value is added to
    // this scope and returned. Otherwise this function proceeds as assign().
    //
    value&
    append (const variable&);

    value&
    append (string name)
    {
      return append (var_pool ().insert (move (name)));
    }

    template <typename V>
    value&
    append (string name)
    {
      return append (var_pool ().insert<V> (move (name)));
    }

    // Target type/pattern-specific variables.
    //
    variable_type_map target_vars;

    // Target types.
    //
    // Note that target types are project-wide (even if the module that
    // registers them is loaded in a base scope). The thinking here is that
    // having target types only visible in certain scopes of a project just
    // complicates and confuses things (e.g., you cannot refer to a target
    // whose buildfile you just included). On the other hand, it feels highly
    // unlikely that a target type will somehow need to be different for
    // different parts of the project (unlike, say, a rule).
    //
    // The target types are also project-local. This means one has to use
    // import to refer to targets across projects, even in own subprojects
    // (because we stop searching at project boundaries).
    //
    // See also context::global_target_types.
    //
  public:
    const target_type&
    insert_target_type (const target_type& tt)
    {
      return root_extra->target_types.insert (tt).first;
    }

    template <typename T>
    const target_type&
    insert_target_type ()
    {
      return root_extra->target_types.insert<T> ();
    }

    void
    insert_target_type_file (const string& n, const target_type& tt)
    {
      root_extra->target_types.insert_file (n, tt);
    }

    const target_type*
    find_target_type (const string&) const;

    // Given a target name, figure out its type, taking into account
    // extensions, special names (e.g., '.' and '..'), or anything else that
    // might be relevant. Process the name (in place) by extracting (and
    // returning) extension, adjusting dir/leaf, etc., (note that the dir is
    // not necessarily normalized). If the target type is already resolved,
    // then it can be passed as the last argument. Return NULL if not found.
    //
    pair<const target_type*, optional<string>>
    find_target_type (name&,
                      const location&,
                      const target_type* = nullptr) const;

    // As above but process the potentially out-qualified target name further
    // by completing (relative to this scope) and normalizing the directories
    // and also issuing appropriate diagnostics if the target type is unknown.
    // If the first argument has the pair flag true, then the second should be
    // the out directory.
    //
    pair<const target_type&, optional<string>>
    find_target_type (name&, name&,
                      const location&,
                      const target_type* = nullptr) const;

    // As above, but return the result as a target key (with its members
    // shallow-pointing to processed parts in the two names).
    //
    target_key
    find_target_key (name&, name&,
                     const location&,
                     const target_type* = nullptr) const;

    // As above, but the names are passed as a vector. Issue appropriate
    // diagnostics if the wrong number of names is passed.
    //
    target_key
    find_target_key (names&,
                     const location&,
                     const target_type* = nullptr) const;

    // Similar to the find_target_type() but does not complete relative
    // directories.
    //
    pair<const target_type&, optional<string>>
    find_prerequisite_type (name&, name&,
                            const location&,
                            const target_type* = nullptr) const;

    // As above, but return a prerequisite key.
    //
    prerequisite_key
    find_prerequisite_key (name&, name&,
                           const location&,
                           const target_type* = nullptr) const;

    prerequisite_key
    find_prerequisite_key (names&,
                           const location&,
                           const target_type* = nullptr) const;

    // Dynamically derive a new target type from an existing one. Return the
    // reference to the target type and an indicator of whether it was
    // actually created.
    //
    // Note: the flags are OR'ed to the base's flags.
    //
    pair<reference_wrapper<const target_type>, bool>
    derive_target_type (const string& name,
                        const target_type& base,
                        target_type::flag flags = target_type::flag::none);

    template <typename T>
    pair<reference_wrapper<const target_type>, bool>
    derive_target_type (const string& name)
    {
      return derive_target_type (name, T::static_type);
    }

    // Derive from an "exemplar" type overriding the factory.
    //
    const target_type&
    derive_target_type (const target_type&);

    // Rules.
    //
  public:
    rule_map rules;
    vector<unique_ptr<adhoc_rule_pattern>> adhoc_rules;

    template <typename T>
    void
    insert_rule (action_id a, string name, const rule& r)
    {
      rules.insert<T> (a, move (name), r);
    }

    // 0 meta-operation id is treated as an (emulated) wildcard.
    //
    // Emulated means that we just iterate over all the meta-operations known
    // to this project (and they should all be known at this point) and
    // register the rule for each of them.
    //
    template <typename T>
    void
    insert_rule (meta_operation_id, operation_id, string name, const rule&);

    // Operation callbacks.
    //
    // An entity (module, core) can register a function that will be called
    // when an action is executed on the dir{} target that corresponds to this
    // scope. The pre callback is called just before the recipe and the post
    // -- immediately after. The callbacks are only called if the recipe
    // (including noop recipe) is executed for the corresponding target. The
    // callbacks should only be registered during the load phase.
    //
    // It only makes sense for callbacks to return target_state changed or
    // unchanged and to throw failed in case of an error. These pre/post
    // states will be merged with the recipe state and become the target
    // state. See execute_recipe() for details.
    //
  public:
    struct operation_callback
    {
      using callback = target_state (action, const scope&, const dir&);

      function<callback> pre;
      function<callback> post;
    };

    using operation_callback_map = multimap<action_id, operation_callback>;

    operation_callback_map operation_callbacks;

    // Extra root scope-only data.
    //
  public:
    struct root_extra_type
    {
      // This project's name (var_project value). Absent means it is not yet
      // determined. NULL means simple project. Empty means unnamed project.
      //
      // Note that it is set to point to a temporary value before loading
      // bootstrap.build and to a permanent one (from the variable) after.
      //
      optional<const project_name*> project;

      // This project's amalgamation (var_amalgamation value). Absent means it
      // is not yet determined. NULL means amalgamation is disabled.
      //
      optional<const dir_path*> amalgamation;

      // This project's subprojects (var_subprojects value). Absent means it
      // is not yet determined (happens at the end of bootstrap_src()). NULL
      // means there are no subprojects.
      //
      optional<build2::subprojects*> subprojects;

      bool altn;   // True if using alternative build file/directory naming.
      bool loaded; // True if already loaded (load_root()).

      // Build file/directory naming scheme used by this project.
      //
      const string&   build_ext;        // build        or  build2     (no dot)
      const dir_path& build_dir;        // build/       or  build2/
      const path&     buildfile_file;   // buildfile    or  build2file
      const path&     buildignore_file; // buildignore  or  build2ignore

      const dir_path& root_dir;        // build[2]/root/
      const dir_path& bootstrap_dir;   // build[2]/bootstrap/
      const dir_path& build_build_dir; // build[2]/build/

      const path&     bootstrap_file; // build[2]/bootstrap.build[2]
      const path&     root_file;      // build[2]/root.build[2]
      const path&     export_file;    // build[2]/export.build[2]
      const path&     src_root_file;  // build[2]/bootstrap/src-root.build[2]
      const path&     out_root_file;  // build[2]/bootstrap/src-root.build[2]

      // Project-private variable pool.
      //
      // Note: see scope::var_pool_ and use scope::var_pool().
      //
      variable_pool var_pool;

      // Meta/operations supported by this project.
      //
      build2::meta_operations meta_operations;
      build2::operations operations;

      // Modules imported/loaded by this project.
      //
      module_import_map imported_modules;
      module_state_map  loaded_modules;

      // Buildfiles already loaded for this project.
      //
      // We don't expect too many of them per project so let's use vector
      // with linear search.
      //
      paths buildfiles;

      bool
      insert_buildfile (const path& f)
      {
        bool r (find (buildfiles.begin (),
                      buildfiles.end (),
                      f) == buildfiles.end ());
        if (r)
          buildfiles.push_back (f);

        return r;
      }

      // Variable override cache.
      //
      mutable variable_override_cache override_cache;

      // Target types.
      //
      target_type_map target_types;

      // Environment variable overrides.
      //
      // These overrides should be applied to the environment when running
      // tools (e.g., compilers) or querying environment variables from the
      // buildfiles and by the build system itself. Populated by the config
      // module and is not available during bootstrap (more precisely, not
      // available until before_first modules have been initialized). The list
      // is either empty of NULL-terminated.
      //
      // See also auto_project_env below.
      //
      vector<const char*> environment;

      // A checksum of the above environment variables (empty if there are
      // none). This can be used to take into account project environment
      // when, for example, caching environment-sensitive information.
      //
      string environment_checksum;

      root_extra_type (scope&, bool altn); // file.cxx
    };

    unique_ptr<root_extra_type> root_extra;

    // The last argument is the operation variable (see var_include) or NULL
    // if not used.
    //
    void
    insert_operation (operation_id id,
                      const operation_info& in,
                      const variable* ovar)
    {
      // The operation variable should have prerequisite or target visibility.
      //
      assert (ovar == nullptr ||
              (ovar->visibility == variable_visibility::prereq ||
               ovar->visibility == variable_visibility::target));

      root_extra->operations.insert (id, project_operation_info {&in, ovar});
    }

    void
    insert_meta_operation (meta_operation_id id, const meta_operation_info& in)
    {
      root_extra->meta_operations.insert (id, &in);
    }

    bool
    find_module (const string& name) const
    {
      return root_extra->loaded_modules.find_module<module> (name) != nullptr;
    }

    template <typename T>
    T*
    find_module (const string& name)
    {
      return root_extra->loaded_modules.find_module<T> (name);
    }

    template <typename T>
    const T*
    find_module (const string& name) const
    {
      return root_extra->loaded_modules.find_module<T> (name);
    }

  public:
    // RW access.
    //
    scope&
    rw () const
    {
      assert (ctx.phase == run_phase::load);
      return const_cast<scope&> (*this);
    }

    // Return the project-private variable pool (which is chained to the
    // public pool) unless pub is true, in which case return the public pool.
    //
    // You would normally go for the public pool directly as an optimization
    // (for example, in the module's init()) if you know all your variables
    // are qualified and thus public.
    //
    variable_pool&
    var_pool (bool pub = false)
    {
      return (pub                  ? ctx.var_pool      :
              var_pool_ != nullptr ? *var_pool_        :
              root_     != nullptr ? *root_->var_pool_ :
              ctx.var_pool).rw (*this);
    }

    const variable_pool&
    var_pool (bool pub = false) const
    {
      return (pub                  ? ctx.var_pool      :
              var_pool_ != nullptr ? *var_pool_        :
              root_     != nullptr ? *root_->var_pool_ :
              ctx.var_pool);
    }

  private:
    friend class parser;
    friend class scope_map;
    friend class temp_scope;

    // These from <libbuild2/file.hxx> set strong_.
    //
    friend LIBBUILD2_SYMEXPORT void create_bootstrap_outer (scope&, bool);
    friend LIBBUILD2_SYMEXPORT scope& create_bootstrap_inner (scope&,
                                                              const dir_path&);

    scope (context&, bool shared);
    ~scope ();

    // Return true if this root scope can be amalgamated.
    //
    bool
    amalgamatable () const;

    // Note that these values represent "physical" scoping relationships not
    // taking into account the project's var_amalgamation value.
    //
    scope* parent_;
    scope* root_;
    scope* strong_ = nullptr; // Only set on root scopes.
                              // NULL means no strong amalgamtion.

    variable_pool* var_pool_ = nullptr; // For temp_scope override.
  };

  inline bool
  operator== (const scope& x, const scope& y) { return &x == &y; }

  inline bool
  operator!= (const scope& x, const scope& y) { return !(x == y); }

  inline ostream&
  operator<< (ostream& os, const scope& s)
  {
    // Always absolute.
    //
    return to_stream (os, s.out_path (), true /* representation */);
  }

  // Automatic project environment setup/cleanup.
  //
  struct auto_project_env: auto_thread_env
  {
    auto_project_env () = default;

    explicit
    auto_project_env (nullptr_t p) // Clear current environment.
        : auto_thread_env (p) {}

    explicit
    auto_project_env (const scope& rs)
        : auto_thread_env (rs.root_extra->environment.empty ()
                           ? nullptr
                           : rs.root_extra->environment.data ()) {}
  };

  // Return the src/out directory corresponding to the given out/src. The
  // passed directory should be a sub-directory of out/src_root.
  //
  dir_path
  src_out (const dir_path& out, const scope& root);

  dir_path
  src_out (const dir_path& out,
           const dir_path& out_root, const dir_path& src_root);

  dir_path
  out_src (const dir_path& src, const scope& root);

  dir_path
  out_src (const dir_path& src,
           const dir_path& out_root, const dir_path& src_root);

  // Return the project name or empty if unnamed.
  //
  // Note that this function and named_project() below expect the root scope
  // to either be already bootstrapped or being src-bootstrapped (see
  // bootstrap_src()).
  //
  const project_name&
  project (const scope& root);

  // Return the name of the first innermost named project in the strong
  // amalgamation chain or empty if all are unnamed.
  //
  const project_name&
  named_project (const scope& root);

  // Temporary scope. The idea is to be able to create a temporary scope in
  // order not to change the variables in the current scope. Such a scope is
  // not entered in to the scope map and its parent is the global scope. As a
  // result it can only be used as a temporary set of variables. In
  // particular, defining targets directly in such a scope will surely end up
  // badly.
  //
  class temp_scope: public scope
  {
  public:
    temp_scope (scope& gs)
        : scope (gs.ctx, false /* shared */),
          var_pool_ (nullptr /* shared */, &gs.ctx.var_pool.rw (gs), nullptr)
    {
      // Note that making this scope its own root is a bad idea.
      //
      root_ = nullptr;
      parent_ = &gs;
      out_path_ = gs.out_path_;
      scope::var_pool_ = &var_pool_;
    }

  private:
    variable_pool var_pool_;
  };

  // Scope map. Protected by the phase mutex.
  //
  // While it contains both out and src paths, the latter is not available
  // during bootstrap (see setup_root() and setup_base() for details).
  //
  // Note also that the same src path can be naturally associated with
  // multiple out paths/scopes (and one of them may be the same as src).
  //
  class scope_map
  {
  public:
    // The first element, if not NULL, is for the "owning" out path. The rest
    // of the elements are for the src path shallow references.
    //
    // Note that the global scope is in the first element.
    //
    struct scopes: small_vector<scope*, 3>
    {
      scopes () = default;
      ~scopes () {if (!empty ()) delete front ();}

      scopes (scopes&&) = default; // For GCC 4.9
      scopes (const scopes&) = delete;
      scopes& operator= (scopes&&) = delete;
      scopes& operator= (const scopes&) = delete;
    };

    using map_type = dir_path_map<scopes>;

    using iterator = map_type::iterator;
    using const_iterator = map_type::const_iterator;

    // Insert a scope given its out path.
    //
    // Note that we assume the first insertion into the map is always the
    // global scope with empty key.
    //
    LIBBUILD2_SYMEXPORT iterator
    insert_out (const dir_path& our_path, bool root = false);

    // Insert a shallow reference to the scope for its src path.
    //
    LIBBUILD2_SYMEXPORT iterator
    insert_src (scope&, const dir_path& src_path);

    // Find the most qualified scope that encompasses this out path.
    //
    const scope&
    find_out (const dir_path& d) const
    {
      return const_cast<scope_map*> (this)->find_out (d);
    }

    // Find all the scopes that encompass this path (out or src).
    //
    // If skip_null_out is false, then the first element always corresponds to
    // the out scope and is NULL if there is none (see struct scopes above for
    // details).
    //
    // Note that the returned range will never be empty (there is always the
    // global scope).
    //
    // If the path is in src, then we may end up with multiple scopes. For
    // example, if two configurations of the same project are being built in a
    // single invocation. How can we pick the scope that is "ours", for some
    // definition of "ours"?
    //
    // The current thinking is that a project can be "associated" with other
    // projects: its sub-projects and imported projects (it doesn't feel like
    // its super-projects should be in this set, but maybe). And "ours" could
    // mean belonging to one of the associated projects. This feels correct
    // since a project shouldn't really be reaching into unrelated projects.
    // And a project can only import one instance of any given project.
    //
    // We could implement this by keeping track (in scope::root_extra) of all
    // the imported projects. The potential problem is performance: we would
    // need to traverse the imported projects set recursively (potentially
    // re-traversing the same projects multiple times).
    //
    // An alternative idea is to tag associated scopes with some marker so
    // that all the scopes that "know" about each other have the same tag,
    // essentially partitioning the scope set into connected subsets. One
    // issue here (other than the complexity of implementing something like
    // this) is that there could potentially be multiple source scopes with
    // the same tag (e.g., two projects that don't know anything about each
    // other could each import a different configuration of some common
    // project and in turn be both imported by yet another project thus all
    // acquiring the same tag). BTW, this could also be related to that
    // "island append" restriction we have on loading additional buildfile.
    //
    LIBBUILD2_SYMEXPORT pair<scopes::const_iterator, scopes::const_iterator>
    find (const dir_path&, bool skip_null_out = true) const;

    const_iterator begin () const {return map_.begin ();}
    const_iterator end () const {return map_.end ();}
    const_iterator find_exact (const dir_path& d) const {return map_.find (d);}

    // RW access.
    //
  public:
    scope_map&
    rw () const
    {
      assert (ctx.phase == run_phase::load);
      return const_cast<scope_map&> (*this);
    }

    scope_map&
    rw (scope&) const {return const_cast<scope_map&> (*this);}

  private:
    friend class context;

    explicit
    scope_map (context& c): ctx (c) {}

    LIBBUILD2_SYMEXPORT scope&
    find_out (const dir_path&);

  private:
    context& ctx;
    map_type map_;
  };
}

#include <libbuild2/scope.ixx>

#endif // LIBBUILD2_SCOPE_HXX
