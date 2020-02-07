// file      : libbuild2/cc/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/utility.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx> // search()

#include <libbuild2/bin/rule.hxx>
#include <libbuild2/bin/target.hxx>

using namespace std;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    const dir_path module_dir ("cc");
    const dir_path modules_sidebuild_dir (dir_path (module_dir) /= "modules");

    lorder
    link_order (const scope& bs, otype ot)
    {
      // Initialize to suppress 'may be used uninitialized' warning produced
      // by MinGW GCC 5.4.0.
      //
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

    const target*
    link_member (const bin::libx& x, action a, linfo li, bool exist)
    {
      if (x.is_a<libul> ())
      {
        // For libul{} that is linked to an executable the member choice
        // should be dictated by the members of lib{} this libul{} is
        // "primarily" for. If both are being built, then it seems natural to
        // prefer static over shared since it could be faster (but I am sure
        // someone will probably want this configurable).
        //
        if (li.type == otype::e)
        {
          // Utility libraries are project-local which means the primarily
          // target should be in the same project as us.
          //
          li.type = lib_rule::build_members (x.root_scope ()).a
            ? otype::a
            : otype::s;
        }

        const target_type& tt (li.type == otype::a
                               ? libua::static_type
                               : libus::static_type);

        // Called by the compile rule during execute.
        //
        return x.ctx.phase == run_phase::match && !exist
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
        assert (gv.members != nullptr);

        lorder lo (li.order);

        bool ls (true);
        switch (lo)
        {
        case lorder::a:
        case lorder::a_s:
          ls = false; // Fall through.
        case lorder::s:
        case lorder::s_a:
          {
            if (ls ? l.s == nullptr : l.a == nullptr)
            {
              if (lo == lorder::a_s || lo == lorder::s_a)
                ls = !ls;
              else
                fail << (ls ? "shared" : "static") << " variant of " << l
                     << " is not available";
            }
          }
        }

        return ls ? static_cast<const target*> (l.s) : l.a;
      }
    }
  }
}
