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

      otype ot (link_type (t).type);

      // Don't install executable's prerequisite headers.
      //
      if (t.is_a<exe> () && x_header (p))
        return nullptr;

      // Here is a problem: if the user spells the obj*/bmi*{} targets
      // explicitly, then the source files, including headers/modules may be
      // specified as preprequisites of those targets and not of this target.
      // While this can be worked around for headers by also listing them as
      // prerequisites of this target, this won't work for modules (since they
      // are compiled). So what we are going to do here is detect bmi*{} and
      // translate them to their mxx{} (this doesn't quite work for headers
      // since there would normally be several of them).
      //
      if (p.is_a<bmi> () || p.is_a (compile_types (ot).bmi))
      {
        const target& mt (p.search (t));

        for (prerequisite_member mp: group_prerequisite_members (a, mt))
        {
          if (mp.is_a (*x_mod))
            return t.is_a<exe> () ? nullptr : file_rule::filter (a, mt, mp);
        }
      }

      // If this is a shared library prerequisite, install it as long as it
      // is in the same amalgamation as we are.
      //
      // Less obvious: we also want to install a static library prerequisite
      // of a library (since it could be referenced from its .pc file, etc).
      //
      bool st (t.is_a<exe>  () || t.is_a<libs> ()); // Target needs shared.
      bool at (t.is_a<liba> () || t.is_a<libs> ()); // Target needs static.

      if ((st && (p.is_a<libx> () || p.is_a<libs> ())) ||
          (at && (p.is_a<libx> () || p.is_a<liba> ())))
      {
        const target* pt (&p.search (t));

        // If this is the lib{}/libu{} group, pick a member which we would
        // link. For libu{} we want to the "see through" logic.
        //
        if (const libx* l = pt->is_a<libx> ())
          pt = &link_member (*l, a, link_info (t.base_scope (), ot));

        if ((st && pt->is_a<libs> ()) || (at && pt->is_a<liba> ()))
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
        {
          const string* p (cast_null<string> (t["bin.lib.prefix"]));
          const string* s (cast_null<string> (t["bin.lib.suffix"]));
          t.data (
            link_.derive_libs_paths (*f,
                                     p != nullptr ? p->c_str (): nullptr,
                                     s != nullptr ? s->c_str (): nullptr));
        }
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
        const scope& rs (t.root_scope ());
        auto& lp (t.data<link::libs_paths> ());

        auto ln = [&rs, &id] (const path& f, const path& l)
        {
          install_l (rs, id, f.leaf (), l.leaf (), false);
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
        const scope& rs (t.root_scope ());
        auto& lp (t.data<link::libs_paths> ());

        auto rm = [&rs, &id] (const path& l)
        {
          return uninstall_f (rs, id, nullptr, l.leaf (), false);
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

      otype ot (link_type (t).type);

      if (t.is_a<libue> () && x_header (p))
        return nullptr;

      if (p.is_a<bmi> () || p.is_a (compile_types (ot).bmi))
      {
        const target& mt (p.search (t));

        for (prerequisite_member mp: group_prerequisite_members (a, mt))
        {
          if (mp.is_a (*x_mod))
            return t.is_a<libue> () ? nullptr : alias_rule::filter (a, mt, mp);
        }
      }

      bool st (t.is_a<libue> () || t.is_a<libus> ()); // Target needs shared.
      bool at (t.is_a<libua> () || t.is_a<libus> ()); // Target needs static.

      if ((st && (p.is_a<libx> () || p.is_a<libs> ())) ||
          (at && (p.is_a<libx> () || p.is_a<liba> ())))
      {
        const target* pt (&p.search (t));

        if (const libx* l = pt->is_a<libx> ())
          pt = &link_member (*l, a, link_info (t.base_scope (), ot));

        if ((st && pt->is_a<libs> ()) || (at && pt->is_a<liba> ()))
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
