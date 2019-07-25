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

  // Loaded modules state.
  //
  struct module_state
  {
    bool boot;  // True if the module boot'ed but not yet init'ed.
    bool first; // True if the boot'ed module must be init'ed first.
    module_init_function* init;
    unique_ptr<module_base> module;
    const location loc; // Boot location.
  };

  struct loaded_module_map: std::map<string, module_state>
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

  // Load and boot the specified module.
  //
  LIBBUILD2_SYMEXPORT void
  boot_module (scope& root, const string& name, const location&);

  // Load (if not already loaded) and initialize the specified module. Used
  // by the parser but also by some modules to load prerequisite modules.
  // Return true if the module was both successfully loaded and configured
  // (false can only be returned if optional).
  //
  // The config_hints variable map can be used to pass configuration hints
  // from one module to another. For example, the cxx modude may pass the
  // target platform (which was extracted from the C++ compiler) to the bin
  // module (which may not always be able to extract the same information from
  // its tools).
  //
  LIBBUILD2_SYMEXPORT bool
  load_module (scope& root,
               scope& base,
               const string& name,
               const location&,
               bool optional = false,
               const variable_map& config_hints = variable_map ());

  // Builtin modules.
  //
  // @@ Maybe this should be renamed to loaded modules?
  // @@ We can also change it to std::map<const char*, const module_functions*>
  //
  using available_module_map = std::map<string, module_functions>;
  LIBBUILD2_SYMEXPORT extern available_module_map builtin_modules;
}

#endif // LIBBUILD2_MODULE_HXX
