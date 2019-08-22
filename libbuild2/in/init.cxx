// file      : libbuild2/in/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/in/init.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/in/rule.hxx>
#include <libbuild2/in/target.hxx>

using namespace std;

namespace build2
{
  namespace in
  {
    static const rule rule_ ("in", "in");

    bool
    base_init (scope& rs,
               scope&,
               const location&,
               unique_ptr<module_base>&,
               bool first,
               bool,
               const variable_map&)
    {
      tracer trace ("in::base_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      // Enter variables.
      //
      {
        auto& vp (rs.ctx.var_pool.rw (rs));

        // Alternative variable substitution symbol with '$' being the
        // default.
        //
        vp.insert<string> ("in.symbol");

        // Substitution mode. Valid values are 'strict' (default) and 'lax'.
        // In the strict mode every substitution symbol is expected to start a
        // substitution with the double symbol (e.g., $$) serving as an escape
        // sequence.
        //
        // In the lax mode a pair of substitution symbols is only treated as a
        // substitution if what's between them looks like a build2 variable
        // name (i.e., doesn't contain spaces, etc). Everything else,
        // including unterminated substitution symbols, is copied as is. Note
        // also that in this mode the double symbol is not treated as an
        // escape sequence.
        //
        // The lax mode is mostly useful when trying to reuse existing .in
        // files, for example, from autoconf. Note, however, that the lax mode
        // is still stricter than the autoconf's semantics which also leaves
        // unknown substitutions as is.
        //
        vp.insert<string> ("in.substitution");
      }

      // Register target types.
      //
      rs.target_types.insert<in> ();

      return true;
    }

    bool
    init (scope& rs,
          scope& bs,
          const location& loc,
          unique_ptr<module_base>&,
          bool,
          bool,
          const variable_map&)
    {
      tracer trace ("in::init");
      l5 ([&]{trace << "for " << bs;});

      // Load in.base.
      //
      if (!cast_false<bool> (rs["in.base.loaded"]))
        load_module (rs, rs, "in.base", loc);

      // Register rules.
      //
      {
        auto& r (bs.rules);

        // There are rules that are "derived" from this generic in rule in
        // order to provide extended preprocessing functionality (see the
        // version module for an example). To make sure they are tried first
        // we register for path_target, not file, but in rule::match() we only
        // match if the target is a file. A bit of a hack.
        //
        r.insert<path_target> (perform_update_id,   "in", rule_);
        r.insert<path_target> (perform_clean_id,    "in", rule_);
        r.insert<path_target> (configure_update_id, "in", rule_);
      }

      return true;
    }

    static const module_functions mod_functions[] =
    {
      // NOTE: don't forget to also update the documentation in init.hxx if
      //       changing anything here.

      {"in.base", nullptr, base_init},
      {"in",      nullptr, init},
      {nullptr,   nullptr, nullptr}
    };

    const module_functions*
    build2_in_load ()
    {
      return mod_functions;
    }
  }
}
