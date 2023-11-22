// file      : libbuild2/cc/install-rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/install-rule.hxx>

#include <libbuild2/algorithm.hxx>

#include <libbuild2/bin/target.hxx>

#include <libbuild2/cc/utility.hxx>
#include <libbuild2/cc/link-rule.hxx> // match()

using namespace std;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    using posthoc_prerequisite_target =
      context::posthoc_target::prerequisite_target;

    // install_rule
    //
    install_rule::
    install_rule (data&& d, const link_rule& l)
        : common (move (d)), link_ (l) {}

    // Wrap the file_rule's recipe into a data-carrying recipe.
    //
    struct install_match_data
    {
      build2::recipe        recipe;
      uint64_t              options; // Match options.
      link_rule::libs_paths libs_paths;

      target_state
      operator() (action a, const target& t)
      {
        return recipe (a, t);
      }
    };

    bool install_rule::
    filter (action a, const target& t, const target& m) const
    {
      if (!t.is_a<exe> ())
      {
        // If runtime-only, filter out all known buildtime target types.
        //
        const auto& md (t.data<install_match_data> (a));

        if ((md.options & lib::option_install_buildtime) == 0)
        {
          if (m.is_a<liba> () || // Staic library.
              m.is_a<pc> ()   || // pkg-config file.
              m.is_a<libi> ())   // Import library.
            return false;
        }
      }

      return true;
    }

    pair<const target*, uint64_t> install_rule::
    filter (const scope* is,
            action a, const target& t, prerequisite_iterator& i,
            match_extra& me) const
    {
      // NOTE: see libux_install_rule::filter() if changing anything here.

      const prerequisite& p (i->prerequisite);

      uint64_t options (match_extra::all_options);

      otype ot (link_type (t).type);

      // @@ TMP: drop eventually.
      //
#if 0
      // If this is a shared library prerequisite, install it as long as it is
      // in the installation scope.
      //
      // Less obvious: we also want to install a static library prerequisite
      // of a library (since it could be referenced from its .pc file, etc).
      //
      // Note: for now we assume these prerequisites never come from see-
      // through groups.
      //
      // Note: we install ad hoc prerequisites by default.
      //

      // Note: at least one must be true since we only register this rule for
      // exe{}, and lib[as]{} (this makes sure the following if-condition will
      // always be true for libx{}).
      //
      bool st (t.is_a<exe>  () || t.is_a<libs> ()); // Target needs shared.
      bool at (t.is_a<liba> () || t.is_a<libs> ()); // Target needs static.
      assert (st || at);

      if ((st && (p.is_a<libx> () || p.is_a<libs> ())) ||
          (at && (p.is_a<libx> () || p.is_a<liba> ())))
      {
        const target* pt (&search (t, p));

        // If this is the lib{}/libu*{} group, pick a member which we would
        // link. For libu*{} we want the "see through" logic.
        //
        if (const libx* l = pt->is_a<libx> ())
          pt = link_member (*l, a, link_info (t.base_scope (), ot));

        // Note: not redundant since we could be returning a member.
        //
        if ((st && pt->is_a<libs> ()) || (at && pt->is_a<liba> ()))
        {
          // Adjust match options.
          //
          if (a.operation () != update_id)
          {
            if (t.is_a<exe> ())
              options = lib::option_install_runtime;
            else
            {
              // This is a library prerequisite of a library target and
              // runtime-only begets runtime-only.
              //
              if (me.cur_options == lib::option_install_runtime)
                options = lib::option_install_runtime;
            }
          }

          return make_pair (is == nullptr || pt->in (*is) ? pt : nullptr,
                            options);
        }

        // See through to libu*{} members. Note that we are always in the same
        // project (and thus amalgamation).
        //
        if (pt->is_a<libux> ())
        {
          // Adjust match options (similar to above).
          //
          if (a.operation () != update_id && !pt->is_a<libue> ())
          {
            if (t.is_a<exe> ())
              options = lib::option_install_runtime;
            else
            {
              if (me.cur_options == lib::option_install_runtime)
                options = lib::option_install_runtime;
            }
          }

          return make_pair (pt, options);
        }
      }
#else
      // Note that at first it may seem like we don't need to install static
      // library prerequisites of executables. But such libraries may still
      // have prerequisites that are needed at runtime (say, some data files).
      // So we install all libraries as long as they are in the installation
      // scope and deal with runtime vs buildtime distiction using match
      // options.
      //
      // Note: for now we assume these prerequisites never come from see-
      // through groups.
      //
      // Note: we install ad hoc prerequisites by default.
      //
      if (p.is_a<libx> () || p.is_a<libs> () || p.is_a<liba> ())
      {
        const target* pt (&search (t, p));

        // If this is the lib{}/libu*{} group, pick a member which we would
        // link. For libu*{} we want the "see through" logic.
        //
        if (const libx* l = pt->is_a<libx> ())
          pt = link_member (*l, a, link_info (t.base_scope (), ot));

        // Adjust match options.
        //
        if (a.operation () != update_id)
        {
          if (t.is_a<exe> ())
            options = lib::option_install_runtime;
          else
          {
            // This is a library prerequisite of a library target and
            // runtime-only begets runtime-only.
            //
            if (me.cur_options == lib::option_install_runtime)
              options = lib::option_install_runtime;
          }
        }

        // Note: not redundant since we could be returning a member.
        //
        if (pt->is_a<libs> () || pt->is_a<liba> ())
        {
          return make_pair (is == nullptr || pt->in (*is) ? pt : nullptr,
                            options);
        }
        else // libua{} or libus{}
        {
          // See through to libu*{} members. Note that we are always in the
          // same project (and thus amalgamation).
          //
          return make_pair (pt, options);
        }
      }
#endif

      // The rest of the tests only succeed if the base filter() succeeds.
      //
      const target* pt (file_rule::filter (is, a, t, p, me).first);
      if (pt == nullptr)
        return make_pair (pt, options);

      // Don't install executable's or runtime-only library's prerequisite
      // headers and module interfaces.
      //
      // Note that if they come from a group, then we assume the entire
      // group is not to be installed.
      //
      // We also skip sources since they may "pull" a header if they are a
      // member of an ad hoc group.
      //
      auto header_source = [this] (const auto& p)
      {
        return (x_header (p)                                   ||
                p.is_a (x_src)                                 ||
                p.is_a (c::static_type)                        ||
                p.is_a (S::static_type)                        ||
                (x_mod != nullptr && p.is_a (*x_mod))          ||
                (x_obj != nullptr && (p.is_a (*x_obj)          ||
                                      p.is_a (m::static_type))));
      };

      if (t.is_a<exe> () ||
          (a.operation () != update_id &&
           me.cur_options == lib::option_install_runtime))
      {
        if (header_source (p))
          pt = nullptr;
        else if (p.type.see_through ())
        {
          for (i.enter_group (); i.group (); )
          {
            ++i; // Note that we have to iterate until the end of the group.
            if (pt != nullptr && header_source (*i))
              pt = nullptr;
          }
        }

        if (pt == nullptr)
          return make_pair (pt, options);
      }

      // Here is a problem: if the user spells the obj*/bmi*{} targets
      // explicitly, then the source files, including headers/modules may be
      // specified as preprequisites of those targets and not of this target.
      // While this can be worked around for headers by also listing them as
      // prerequisites of this target, this won't work for modules (since they
      // are compiled). So what we are going to do here is detect bmi*{} and
      // translate them to their mxx{} (this doesn't quite work for headers
      // since there would normally be many of them).
      //
      // Note: for now we assume bmi*{} never come from see-through groups.
      //
      bool g (false);
      if (p.is_a<bmi> () || (g = p.is_a (compile_types (ot).bmi)))
      {
        if (g)
          resolve_group (a, *pt);

        for (prerequisite_member pm:
               group_prerequisite_members (a, *pt, members_mode::maybe))
        {
          // This is tricky: we need to "look" inside groups for mxx{} but if
          // found, remap to the group, not member.
          //
          if (pm.is_a (*x_mod))
          {
            pt = t.is_a<exe> ()
              ? nullptr
              : file_rule::filter (is, a, *pt, pm.prerequisite, me).first;
            break;
          }
        }

        if (pt == nullptr)
          return make_pair (pt, options);
      }

      return make_pair (pt, options);
    }

    bool install_rule::
    match (action a, target& t, const string&, match_extra& me) const
    {
      // We only want to handle installation if we are also the ones building
      // this target. So first run link's match().
      //
      return link_.sub_match (x_link, update_id, a, t, me) &&
        file_rule::match (a, t);
    }

    recipe install_rule::
    apply (action a, target& t, match_extra& me) const
    {
      // Handle match options.
      //
      // Do it before calling apply_impl() since we need this information
      // in the filter() callbacks.
      //
      if (a.operation () != update_id)
      {
        if (!t.is_a<exe> ())
        {
          if (me.new_options == 0)
            me.new_options = lib::option_install_runtime; // Minimum we can do.

          me.cur_options = me.new_options;
        }
      }

      recipe r (file_rule::apply_impl (
                  a, t, me,
                  me.cur_options != match_extra::all_options /* reapply */));

      if (r == nullptr)
      {
        me.cur_options = match_extra::all_options; // Noop for all options.
        return noop_recipe;
      }

      if (a.operation () == update_id)
      {
        // Signal to the link rule that this is update for install. And if the
        // update has already been executed, verify it was done for install.
        //
        auto& md (t.data<link_rule::match_data> (a.inner_action ()));

        if (md.for_install)
        {
          // Note: see also append_libraries() for the other half.
          //
          if (!*md.for_install)
            fail << "incompatible " << t << " build" <<
              info << "target already built not for install";
        }
        else
          md.for_install = true;
      }
      else // install or uninstall
      {
        file* ls;
        if ((ls = t.is_a<libs> ()) || t.is_a<liba> ())
        {
          // Derive shared library paths and cache them in the target's aux
          // storage if we are un/installing (used in the *_extra() functions
          // below).
          //
          link_rule::libs_paths lsp;
          if (ls != nullptr && !ls->path ().empty ()) // Not binless.
          {
            const string* p (cast_null<string> (t["bin.lib.prefix"]));
            const string* s (cast_null<string> (t["bin.lib.suffix"]));

            lsp = link_.derive_libs_paths (*ls,
                                           p != nullptr ? p->c_str (): nullptr,
                                           s != nullptr ? s->c_str (): nullptr);
          }

          return install_match_data {move (r), me.cur_options, move (lsp)};
        }
      }

      return r;
    }

    void install_rule::
    apply_posthoc (action a, target& t, match_extra& me) const
    {
      // Similar semantics to filter() above for shared libraries specified as
      // post hoc prerequisites (e.g., plugins).
      //
      if (a.operation () != update_id)
      {
        for (posthoc_prerequisite_target& p: *me.posthoc_prerequisite_targets)
        {
          if (p.target != nullptr && p.target->is_a<libs> ())
          {
            if (t.is_a<exe> ())
              p.match_options = lib::option_install_runtime;
            else
            {
              if (me.cur_options == lib::option_install_runtime)
                p.match_options = lib::option_install_runtime;
            }
          }
        }
      }
    }

    void install_rule::
    reapply (action a, target& t, match_extra& me) const
    {
      tracer trace ("cc::install_rule::reapply");

      assert (a.operation () != update_id && !t.is_a<exe> ());

      l6 ([&]{trace << "rematching " << t
                    << ", current options " << me.cur_options
                    << ", new options " << me.new_options;});

      me.cur_options |= me.new_options;

      // We also need to update options in install_match_data.
      //
      t.data<install_match_data> (a).options = me.cur_options;

      if ((me.new_options & lib::option_install_buildtime) != 0)
      {
        // If we are rematched with the buildtime option, propagate it to our
        // prerequisite libraries.
        //
        for (const target* pt: t.prerequisite_targets[a])
        {
          if (pt != nullptr && (pt->is_a<liba> ()  || pt->is_a<libs> () ||
                                pt->is_a<libua> () || pt->is_a<libus> ()))
          {
            // Go for all options instead of just install_buildtime to avoid
            // any further relocking/reapply (we only support runtime-only or
            // everything).
            //
            rematch_sync (a, *pt, match_extra::all_options);
          }
        }

        // Also to post hoc.
        //
        if (me.posthoc_prerequisite_targets != nullptr)
        {
          for (posthoc_prerequisite_target& p: *me.posthoc_prerequisite_targets)
          {
            if (p.target != nullptr && p.target->is_a<libs> ())
            {
              p.match_options = match_extra::all_options;
            }
          }
        }

        // Also match any additional prerequisites (e.g., headers).
        //
        file_rule::reapply_impl (a, t, me);
      }
    }

    bool install_rule::
    install_extra (const file& t, const install_dir& id) const
    {
      bool r (false);

      if (t.is_a<libs> ())
      {
        const auto& md (t.data<install_match_data> (perform_install_id));

        // Here we may have a bunch of symlinks that we need to install.
        //
        // Note that for runtime-only install we only omit the name that is
        // used for linking (e.g., libfoo.so).
        //
        const scope& rs (t.root_scope ());
        const link_rule::libs_paths& lp (md.libs_paths);

        auto ln = [&t, &rs, &id] (const path& f, const path& l)
        {
          install_l (rs, id, l.leaf (), t, f.leaf (), 2 /* verbosity */);
          return true;
        };

        const path& lk (lp.link);
        const path& ld (lp.load);
        const path& so (lp.soname);
        const path& in (lp.interm);

        const path* f (lp.real);

        if (!in.empty ()) {r = ln (*f, in) || r; f = &in;}
        if (!so.empty ()) {r = ln (*f, so) || r; f = &so;}
        if (!ld.empty ()) {r = ln (*f, ld) || r; f = &ld;}
        if ((md.options & lib::option_install_buildtime) != 0)
        {
          if (!lk.empty ()) {r = ln (*f, lk) || r;}
        }
      }

      return r;
    }

    bool install_rule::
    uninstall_extra (const file& t, const install_dir& id) const
    {
      bool r (false);

      if (t.is_a<libs> ())
      {
        const auto& md (t.data<install_match_data> (perform_uninstall_id));

        // Here we may have a bunch of symlinks that we need to uninstall.
        //
        const scope& rs (t.root_scope ());
        const link_rule::libs_paths& lp (md.libs_paths);

        auto rm = [&rs, &id] (const path& f, const path& l)
        {
          return uninstall_l (rs, id, l.leaf (), f.leaf (), 2 /* verbosity */);
        };

        const path& lk (lp.link);
        const path& ld (lp.load);
        const path& so (lp.soname);
        const path& in (lp.interm);

        const path* f (lp.real);

        if (!in.empty ()) {r = rm (*f, in) || r; f = &in;}
        if (!so.empty ()) {r = rm (*f, so) || r; f = &so;}
        if (!ld.empty ()) {r = rm (*f, ld) || r; f = &ld;}
        if ((md.options & lib::option_install_buildtime) != 0)
        {
          if (!lk.empty ()) {r = rm (*f, lk) || r;}
        }
      }

      return r;
    }

    // libux_install_rule
    //
    libux_install_rule::
    libux_install_rule (data&& d, const link_rule& l)
        : common (move (d)), link_ (l) {}

    pair<const target*, uint64_t> libux_install_rule::
    filter (const scope* is,
            action a, const target& t, prerequisite_iterator& i,
            match_extra& me) const
    {
      using file_rule = install::file_rule;

      const prerequisite& p (i->prerequisite);

      uint64_t options (match_extra::all_options);

      otype ot (link_type (t).type);

      // The "see through" semantics that should be parallel to install_rule
      // above. In particular, here we use libue/libua/libus{} as proxies for
      // exe/liba/libs{} there.
      //

      // @@ TMP: drop eventually.
      //
#if 0
      bool st (t.is_a<libue> () || t.is_a<libus> ()); // Target needs shared.
      bool at (t.is_a<libua> () || t.is_a<libus> ()); // Target needs static.
      assert (st || at);

      if ((st && (p.is_a<libx> () || p.is_a<libs> ())) ||
          (at && (p.is_a<libx> () || p.is_a<liba> ())))
      {
        const target* pt (&search (t, p));

        if (const libx* l = pt->is_a<libx> ())
          pt = link_member (*l, a, link_info (t.base_scope (), ot));

        if ((st && pt->is_a<libs> ()) || (at && pt->is_a<liba> ()))
        {
          if (a.operation () != update_id)
          {
            if (t.is_a<libue> ())
              options = lib::option_install_runtime;
            else
            {
              if (me.cur_options == lib::option_install_runtime)
                options = lib::option_install_runtime;
            }
          }

          return make_pair (is == nullptr || pt->in (*is) ? pt : nullptr,
                            options);
        }

        if (pt->is_a<libux> ())
        {
          if (a.operation () != update_id && !pt->is_a<libue> ())
          {
            if (t.is_a<libue> ())
              options = lib::option_install_runtime;
            else
            {
              if (me.cur_options == lib::option_install_runtime)
                options = lib::option_install_runtime;
            }
          }

          return make_pair (pt, options);
        }
      }
#else
      if (p.is_a<libx> () || p.is_a<libs> () || p.is_a<liba> ())
      {
        const target* pt (&search (t, p));

        if (const libx* l = pt->is_a<libx> ())
          pt = link_member (*l, a, link_info (t.base_scope (), ot));

        if (a.operation () != update_id)
        {
          if (t.is_a<libue> ())
            options = lib::option_install_runtime;
          else
          {
            if (me.cur_options == lib::option_install_runtime)
              options = lib::option_install_runtime;
          }
        }

        if (pt->is_a<libs> () || pt->is_a<liba> ())
        {
          return make_pair (is == nullptr || pt->in (*is) ? pt : nullptr,
                            options);
        }
        else
          return make_pair (pt, options);
      }
#endif

      const target* pt (file_rule::instance.filter (is, a, t, p, me).first);
      if (pt == nullptr)
        return make_pair (pt, options);

      auto header_source = [this] (const auto& p)
      {
        return (x_header (p)                                   ||
                p.is_a (x_src)                                 ||
                p.is_a (c::static_type)                        ||
                p.is_a (S::static_type)                        ||
                (x_mod != nullptr && p.is_a (*x_mod))          ||
                (x_obj != nullptr && (p.is_a (*x_obj)          ||
                                      p.is_a (m::static_type))));
      };

      if (t.is_a<libue> () ||
          (a.operation () != update_id &&
           me.cur_options == lib::option_install_runtime))
      {
        if (header_source (p))
          pt = nullptr;
        else if (p.type.see_through ())
        {
          for (i.enter_group (); i.group (); )
          {
            ++i;
            if (pt != nullptr && header_source (*i))
              pt = nullptr;
          }
        }

        if (pt == nullptr)
          return make_pair (pt, options);
      }

      bool g (false);
      if (p.is_a<bmi> () || (g = p.is_a (compile_types (ot).bmi)))
      {
        if (g)
          resolve_group (a, *pt);

        for (prerequisite_member pm:
               group_prerequisite_members (a, *pt, members_mode::maybe))
        {
          if (pm.is_a (*x_mod))
          {
            pt = t.is_a<libue> ()
              ? nullptr
              : file_rule::instance.filter (
                  is, a, *pt, pm.prerequisite, me).first;
            break;
          }
        }

        if (pt == nullptr)
          return make_pair (pt, options);
      }

      return make_pair (pt, options);
    }

    bool libux_install_rule::
    match (action a, target& t, const string&, match_extra& me) const
    {
      // We only want to handle installation if we are also the ones building
      // this target. So first run link's match().
      //
      return link_.sub_match (x_link, update_id, a, t, me) &&
        alias_rule::match (a, t);
    }

    recipe libux_install_rule::
    apply (action a, target& t, match_extra& me) const
    {
      if (a.operation () != update_id)
      {
        if (!t.is_a<libue> ())
        {
          if (me.new_options == 0)
            me.new_options = lib::option_install_runtime;

          me.cur_options = me.new_options;
        }
      }

      return alias_rule::apply_impl (
        a, t, me, me.cur_options != match_extra::all_options /* reapply */);
    }

    void libux_install_rule::
    apply_posthoc (action a, target& t, match_extra& me) const
    {
      if (a.operation () != update_id)
      {
        for (posthoc_prerequisite_target& p: *me.posthoc_prerequisite_targets)
        {
          if (p.target != nullptr && p.target->is_a<libs> ())
          {
            if (t.is_a<libue> ())
              p.match_options = lib::option_install_runtime;
            else
            {
              if (me.cur_options == lib::option_install_runtime)
                p.match_options = lib::option_install_runtime;
            }
          }
        }
      }
    }

    void libux_install_rule::
    reapply (action a, target& t, match_extra& me) const
    {
      tracer trace ("cc::linux_install_rule::reapply");

      assert (a.operation () != update_id && !t.is_a<libue> ());

      l6 ([&]{trace << "rematching " << t
                    << ", current options " << me.cur_options
                    << ", new options " << me.new_options;});

      me.cur_options |= me.new_options;

      if ((me.new_options & lib::option_install_buildtime) != 0)
      {
        for (const target* pt: t.prerequisite_targets[a])
        {
          if (pt != nullptr && (pt->is_a<liba> ()  || pt->is_a<libs> () ||
                                pt->is_a<libua> () || pt->is_a<libus> ()))
            rematch_sync (a, *pt, match_extra::all_options);
        }

        if (me.posthoc_prerequisite_targets != nullptr)
        {
          for (posthoc_prerequisite_target& p: *me.posthoc_prerequisite_targets)
          {
            if (p.target != nullptr && p.target->is_a<libs> ())
            {
              p.match_options = match_extra::all_options;
            }
          }
        }

        alias_rule::reapply_impl (a, t, me);
      }
    }
  }
}
