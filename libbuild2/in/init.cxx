// file      : libbuild2/in/init.cxx -*- C++ -*-
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
               bool first,
               bool,
               module_init_extra&)
    {
      tracer trace ("in::base_init");
      l5 ([&]{trace << "for " << rs;});

      assert (first);

      // Enter variables.
      //
      {
        // All the variables we enter are qualified so go straight for the
        // public variable pool.
        //
        auto& vp (rs.var_pool (true /* public */));

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
        const variable& im (vp.insert<string> ("in.mode"));

        // Original name of this variable for backwards compatibility.
        //
        vp.insert_alias (im, "in.substitution");

        // Substitution map. Substitutions can be specified as key-value pairs
        // rather than buildfile variables. This map is checked before the
        // variables. An absent value in key-value has the NULL semantics.
        //
        // This mechanism has two primary uses: Firstly, it allows us to have
        // substitution names that cannot be specified as buildfile variables.
        // For example, a name may start with an underscore and thus be
        // reserved or it may refer to one of the predefined variables such a
        // `include` or `extension` that may have a wrong visibility and/or
        // type.
        //
        // Secondly, this mechanism allows us to encapsulate a group of
        // substitutions and pass this group around as a single value.
        //
        vp.insert<map<string, optional<string>>> ("in.substitutions");

        // Fallback value to use for NULL value substitutions. If unspecified,
        // NULL substitutions are an error.
        //
        vp.insert<string> ("in.null");
      }

      // Register target types.
      //
      rs.insert_target_type<in> ();

      return true;
    }

    bool
    init (scope& rs,
          scope& bs,
          const location& loc,
          bool,
          bool,
          module_init_extra&)
    {
      tracer trace ("in::init");
      l5 ([&]{trace << "for " << bs;});

      // Load in.base.
      //
      load_module (rs, rs, "in.base", loc);

      // Register rules.
      //
      // There are rules that are "derived" from this generic in rule in
      // order to provide extended preprocessing functionality (see the
      // version module for an example). To make sure they are tried first
      // we register for path_target, not file, but in rule::match() we only
      // match if the target is a file. A bit of a hack.
      //
      bs.insert_rule<path_target> (perform_update_id,   "in", rule_);
      bs.insert_rule<path_target> (perform_clean_id,    "in", rule_);
      bs.insert_rule<path_target> (configure_update_id, "in", rule_);

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
