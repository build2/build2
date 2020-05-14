// file      : libbuild2/build/script/script.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/build/script/script.hxx>

#include <libbuild2/target.hxx>

#include <libbuild2/script/parser.hxx>

using namespace std;

namespace build2
{
  namespace build
  {
    namespace script
    {
      // script
      //
      script::
      script ()
          : primary_target_var (var_pool.insert<path> (">"))
      {
      }

      // environment
      //
      static const string wd_name ("current directory");

      environment::
      environment (const build::script::script& s, const target& pt)
          : build2::script::environment (
              pt.ctx,
              cast<target_triplet> (pt.ctx.global_scope["build.host"]),
              work,
              wd_name),
            script (s),
            vars (context, false /* global */),
            primary_target (pt)
      {
        // Set the $> variable.
        //
        {
          value& v (assign (s.primary_target_var));

          if (auto* t = pt.is_a<path_target> ())
            v = t->path ();
          else
            fail << "target " << pt << " is not path-based";
        }
      }

      void environment::
      set_variable (string&& nm, names&& val, const string& attrs)
      {
        // Set the variable value and attributes. Note that we need to aquire
        // unique lock before potentially changing the script's variable
        // pool. The obtained variable reference can safelly be used with no
        // locking as the variable pool is an associative container
        // (underneath) and we are only adding new variables into it.
        //
        ulock ul (script.var_pool_mutex);

        const variable& var (
          const_cast<build::script::script&> (script).var_pool.
          insert (move (nm)));

        ul.unlock ();

        value& lhs (assign (var));

        // If there are no attributes specified then the variable assignment
        // is straightforward. Otherwise we will use the build2 parser helper
        // function.
        //
        if (attrs.empty ())
          lhs.assign (move (val), &var);
        else
        {
          build2::script::parser p (context);
          p.apply_value_attributes (&var,
                                    lhs,
                                    value (move (val)),
                                    attrs,
                                    token_type::assign,
                                    path_name ("<attributes>"));
        }
      }

      lookup environment::
      lookup (const variable& var) const
      {
        auto p (vars.lookup (var));
        if (p.first != nullptr)
          return lookup_type (*p.first, p.second, vars);

        return lookup_in_buildfile (var.name);
      }

      lookup environment::
      lookup (const string& name) const
      {
        // Every variable that is ever set in a script has been pre-entered
        // during pre-parse or introduced with the set builtin during
        // execution. Which means that if one is not found in the script pool
        // then it can only possibly be set in the buildfile.
        //
        // Note that we need to acquire the variable pool lock. The pool can
        // be changed from multiple threads by the set builtin. The obtained
        // variable pointer can safelly be used with no locking as the
        // variable pool is an associative container (underneath) and we are
        // only adding new variables into it.
        //
        const variable* pvar (nullptr);

        slock sl (script.var_pool_mutex);
        pvar = script.var_pool.find (name);
        sl.unlock ();

        return pvar != nullptr ? lookup (*pvar) : lookup_in_buildfile (name);
      }

      lookup environment::
      lookup_in_buildfile (const string& n) const
      {
        // Switch to the corresponding buildfile variable. Note that we don't
        // want to insert a new variable into the pool (we might be running
        // in parallel). Plus, if there is no such variable, then we cannot
        // possibly find any value.
        //
        const variable* pvar (context.var_pool.find (n));

        if (pvar == nullptr)
          return lookup_type ();

        return primary_target[*pvar];
      }

      value& environment::
      append (const variable& var)
      {
        auto l (lookup (var));

        if (l.defined () && l.belongs (*this)) // Existing var.
          return vars.modify (l);

        value& r (assign (var)); // NULL.

        if (l.defined ())
          r = *l; // Copy value (and type) from the outer scope.

        return r;
      }
    }
  }
}
