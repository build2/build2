// file      : build2/bin/rule.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/bin/rule.hxx>

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/algorithm.hxx>
#include <build2/diagnostics.hxx>

#include <build2/bin/target.hxx>

using namespace std;

namespace build2
{
  namespace bin
  {
    // fail_rule
    //
    bool fail_rule::
    match (action a, target& t, const string&) const
    {
      const char* n (t.dynamic_type ().name); // Ignore derived type.

      fail << diag_doing (a, t) << " target group" <<
        info << "explicitly select " << n << "e{}, " << n << "a{}, or "
           << n << "s{} member" << endf;
    }

    recipe fail_rule::
    apply (action, target&) const {return empty_recipe;}

    // lib_rule
    //
    // The whole logic is pretty much as if we had our two group members as
    // our prerequisites.
    //
    bool lib_rule::
    match (action, target& xt, const string&) const
    {
      lib& t (xt.as<lib> ());

      // Get the library type to build. If not set for a target, this should
      // be configured at the project scope by init().
      //
      const string& type (cast<string> (t["bin.lib"]));

      bool a (type == "static" || type == "both");
      bool s (type == "shared" || type == "both");

      if (!a && !s)
        fail << "unknown library type: " << type <<
          info << "'static', 'shared', or 'both' expected";

      t.a = a ? &search<liba> (t, t.dir, t.out, t.name) : nullptr;
      t.s = s ? &search<libs> (t, t.dir, t.out, t.name) : nullptr;

      return true;
    }

    recipe lib_rule::
    apply (action a, target& xt) const
    {
      lib& t (xt.as<lib> ());

      //@@ outer: also prerequisites (if outer) or not?

      const target* m[] = {t.a, t.s};
      match_members (a, t, m);

      return &perform;
    }

    target_state lib_rule::
    perform (action a, const target& xt)
    {
      const lib& t (xt.as<lib> ());

      const target* m[] = {t.a, t.s};
      return execute_members (a, t, m);
    }
  }
}
