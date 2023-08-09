// file      : libbuild2/functions-target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/functions-name.hxx> // to_target()

#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  target_functions (function_map& m)
  {
    // Functions that can be called only on real targets.
    //
    function_family f (m, "target");

    // $path(<names>)
    //
    // Return the path of a target (or a list of paths for a list of
    // targets). The path must be assigned, which normally happens during
    // match. As a result, this function is normally called form a recipe.
    //
    // Note that while this function is technically not pure, we don't mark it
    // as such since it can only be called (normally form a recipe) after the
    // target has been matched, meaning that this target is a prerequisite and
    // therefore this impurity has been accounted for.
    //
    f["path"] += [](const scope* s, names ns)
    {
      if (s == nullptr)
        fail << "target.path() called out of scope";

      // Most of the time we will have a single target so optimize for that.
      //
      small_vector<path, 1> r;

      for (auto i (ns.begin ()); i != ns.end (); ++i)
      {
        name& n (*i), o;
        const target& t (to_target (*s, move (n), move (n.pair ? *++i : o)));

        if (const auto* pt = t.is_a<path_target> ())
        {
          const path& p (pt->path ());

          if (&p != &empty_path)
            r.push_back (p);
          else
            fail << "target " << t << " path is not assigned";
        }
        else
          fail << "target " << t << " is not path-based";
      }

      // We want the result to be path if we were given a single target and
      // paths if multiple (or zero). The problem is, we cannot distinguish it
      // based on the argument type (e.g., name vs names) since passing an
      // out-qualified single target requires two names.
      //
      if (r.size () == 1)
        return value (move (r[0]));

      return value (paths (make_move_iterator (r.begin ()),
                           make_move_iterator (r.end ())));
    };

    // $process_path(<name>)
    //
    // Return the process path of an executable target.
    //
    // Note that while this function is not technically pure, we don't mark it
    // as such for the same reasons as for `$path()` above.
    //

    // This one can only be called on a single target since we don't support
    // containers of process_path's (though we probably could).
    //
    f["process_path"] += [](const scope* s, names ns)
    {
      if (s == nullptr)
        fail << "target.process_path() called out of scope";

      if (ns.empty () || ns.size () != (ns[0].pair ? 2 : 1))
        fail << "target.process_path() expects single target";

      name o;
      const target& t (
        to_target (*s, move (ns[0]), move (ns[0].pair ? ns[1] : o)));

      if (const auto* et = t.is_a<exe> ())
      {
        process_path r (et->process_path ());

        if (r.empty ())
          fail << "target " << t << " path is not assigned";

        return r;
      }
      else
        fail << "target " << t << " is not executable-based" << endf;
    };
  }
}
