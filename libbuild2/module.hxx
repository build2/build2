// file      : libbuild2/module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_MODULE_HXX
#define LIBBUILD2_MODULE_HXX

#include <map>

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // A few high-level notes on the terminology: From the user's perspective,
  // the module is "loaded" (with the `using` directive). From the
  // implementation's perspectives, the module library is "loaded" and the
  // module is optionally "bootstrapped" (or "booted" for short) and then
  // "initialized" (or "inited").

  // Base class for module instance.
  //
  class module
  {
  public:
    virtual
    ~module () = default;
  };

  // Module boot function signature.
  //
  // The module_*_extra arguments (here and in init below) are used to pass
  // additional information that is only used by some modules. It is also a
  // way for us to later pass more information without breaking source
  // compatibility.
  //
  struct module_boot_extra
  {
    shared_ptr<build2::module> module; // Module instance (out).

    // Convenience functions.
    //
    template <typename T>
    T& set_module (T* p) {assert (!module); module.reset (p); return *p;}

    template <typename T>
    T& module_as () {assert (module); return static_cast<T&> (*module);}
  };

  // Return true if the module should be initialized first (within the
  // resulting two groups the modules are initializated in the order loaded).
  //
  using module_boot_function =
    bool (scope& root,
          const location&,
          module_boot_extra&);

  // Module init function signature.
  //
  struct module_init_extra
  {
    shared_ptr<build2::module> module; // Module instance (in/out).
    const variable_map&        hints;  // Configuration hints (see below).

    // Convenience functions.
    //
    template <typename T>
    T& set_module (T* p) {assert (!module); module.reset (p); return *p;}

    template <typename T>
    T& module_as () {assert (module); return static_cast<T&> (*module);}
  };

  // Return false if the module configuration (normally based on the default
  // values) was unsuccessful but this is not (yet) an error. One example
  // would be the optional use of a module. Or a module might remain
  // unconfigured for as long as it is actually not used (e.g., install,
  // dist). The return value is used to set the <module>.configured variable.
  //
  using module_init_function =
    bool (scope& root,
          scope& base,
          const location&,
          bool first,                 // First time for this project.
          bool optional,              // Loaded with using? (optional module).
          module_init_extra&);

  // If the boot function is not NULL, then such a module is said to require
  // bootstrapping and must be loaded in bootstrap.build. Such a module cannot
  // be optional.
  //
  struct module_functions
  {
    const char*           name; // Module/submodule name.
    module_boot_function* boot;
    module_init_function* init;
  };

  // The build2_<name>_load() function will be written in C++ and will be
  // called from C++ but we need to suppress name mangling to be able to use
  // dlsym() or equivalent, thus extern "C".
  //
  // The <name> part in the function name is the main module name without
  // submodule components (for example, `c` in `c.config`) and the load
  // function is expected to return boot/init functions for all its submodules
  // (if any) as well as for the module itself as an array of module_functions
  // terminated with an all-NULL entry.
  //
  // Note that the load function is guaranteed to be called during serial
  // execution (either from main() or during the load phase).
  //
  extern "C"
  using module_load_function = const module_functions* ();

  // Module state.
  //
  struct module_state
  {
    bool boot;  // True if the module boot'ed but not yet init'ed.
    bool first; // True if the boot'ed module must be init'ed first.
    const string name;
    module_init_function* init;
    shared_ptr<build2::module> module;
    location_value loc; // Boot location.
  };

  struct module_map: vector<module_state>
  {
    iterator
    find (const string& name)
    {
      return find_if (
        begin (), end (),
        [&name] (const module_state& s) {return s.name == name;});
    }

    const_iterator
    find (const string& name) const
    {
      return find_if (
        begin (), end (),
        [&name] (const module_state& s) {return s.name == name;});
    }

    template <typename T>
    T*
    find_module (const string& name) const
    {
      auto i (find (name));
      return i != end ()
        ? static_cast<T*> (i->module.get ())
        : nullptr;
    }
  };

  // Boot the specified module loading its library if necessary.
  //
  LIBBUILD2_SYMEXPORT void
  boot_module (scope& root, const string& name, const location&);

  // Init the specified module loading its library if necessary. Used by the
  // parser but also by some modules to init prerequisite modules. Return a
  // pointer to the corresponding module state if the module was both
  // successfully loaded and configured and NULL otherwise (which can only
  // happen if optional is true). Note that the result can be used as a bool
  // but should not be assumed stable (vector resize).
  //
  // The config_hints variable map can be used to pass configuration hints
  // from one module to another. For example, the cxx modude may pass the
  // target platform (which was extracted from the C++ compiler) to the bin
  // module (which may not always be able to extract the same information from
  // its tools).
  //
  LIBBUILD2_SYMEXPORT module_state*
  init_module (scope& root,
               scope& base,
               const string& name,
               const location&,
               bool optional = false,
               const variable_map& config_hints = empty_variable_map);

  // A wrapper over init_module() to use from other modules that incorporates
  // the <name>.loaded variable check (use init_module() directly to sidestep
  // this check). Return an optional pointer to the module instance that is
  // present if the module was both successfully loaded and configured and
  // absent otherwise (so can be used as bool).
  //
  // Note also that absent can be returned even of optional is false which
  // happens if the requested module has already been loaded but failed to
  // configure. The function could have issued diagnostics but the caller can
  // normally provide additional information.
  //
  // Note: for non-optional modules use the versions below.
  //
  LIBBUILD2_SYMEXPORT optional<shared_ptr<module>>
  load_module (scope& root,
               scope& base,
               const string& name,
               const location&,
               bool optional,
               const variable_map& config_hints = empty_variable_map);

  // As above but always load and return a pointer to the module instance.
  //
  LIBBUILD2_SYMEXPORT shared_ptr<module>
  load_module (scope& root,
               scope& base,
               const string& name,
               const location&,
               const variable_map& config_hints = empty_variable_map);

  template <typename T>
  inline T&
  load_module (scope& root,
               scope& base,
               const string& name,
               const location& l,
               const variable_map& config_hints = empty_variable_map)
  {
    return static_cast<T&> (*load_module (root, base, name, l, config_hints));
  }

  // Loaded modules (as in libraries).
  //
  // A NULL entry for the main module indicates that a module library was not
  // found.
  //
  using loaded_module_map = std::map<string, const module_functions*>;

  // The loaded_modules map is locked per top-level (as opposed to nested)
  // context (see context.hxx for details).
  //
  // Note: should only be constructed during contexts-wide serial execution.
  //
  class LIBBUILD2_SYMEXPORT loaded_modules_lock
  {
  public:
    explicit
    loaded_modules_lock (context& c)
      : ctx_ (c), lock_ (mutex_, defer_lock)
    {
      if (ctx_.modules_lock == nullptr)
      {
        lock_.lock ();
        ctx_.modules_lock = this;
      }
    }

    ~loaded_modules_lock ()
    {
      if (ctx_.modules_lock == this)
        ctx_.modules_lock = nullptr;
    }

  private:
    static mutex mutex_;
    context& ctx_;
    mlock lock_;
  };

  LIBBUILD2_SYMEXPORT extern loaded_module_map loaded_modules;

  // Load a builtin module (i.e., a module linked as a static/shared library
  // or that is part of the build system driver).
  //
  // Note: assumes serial execution.
  //
  LIBBUILD2_SYMEXPORT void
  load_builtin_module (module_load_function*);
}

#endif // LIBBUILD2_MODULE_HXX
