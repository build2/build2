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
      // environment
      //
      static const optional<string> wd_name ("current directory");

      environment::
      environment (action a, const target_type& t)
          : build2::script::environment (
              t.ctx,
              cast<target_triplet> (t.ctx.global_scope["build.host"]),
              dir_name_view (&work, &wd_name),
              temp_dir.path, false /* temp_dir_keep */,
              redirect (redirect_type::none),
              redirect (redirect_type::merge, 2),
              redirect (redirect_type::pass)),
            target (t),
            vars (context, false /* global */)
      {
        // Set special variables.
        //
        {
          // $>
          //
          names ns;
          for (const target_type* m (&t); m != nullptr; m = m->adhoc_member)
            m->as_name (ns);

          assign (var_pool.insert (">")) = move (ns);
        }

        {
          // $<
          //
          // Note that at this stage (after execute_prerequisites()) ad hoc
          // prerequisites are no longer in prerequisite_targets which means
          // they won't end up in $< either. While at first thought ad hoc
          // prerequisites in ad hoc recipes don't seem to make much sense,
          // they could be handy to exclude certain preresquisites from $<
          // while still treating them as such.
          //
          names ns;
          for (const target_type* pt: t.prerequisite_targets[a])
            if (pt != nullptr)
              pt->as_name (ns);

          assign (var_pool.insert ("<")) = move (ns);
        }
      }

      void environment::
      set_variable (string&& nm, names&& val, const string& attrs)
      {
        // Set the variable value and attributes.
        //
        const variable& var (var_pool.insert (move (nm)));

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
        // Every variable that is ever set in a script has been added during
        // variable line execution or introduced with the set builtin. Which
        // means that if one is not found in the environment pool then it can
        // only possibly be set in the buildfile.
        //
        const variable* pvar (var_pool.find (name));
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

        return target[*pvar];
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
