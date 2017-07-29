// file      : build2/cc/install.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/install.hxx>

#include <build2/algorithm.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/link.hxx>    // match()
#include <build2/cc/utility.hxx>

using namespace std;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // file_install
    //
    file_install::
    file_install (data&& d, const link& l): common (move (d)), link_ (l) {}

    const target* file_install::
    filter (action a, const target& t, prerequisite_member p) const
    {
      // NOTE: see also alias_install::filter() below if changing anything
      // here.

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
      if ((t.is_a<exe> ()  || t.is_a<libs> ()) &&
          (p.is_a<libx> () || p.is_a<libs> ()))
      {
        const target* pt (&p.search (t));

        // If this is the lib{}/libu{} group, pick a member which we would
        // link. For libu{} we want to the "see through" logic.
        //
        if (const libx* l = pt->is_a<libx> ())
          pt = &link_member (
            *l, a, link_info (t.base_scope (), link_type (t).type));

        if (pt->is_a<libs> ()) // Can be liba{}.
          return pt->in (t.weak_scope ()) ? pt : nullptr;

        // See through libux{}. Note that we are always in the same project
        // (and thus amalgamation).
        //
        if (pt->is_a<libux> ())
          return pt;
      }

      return file_rule::filter (a, t, p);
    }

    match_result file_install::
    match (action a, target& t, const string& hint) const
    {
      // @@ How do we split the hint between the two?
      //

      // We only want to handle installation if we are also the ones building
      // this target. So first run link's match().
      //
      match_result r (link_.match (a, t, hint));
      return r ? file_rule::match (a, t, "") : r;
    }

    recipe file_install::
    apply (action a, target& t) const
    {
      recipe r (file_rule::apply (a, t));

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

    void file_install::
    install_extra (const file& t, const install_dir& id) const
    {
      if (t.is_a<libs> () && tclass != "windows")
      {
        // Here we may have a bunch of symlinks that we need to install.
        //
        auto& lp (t.data<link::libs_paths> ());

        auto ln = [&id] (const path& f, const path& l)
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

    bool file_install::
    uninstall_extra (const file& t, const install_dir& id) const
    {
      bool r (false);

      if (t.is_a<libs> () && tclass != "windows")
      {
        // Here we may have a bunch of symlinks that we need to uninstall.
        //
        auto& lp (t.data<link::libs_paths> ());

        auto rm = [&id] (const path& l)
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

    // alias_install
    //
    alias_install::
    alias_install (data&& d, const link& l): common (move (d)), link_ (l) {}

    const target* alias_install::
    filter (action a, const target& t, prerequisite_member p) const
    {
      // The "see through" semantics that should be parallel to file_install
      // above. In particular, here we use libue/libua/libus{} as proxies for
      // exe/liba/libs{} there.
      //
      if (t.is_a<libue> ())
      {
        if (x_header (p))
          return nullptr;
      }

      if ((t.is_a<libue> ()  || t.is_a<libus> ()) &&
          (p.is_a<libx> ()   || p.is_a<libs> ()))
      {
        const target* pt (&p.search (t));

        if (const libx* l = pt->is_a<libx> ())
          pt = &link_member (
            *l, a, link_info (t.base_scope (), link_type (t).type));

        if (pt->is_a<libs> ())
          return pt->in (t.weak_scope ()) ? pt : nullptr;

        if (pt->is_a<libux> ())
          return pt;
      }

      return alias_rule::filter (a, t, p);
    }

    match_result alias_install::
    match (action a, target& t, const string& hint) const
    {
      // We only want to handle installation if we are also the ones building
      // this target. So first run link's match().
      //
      match_result r (link_.match (a, t, hint));
      return r ? alias_rule::match (a, t, "") : r;
    }
  }
}
