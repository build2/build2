// file      : libbuild2/scope.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_SCOPE_HXX
#define LIBBUILD2_SCOPE_HXX

#include <map>
#include <unordered_set>

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

#include <libbuild2/export.hxx>

namespace build2
{
  class dir;

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

    // The first is a pointer to the key in scope_map. The second is a pointer
    // to the src_root/base variable value, if any (i.e., it can be NULL).
    //
    const dir_path* out_path_ = nullptr;
    const dir_path* src_path_ = nullptr;

    bool
    root () const {return root_ == this;}

    scope*       parent_scope ()       {return parent_;}
    const scope* parent_scope () const {return parent_;}

    // Root scope of this scope or NULL if this scope is not (yet) in any
    // (known) project. Note that if the scope itself is root, then this
    // function return this. To get to the outer root, query the root scope of
    // the parent.
    //
    scope*       root_scope ()       {return root_;}
    const scope* root_scope () const {return root_;}

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

    // Return true if the specified root scope is a sub-scope of this root
    // scope. Note that both scopes must be root.
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
      const variable* var (ctx.var_pool.find (name));
      return var != nullptr ? operator[] (*var) : lookup_type ();
    }

    // As above, but include target type/pattern-specific variables.
    //
    lookup_type
    lookup (const variable& var, const target_key& tk) const
    {
      return lookup (var, tk.type, tk.name).first;
    }

    lookup_type
    lookup (const variable& var, const target_type& tt, const string& tn) const
    {
      return lookup (var, &tt, &tn).first;
    }

    pair<lookup_type, size_t>
    lookup (const variable& var,
            const target_type* tt = nullptr,
            const string* tn = nullptr) const
    {
      auto p (lookup_original (var, tt, tn));
      return var.overrides == nullptr ? p : lookup_override (var, move (p));
    }

    // Implementation details (used by scope target lookup). The start_depth
    // can be used to skip a number of initial lookups.
    //
    pair<lookup_type, size_t>
    lookup_original (
      const variable&,
      const target_type* tt = nullptr, const string* tn = nullptr,
      const target_type* gt = nullptr, const string* gn = nullptr,
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

    // Assign an untyped non-overridable variable with project visibility.
    //
    value&
    assign (string name)
    {
      return assign (var_pool ().insert (move (name)));
    }

    // As above, but assign a typed variable.
    //
    template <typename T>
    value&
    assign (string name)
    {
      return vars.assign (var_pool ().insert<T> (move (name)));
    }

    template <typename T>
    T&
    assign (string name, T&& val)
    {
      value& v (assign<T> (move (name)) = forward<T> (val));
      return v.as<T> ();
    }

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

    // Return a value suitable for appending. If the variable does not
    // exist in this scope's map, then outer scopes are searched for
    // the same variable. If found then a new variable with the found
    // value is added to this scope and returned. Otherwise this
    // function proceeds as assign().
    //
    value&
    append (const variable&);

    // Target type/pattern-specific variables.
    //
    variable_type_map target_vars;

    // Set of buildfiles already loaded for this scope. The included
    // buildfiles are checked against the project's root scope while
    // imported -- against the global scope (global_scope).
    //
  public:
    std::unordered_set<path> buildfiles;

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
      return root_extra->target_types.insert (tt);
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
    // not necessarily normalized). Return NULL if not found.
    //
    pair<const target_type*, optional<string>>
    find_target_type (name&, const location&) const;

    // As above but process the potentially out-qualified target name further
    // by completing (relative to this scope) and normalizing the directories
    // and also issuing appropriate diagnostics if the target type is unknown.
    // If the first argument has the pair flag true, then the second should be
    // the out directory.
    //
    pair<const target_type&, optional<string>>
    find_target_type (name&, name&, const location&) const;

    // As above, but return the result as a target key (with its members
    // shallow-pointing to processed parts in the two names).
    //
    target_key
    find_target_key (name&, name&, const location&) const;

    // As above, but the names are passed as a vector. Issue appropriate
    // diagnostics if the wrong number of names is passed.
    //
    target_key
    find_target_key (names&, const location&) const;

    // Dynamically derive a new target type from an existing one. Return the
    // reference to the target type and an indicator of whether it was
    // actually created.
    //
    pair<reference_wrapper<const target_type>, bool>
    derive_target_type (const string& name, const target_type& base);

    template <typename T>
    pair<reference_wrapper<const target_type>, bool>
    derive_target_type (const string& name)
    {
      return derive_target_type (name, T::static_type);
    }

    // Rules.
    //
  public:
    rule_map rules;

    template <typename T>
    void
    insert_rule (action_id a, const char* hint, const rule& r)
    {
      rules.insert<T> (a, hint, r);
    }

    template <typename T>
    void
    insert_rule (meta_operation_id mid, operation_id oid,
                 const char* hint,
                 const rule& r)
    {
      rules.insert<T> (mid, oid, hint, r);
    }

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

    using operation_callback_map = std::multimap<action_id,
                                                 operation_callback>;

    operation_callback_map operation_callbacks;

    // Extra root scope-only data.
    //
  public:
    struct root_extra_type
    {
      bool altn; // True if using alternative build file/directory naming.

      // Build file/directory naming scheme used by this project.
      //
      const string&   build_ext;        // build        or  build2     (no dot)
      const dir_path& build_dir;        // build/       or  build2/
      const path&     buildfile_file;   // buildfile    or  build2file
      const path&     buildignore_file; // buildignore  or  build2ignore

      const dir_path& root_dir;       // build[2]/root/
      const dir_path& bootstrap_dir;  // build[2]/bootstrap/

      const path&     bootstrap_file; // build[2]/bootstrap.build[2]
      const path&     root_file;      // build[2]/root.build[2]
      const path&     export_file;    // build[2]/export.build[2]
      const path&     src_root_file;  // build[2]/bootstrap/src-root.build[2]
      const path&     out_root_file;  // build[2]/bootstrap/src-root.build[2]

      // Meta/operations supported by this project.
      //
      build2::meta_operations meta_operations;
      build2::operations operations;

      // Modules loaded by this project.
      //
      module_map modules;

      // Variable override cache.
      //
      mutable variable_override_cache override_cache;

      // Target types.
      //
      target_type_map target_types;
    };

    unique_ptr<root_extra_type> root_extra;

    void
    insert_operation (operation_id id, const operation_info& in)
    {
      root_extra->operations.insert (id, in);
    }

    void
    insert_meta_operation (meta_operation_id id, const meta_operation_info& in)
    {
      root_extra->meta_operations.insert (id, in);
    }

    bool
    find_module (const string& name) const
    {
      return root_extra->modules.find_module<module> (name) != nullptr;
    }

    template <typename T>
    T*
    find_module (const string& name) const
    {
      return root_extra->modules.find_module<T> (name);
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

    variable_pool&
    var_pool ()
    {
      return ctx.var_pool.rw (*this);
    }

  private:
    friend class parser;
    friend class scope_map;
    friend class temp_scope;

    // These two from <libbuild2/file.hxx> set strong_.
    //
    friend LIBBUILD2_SYMEXPORT void create_bootstrap_outer (scope&);
    friend LIBBUILD2_SYMEXPORT scope& create_bootstrap_inner (scope&,
                                                              const dir_path&);

    scope (context& c, bool global)
      : ctx (c), vars (c, global), target_vars (c, global) {}

    scope* parent_;
    scope* root_;
    scope* strong_ = nullptr; // Only set on root scopes.
                              // NULL means no strong amalgamtion.
  };

  inline ostream&
  operator<< (ostream& os, const scope& s)
  {
    // Always absolute.
    //
    return to_stream (os, s.out_path (), true /* representation */);
  }

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
  const project_name&
  project (const scope& root);

  // Return the name of the first innermost named project in the strong
  // amalgamation chain or empty if all are unnamed.
  //
  const project_name&
  named_project (const scope& root);

  // Temporary scope. The idea is to be able to create a temporary scope in
  // order not to change the variables in the current scope. Such a scope is
  // not entered in to the scope map. As a result it can only be used as a
  // temporary set of variables. In particular, defining targets directly in
  // such a scope will surely end up badly. Defining any nested scopes will be
  // as if defining such a scope in the parent (since path() returns parent's
  // path).
  //
  class temp_scope: public scope
  {
  public:
    temp_scope (scope& p)
        : scope (p.ctx, false /* global */)
    {
      out_path_ = p.out_path_;
      src_path_ = p.src_path_;
      parent_ = &p;
      root_ = p.root_;
      // No need to copy strong_ since we are never root scope.
    }
  };

  // Scope map.
  //
  // Protected by the phase mutex. Note that the scope map is only for paths
  // from the out tree.
  //
  using scope_map_base = dir_path_map<scope>;

  class scope_map: public scope_map_base
  {
  public:
    // Note that we assume the first insertion into the map is always the
    // global scope with empty key.
    //
    LIBBUILD2_SYMEXPORT iterator
    insert (const dir_path&, bool root = false);

    // Find the most qualified scope that encompasses this path.
    //
    const scope&
    find (const dir_path& d) const
    {
      return const_cast<scope_map*> (this)->find (d);
    }

    const scope&
    find (const path& p) const
    {
      // Natural thing to do here would be to call find (p.directory ()).
      // However, there could be a situation where the passed path is a
      // directory (i.e., the calling code does not know what it is dealing
      // with), so let's use the whole path.
      //
      // In fact, ideally, we should have used path_map instead of
      // dir_path_map to be able to search for both paths without any casting
      // (and copies). But currently we have too much stuff pointing to the
      // key.
      //
      return find (path_cast<dir_path> (p));
    }

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

    // Entities that can access bypassing the lock proof.
    //
    friend int main (int, char*[]);

    LIBBUILD2_SYMEXPORT scope&
    find (const dir_path&);

  private:
    context& ctx;
  };
}

#include <libbuild2/scope.ixx>

#endif // LIBBUILD2_SCOPE_HXX
