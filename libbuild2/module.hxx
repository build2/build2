// file      : libbuild2/module.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_MODULE_HXX
#define LIBBUILD2_MODULE_HXX

#include <map>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // A few high-level notes on the terminology: From the user's perspective,
  // the module is "loaded" (with the `using` directive). From the
  // implementation's perspectives, the module library is "loaded" and the
  // module is "bootstrapped" (or "booted" for short) and then "initialized"
  // (or "inited").

  class scope;
  class location;

  class module_base
  {
  public:
    virtual
    ~module_base () = default;
  };

  // Return true if the module should be initialized first (the order of
  // initialization within each group is unspecified).
  //
  using module_boot_function =
    bool (scope& root,
          const location&,
          unique_ptr<module_base>&);

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
          unique_ptr<module_base>&,
          bool first,                 // First time for this project.
          bool optional,              // Loaded with using? (optional module).
          const variable_map& hints); // Configuration hints (see below).

  // If the boot function is not NULL, then such a module is said to require
  // bootstrapping and must be loaded in bootstrap.build.
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
    module_init_function* init;
    unique_ptr<module_base> module;
    const location loc; // Boot location.
  };

  struct module_map: std::map<string, module_state>
  {
    template <typename T>
    T*
    lookup (const string& name) const
    {
      auto i (find (name));
      return i != end ()
        ? static_cast<T*> (i->second.module.get ())
        : nullptr;
    }
  };

  // Boot the specified module loading its library if necessary.
  //
  LIBBUILD2_SYMEXPORT void
  boot_module (scope& root, const string& name, const location&);

  // Init the specified module loading its library if necessary. Used by the
  // parser but also by some modules to init prerequisite modules. Return true
  // if the module was both successfully loaded and configured (false can only
  // be returned if optional is true).
  //
  // The config_hints variable map can be used to pass configuration hints
  // from one module to another. For example, the cxx modude may pass the
  // target platform (which was extracted from the C++ compiler) to the bin
  // module (which may not always be able to extract the same information from
  // its tools).
  //
  LIBBUILD2_SYMEXPORT bool
  init_module (scope& root,
               scope& base,
               const string& name,
               const location&,
               bool optional = false,
               const variable_map& config_hints = variable_map ());

  // An alias to use from other modules (we could also distinguish between
  // boot and init).
  //
  // @@ TODO: maybe incorporate the .loaded variable check we have all over
  //          (it's not clear if init_module() already has this semantics)?
  //
  inline bool
  load_module (scope& root,
               scope& base,
               const string& name,
               const location& loc,
               bool optional = false,
               const variable_map& config_hints = variable_map ())
  {
    return init_module (root, base, name, loc, optional, config_hints);
  }

  // Loaded modules (as in libraries).
  //
  // A NULL entry for the main module indicates that a module library was not
  // found.
  //
  using loaded_module_map = std::map<string, const module_functions*>;
  LIBBUILD2_SYMEXPORT extern loaded_module_map loaded_modules;
}

#endif // LIBBUILD2_MODULE_HXX
