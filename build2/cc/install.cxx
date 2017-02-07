// file      : build2/cc/install.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/install>

#include <build2/algorithm>

#include <build2/bin/target>

#include <build2/cc/link>    // match()
#include <build2/cc/utility>

using namespace std;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    install::
    install (data&& d, const link& l): common (move (d)), link_ (l) {}

    target* install::
    filter (slock& ml, action a, target& t, prerequisite_member p) const
    {
      if (t.is_a<exe> ())
      {
        // Don't install executable's prerequisite headers.
        //
        if (x_header (p))
          return nullptr;
      }

      // If this is a shared library prerequisite, install it as long as it
      // is in the same amalgamation as we are.
      //
      // @@ Shouldn't we also install a static library prerequisite of a
      //    static library?
      //
      if ((t.is_a<exe> () || t.is_a<libs> ()) &&
          (p.is_a<lib> () || p.is_a<libs> ()))
      {
        target* pt (&p.search ());

        // If this is the lib{} group, pick a member which we would link.
        //
        if (lib* l = pt->is_a<lib> ())
          pt = &link_member (*l, link_order (t.base_scope (), link_type (t)));

        if (pt->is_a<libs> ()) // Can be liba{}.
          return pt->in (t.weak_scope ()) ? pt : nullptr;
      }

      return file_rule::filter (ml, a, t, p);
    }

    match_result install::
    match (slock& ml, action a, target& t, const string& hint) const
    {
      // @@ How do we split the hint between the two?
      //

      // We only want to handle installation if we are also the
      // ones building this target. So first run link's match().
      //
      match_result r (link_.match (ml, a, t, hint));
      return r ? file_rule::match (ml, a, t, "") : r;
    }

    recipe install::
    apply (slock& s, action a, target& t) const
    {
      recipe r (file_rule::apply (s, a, t));

      // Derive shared library paths and cache them in the target's aux
      // storage if we are (un)installing (used in *_extra() functions below).
      //
      if (a.operation () == install_id || a.operation () == uninstall_id)
      {
        file* f;
        if ((f = t.is_a<libs> ()) != nullptr && tclass != "windows")
          t.data (link_.derive_libs_paths (*f));
      }

      return r;
    }

    void install::
    install_extra (const file& t, const install_dir& id) const
    {
      if (t.is_a<libs> () && tclass != "windows")
      {
        // Here we may have a bunch of symlinks that we need to install.
        //
        auto& lp (t.data<link::libs_paths> ());

        auto ln = [&id, this] (const path& f, const path& l)
        {
          install_l (id, f.leaf (), l.leaf (), false);
        };

        const path& lk (lp.link);
        const path& so (lp.soname);
        const path& in (lp.interm);

        const path* f (&lp.real);

        if (!in.empty ()) {ln (*f, in); f = &in;}
        if (!so.empty ()) {ln (*f, so); f = &so;}
        if (!lk.empty ()) {ln (*f, lk);}
      }
    }

    bool install::
    uninstall_extra (const file& t, const install_dir& id) const
    {
      bool r (false);

      if (t.is_a<libs> () && tclass != "windows")
      {
        // Here we may have a bunch of symlinks that we need to uninstall.
        //
        auto& lp (t.data<link::libs_paths> ());

        auto rm = [&id, this] (const path& l)
        {
          return uninstall_f (id, nullptr, l.leaf (), false);
        };

        const path& lk (lp.link);
        const path& so (lp.soname);
        const path& in (lp.interm);

        if (!lk.empty ()) r = rm (lk) || r;
        if (!so.empty ()) r = rm (so) || r;
        if (!in.empty ()) r = rm (in) || r;
      }

      return r;
    }
  }
}
