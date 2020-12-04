// file      : libbuild2/bin/functions.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

#include <libbuild2/bin/utility.hxx>

namespace build2
{
  namespace bin
  {
    void
    functions (function_map& m)
    {
      function_family f (m, "bin");

      // Given a linker output target type ("exe", "lib[as]", or "libu[eas]")
      // and a lib{} target group, return the type of library member ("liba"
      // or "libs") that will be picked when linking this library group to
      // this target type.
      //
      // The lib{} target is only used to resolve the scope to lookup the
      // bin.lib value on. As a result, it can be omitted in which case the
      // function call scope is used (covers project-local lib{} targets).
      //
      // Note that this function is not pure.
      //
      // @@ TODO: support for target (note that if it's out of project, then
      //          it's imported, which means it might still be qualified.)
      //
      // @@ TODO: support utility libraries (see link_member()).
      //
      f.insert (".link_member", false) += [] (const scope* bs, names ns)
      {
        string t (convert<string> (move (ns)));

        if (bs == nullptr)
          fail << "bin.link_member() called out of scope";

        const scope* rs (bs->root_scope ());

        if (rs == nullptr)
          fail << "bin.link_member() called out of root scope";

        const target_type* tt (bs->find_target_type (t));

        if (tt == nullptr)
          fail << "unknown target type '" << t << "'";

        otype ot (link_type (*tt).type);

        switch (ot)
        {
        case otype::e:
        case otype::a:
        case otype::s:
          break;
        default:
          fail << "target type " << t << " is not linker output";
        }

        lorder lo (link_order (*bs, ot));
        lmembers lm (link_members (*rs));

        return link_member (lm, lo).first == otype::s ? "libs" : "liba";
      };
    }
  }
}
