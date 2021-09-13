// file      : libbuild2/install/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/install/utility.hxx>

namespace build2
{
  namespace install
  {
    const scope*
    install_scope (const target& t)
    {
      context& ctx (t.ctx);

      const variable& var (*ctx.var_pool.find ("config.install.scope"));

      if (const string* s = cast_null<string> (ctx.global_scope[var]))
      {
        if (*s == "project")
          return &t.root_scope ();
        else if (*s == "strong")
          return &t.strong_scope ();
        else if (*s == "weak")
          return &t.weak_scope ();
        else if (*s != "global")
          fail << "invalid " << var << " value '" << *s << "'";
      }

      return nullptr;
    }
  }
}
