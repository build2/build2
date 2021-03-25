// file      : libbuild2/config/module.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CONFIG_MODULE_HXX
#define LIBBUILD2_CONFIG_MODULE_HXX

#include <libbutl/prefix-map.mxx>

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/module.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/config/utility.hxx>

namespace build2
{
  namespace config
  {
    // An ordered list of build system modules each with an ordered list of
    // config.* variables and their "save flags" (see save_variable()) that
    // are used (as opposed to just being specified) in this configuration.
    // Populated by the config utility functions (required(), optional()) and
    // saved in the order populated. If flags are absent, then this variable
    // was marked as "unsaved" (always transient).
    //
    // The optional save function can be used to implement custom variable
    // saving, for example, as a difference appended to the base value. The
    // second half of the result is the assignment operator to use.
    //
    using save_variable_function =
      pair<names_view, const char*> (const value&,
                                     const value* base,
                                     names& storage);
    struct saved_variable
    {
      reference_wrapper<const variable> var;
      optional<uint64_t> flags;
      save_variable_function* save;
    };

    struct saved_variables: vector<saved_variable>
    {
      // Normally each module only have a handful of config variables and we
      // only do this during configuration so for now we do linear search
      // instead of adding a map.
      //
      const_iterator
      find (const variable& var) const
      {
        return std::find_if (
          begin (),
          end (),
          [&var] (const saved_variable& v) {return var == v.var;});
      }
    };

    struct saved_modules: butl::prefix_map<string, saved_variables, '.'>
    {
      // Priority order with INT32_MIN being the highest. Modules with the
      // same priority are saved in the order inserted.
      //
      multimap<std::int32_t, const_iterator> order;

      pair<iterator, bool>
      insert (string name, int prio = 0)
      {
        auto p (emplace (move (name), saved_variables ()));

        if (p.second)
          order.emplace (prio, p.first);

        return p;
      }
    };

    struct module: build2::module
    {
      config::saved_modules saved_modules;

      // Return true if variable/module were newly inserted.
      //
      bool
      save_variable (const variable&,
                     optional<uint64_t> flags,
                     save_variable_function* = nullptr);

      static void
      save_variable (scope&, const variable&, optional<uint64_t>);

      bool
      save_module (const char* name, int prio = 0);

      static void
      save_module (scope&, const char*, int);

      const saved_variable*
      find_variable (const variable& var)
      {
        auto i (saved_modules.find_sup (var.name));
        if (i != saved_modules.end ())
        {
          auto j (i->second.find (var));
          if (j != i->second.end ())
            return &*j;
        }

        return nullptr;
      }

      // Configure/disfigure hooks.
      //
      static bool
      configure_post (scope&, configure_post_hook*);

      static bool
      disfigure_pre (scope&, disfigure_pre_hook*);

      small_vector<configure_post_hook*, 1> configure_post_;
      small_vector<disfigure_pre_hook*,  1> disfigure_pre_;

      // Cached (during init) config.config.persist value, if defined.
      //
      const vector<pair<string, string>>* persist = nullptr;

      static const string name;
      static const uint64_t version;
    };

    // Implementation-specific utilities.
    //

    inline path
    config_file (const scope& rs)
    {
      return (rs.out_path () /
              rs.root_extra->build_dir /
              "config." + rs.root_extra->build_ext);
    }
  }
}

#endif // LIBBUILD2_CONFIG_MODULE_HXX
