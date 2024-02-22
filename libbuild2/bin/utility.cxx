// file      : libbuild2/bin/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bin/utility.hxx>

#include <libbuild2/scope.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx> // search()

namespace build2
{
  namespace bin
  {
    lorder
    link_order (const scope& bs, otype ot)
    {
      const char* var (nullptr);

      switch (ot)
      {
      case otype::e: var = "bin.exe.lib";  break;
      case otype::a: var = "bin.liba.lib"; break;
      case otype::s: var = "bin.libs.lib"; break;
      }

      const auto& v (cast<strings> (bs[var]));
      return v[0] == "shared"
        ? v.size () > 1 && v[1] == "static" ? lorder::s_a : lorder::s
        : v.size () > 1 && v[1] == "shared" ? lorder::a_s : lorder::a;
    }

    lmembers
    link_members (const scope& rs)
    {
      const string& type (cast<string> (rs["bin.lib"]));

      bool a (type == "static" || type == "both");
      bool s (type == "shared" || type == "both");

      if (!a && !s)
        fail << "unknown library type: " << type <<
          info << "'static', 'shared', or 'both' expected";

      return lmembers {a, s};
    }

    const file*
    link_member (const libx& x, action a, linfo li, bool exist)
    {
      const target* r;

      if (x.is_a<libul> ())
      {
        // For libul{} that is linked to an executable the member choice
        // should be dictated by the members of lib{} this libul{} is
        // "primarily" for. If both are being built, then it seems natural to
        // prefer static over shared since it could be faster (but I am sure
        // someone will probably want this configurable).
        //
        // Maybe we should use the bin.exe.lib order as a heuristics (i.e.,
        // the most likely utility library to be built is the one most likely
        // to be linked)? Will need the variables rs-only, similar to
        // bin.lib, which probably is a good thing. See also libul_rule.
        //
        if (li.type == otype::e)
        {
          // Utility libraries are project-local which means the primarily
          // target should be in the same project as us.
          //
          li.type = link_members (x.root_scope ()).a ? otype::a : otype::s;
        }

        const target_type& tt (li.type == otype::a
                               ? libua::static_type
                               : libus::static_type);

        // Called by the compile rule during execute.
        //
        r = x.ctx.phase == run_phase::match && !exist
          ? &search (x, tt, x.dir, x.out, x.name)
          : search_existing (x.ctx, tt, x.dir, x.out, x.name);
      }
      else
      {
        assert (!exist);

        const lib& l (x.as<lib> ());

        // Make sure group members are resolved.
        //
        group_view gv (resolve_members (a, l));

        if (gv.members == nullptr)
          fail << "group " << l << " has no members";

        pair<otype, bool> p (
          link_member (lmembers {l.a != nullptr, l.s != nullptr}, li.order));

        if (!p.second)
          fail << (p.first == otype::s ? "shared" : "static")
               << " variant of " << l << " is not available";

        r = p.first == otype::s ? static_cast<const target*> (l.s) : l.a;
      }

      return static_cast<const file*> (r);
    }

    pattern_paths
    lookup_pattern (const scope& rs)
    {
      pattern_paths r;

      // Theoretically, we could have both the pattern and the search paths,
      // for example, the pattern can come first followed by the paths.
      //
      if (const string* v = cast_null<string> (rs["bin.pattern"]))
      {
        (path::traits_type::is_separator (v->back ())
         ? r.paths
         : r.pattern) = v->c_str ();
      }

      return r;
    }
  }
}
