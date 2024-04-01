// file      : libbuild2/adhoc-rule-cxx.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/adhoc-rule-cxx.hxx>

#include <libbutl/filesystem.hxx> // file_time()

#include <libbuild2/file.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

using namespace butl;

namespace build2
{
  // cxx_rule_v1
  //
  bool cxx_rule_v1::
  match (action, target&) const
  {
    return true;
  }

  recipe cxx_rule_v1::
  apply (action, target&) const
  {
    assert (false); // This (or the match_extra version) must be overriden.
    return empty_recipe;
  }

  // adhoc_cxx_rule
  //
  adhoc_cxx_rule::
  adhoc_cxx_rule (string n, const location& l, size_t b,
                  uint64_t v,
                  optional<string> s)
      : adhoc_rule (move (n), l, b),
        version (v),
        separator (move (s)),
        impl (nullptr)
  {
    if (v != 1)
      fail (l) << "unsupported c++ recipe version " << v;
  }

  bool adhoc_cxx_rule::
  recipe_text (const scope&, const target_type&, string&& t, attributes&)
  {
    code = move (t);
    return true;
  }

  adhoc_cxx_rule::
  ~adhoc_cxx_rule ()
  {
    delete impl.load (memory_order_relaxed); // Serial execution.
  }

  void adhoc_cxx_rule::
  dump_text (ostream& os, string& ind) const
  {
    // @@ TODO: indentation is multi-line recipes is off (would need to insert
    //          indentation after every newline).
    //
    os << ind << string (braces, '{') << " c++ " << version << endl
       << ind << code
       << ind << string (braces, '}');
  }

#if defined(BUILD2_BOOTSTRAP) || defined(LIBBUILD2_STATIC_BUILD)
  bool adhoc_cxx_rule::
  match (action, target&, const string&, match_extra&) const
  {
    // Note that we wait until match() (instead of, say, failing in the
    // parser) to allow the presence of ad hoc C++ recipes for other
    // operations.
    //
    fail (loc) << "ad hoc c++ recipe" <<
#ifdef BUILD2_BOOTSTRAP
      info << "running bootstrap build system" << endf;
#else
      info << "running statically-linked build system" << endf;
#endif
  }

#else

  // From module.cxx.
  //
  void
  create_module_context (context&, const location&);

  const target&
  update_in_module_context (context&, const scope&, names tgt,
                            const location&, const path& bf);

  pair<void*, void*>
  load_module_library (const path& lib, const string& sym, string& err);

  bool adhoc_cxx_rule::
  match (action a, target& xt, const string& hint, match_extra& me) const
  {
    const target& t (xt); // See adhoc_rule::match() for background.

    if (pattern != nullptr && !pattern->match (a, t, hint, me))
      return false;

    tracer trace ("adhoc_cxx_rule::match");

    context& ctx (t.ctx);
    const scope& rs (t.root_scope ());

    // The plan is to reduce this to the build system module case as much as
    // possible. Specifically, we switch to the load phase, create a module-
    // like library with the recipe text as a rule implementation, then build
    // and load it.
    //
    // Since the recipe can be shared among multiple targets, several threads
    // can all be trying to do this in parallel.
    //
    // We use the relaxed memory order here because any change must go through
    // the serial load phase. In other words, all we need here is atomicity
    // with ordering/visibility provided by the phase mutex.
    //
    cxx_rule* impl (this->impl.load (memory_order_relaxed));

    while (impl == nullptr) // Breakout loop.
    {
      // Switch the phase to (serial) load and re-check.
      //
      phase_switch ps (ctx, run_phase::load);

      if ((impl = this->impl.load (memory_order_relaxed)) != nullptr)
        break;

      using create_function = cxx_rule_v1* (
        const location&, target_state, const adhoc_rule_pattern*);
      using load_function = create_function* ();

      // The only way to guarantee that the name of our module matches its
      // implementation is to based the name on the implementation hash (plus
      // the language, in case we support other compiled implementations in
      // the future).
      //
      // Unfortunately, this means we will be creating a new project (and
      // leaving behind the old one as garbage) for every change to the
      // recipe. On the other hand, if the recipe is moved around unchanged,
      // we will reuse the same project. In fact, two different recipes (e.g.,
      // in different buildfiles) with the same text will share the project.
      //
      // The fact that we don't incorporate the recipe location into the hash
      // but include it in the source (in the form of the #line directive; see
      // below) has its own problems. If we do nothing extra here, then if a
      // "moved" but otherwise unchanged recipe is updated (for example,
      // because of changes in the build system core), then we may end up with
      // bogus location in the diagnostics.
      //
      // The straightforward solution would be to just update the location in
      // the source code if it has changed. This, however, will lead to
      // unnecessary and probably surprising recompilations since any line
      // count change before the recipe will trigger this update. One key
      // observation here is that we need accurate location information only
      // if we are going to recompile the recipe but the change to location
      // itself does not render the recipe out of date. So what we going to do
      // is factor the location information into its own small header and then
      // keep it up-to-date without changing its modification time.
      //
      // This works well if the project is not shared by multiple recipes.
      // However, if we have recipes in several buildfiles with identical
      // text, then the location information may end up yo-yo'ing depending on
      // which recipe got here first.
      //
      // There doesn't seem to be much we can do about it without incurring
      // other drawbacks/overheads. So the answer is for the user to use an ad
      // hoc rule with the common implementation instead of a bunch of
      // duplicate recipes.
      //
      string id;
      {
        sha256 cs;
        cs.append ("c++");
        cs.append (separator ? *separator : "");
        cs.append (code);
        id = cs.abbreviated_string (12);
      }

      dir_path pd (rs.out_path () /
                   rs.root_extra->build_build_dir /
                   recipes_build_dir /= id);

      path bf (pd / std_buildfile_file);

      string sym ("load_" + id);

      // Check whether the file exists and its last line matches the specified
      // signature.
      //
      // Note: we use the last instead of the first line for extra protection
      // against incomplete writes.
      //
      auto check_sig = [] (const path& f, const string& s) -> bool
      {
        try
        {
          if (!file_exists (f))
            return false;

          ifdstream ifs (f);

          string l;
          while (ifs.peek () != ifdstream::traits_type::eof ())
            getline (ifs, l);

          return l == s;
        }
        catch (const io_error& e)
        {
          fail << "unable to read " << f << ": " << e << endf;
        }
        catch (const system_error& e)
        {
          fail << "unable to access " << f << ": " << e << endf;
        }
      };

      // Calculate (and cache) the global/local fragments split.
      //
      struct fragments
      {
        size_t   global_p; // Start position.
        size_t   global_n; // Length (0 if no global fragment).
        location global_l; // Position.

        size_t   local_p;
        size_t   local_n;
        location local_l;
      };

      auto split = [this, f = optional<fragments> ()] () mutable ->
        const fragments&
      {
        if (f)
          return *f;

        // Note that the code starts from the next line thus +1.
        //
        location gl (loc.file, loc.line + 1, 1);

        if (!separator)
        {
          f = fragments {0, 0, location (), 0, code.size (), gl};
          return *f;
        }

        // Iterate over lines (keeping track of the current line) looking
        // for the separator.
        //
        uint64_t l (gl.line);
        for (size_t b (0), e (b), n (code.size ()); b < n; b = e + 1, l++)
        {
          if ((e = code.find ('\n', b)) == string::npos)
            e = n;

          // Trim the line.
          //
          size_t tb (b), te (e);
          auto ws = [] (char c) {return c == ' ' || c == '\t' || c == '\r';};
          for (; tb != te && ws (code[tb    ]); ++tb) ;
          for (; te != tb && ws (code[te - 1]); --te) ;

          // text << "'" << string (code, tb, te - tb) << "'";

          if (code.compare (tb, te - tb, *separator) == 0)
          {
            // End the global fragment at the previous newline and start the
            // local fragment at the beginning of the next line.
            //
            location ll (loc.file, l + 1, 1);

            if (++e >= n)
              fail (ll) << "empty c++ recipe local fragment";

            f = fragments {0, b, gl, e, n - e, ll};
            return *f;
          }
        }

        fail (loc) << "c++ recipe fragment separator '" << *separator
                   << "' not found" << endf;
      };

      bool nested (ctx.module_context == &ctx);

      // Create the build context if necessary.
      //
      if (ctx.module_context == nullptr)
      {
        if (!ctx.module_context_storage)
          fail (loc) << "unable to update ad hoc recipe for target " << t <<
            info << "building of ad hoc recipes is disabled";

        create_module_context (ctx, loc);
      }

      // Clear current project's environment and "switch" to the module
      // context, including entering a scheduler sub-phase.
      //
      auto_thread_env penv (nullptr);
      context& ctx (*t.ctx.module_context);
      scheduler::phase_guard pg (*ctx.sched);

      uint16_t verbosity (3); // Project creation command verbosity.

      // Project and location signatures.
      //
      // Specifically, we update the project version when changing anything
      // which would make the already existing projects unusable.
      //
      const string& lf (!loc.file.path.empty ()
                        ? loc.file.path.string ()
                        : loc.file.name ? *loc.file.name : string ());

      const string psig ("# c++ " + to_string (version));
      const string lsig ("// " + lf + ':' + to_string (loc.line));

      // Check whether we need to (re)create the project.
      //
      optional<bool> altn (false); // Standard naming scheme.
      bool create (!is_src_root (pd, altn));

      if (!create && (create = !check_sig (bf, psig)))
        rmdir_r (ctx, pd, false, verbosity); // Never dry-run.

      auto diag = [verbosity] (const path& f)
      {
        if (verb >= verbosity)
        {
          if (verb >= 2)
            text << "cat >" << f;
          else if (verb)
            print_diag ("save", f);
        }
      };

      path of;
      ofdstream ofs;

      if (create)
      try
      {
        const fragments& frag (split ());

        // Write ad hoc config.build that loads the ~build2 configuration.
        // This way the configuration will be always in sync with ~build2
        // and we can update the recipe manually (e.g., for debugging).
        //
        // Should we use ~build2 or ~build2-no-warnings? This case is similar
        // to private host/module configurations in that the user doesn't have
        // any control over the options used, etc. So it would be natural to
        // use the no-warnings variant. However, unlike with tools/modules
        // which can be configured in a user-created configuration (and which
        // will normally be the case during development), for recipes it's
        // always this automatically-create configuration. It feels like the
        // best we can do is use ~build2-no-warnings by default but switch to
        // ~build2 if the project is configured for development
        // (config.<project>.develop).
        //
        string cfg;
        {
          const project_name& pn (named_project (rs));

          if (!pn.empty ())
          {
            string var ("config." + pn.variable () + ".develop");

            if (lookup l = rs[var])
            {
              // The value could be untyped if the project didn't declare this
              // variable. Let's handle that case gracefully.
              //
              try
              {
                if (convert<bool> (*l))
                  cfg = "~build2";
              }
              catch (const invalid_argument& e)
              {
                fail << "invalid " << var << " value: " << e;
              }
            }
          }

          if (cfg.empty ())
            cfg = "~build2-no-warnings";
        }

        create_project (
          pd,
          dir_path (),                             /* amalgamation */
          {},                                      /* boot_modules */
          "cxx.std = latest",                      /* root_pre */
          {"cxx."},                                /* root_modules */
          "",                                      /* root_post */
          string ("config"),                       /* config_module */
          "config.config.load = " + cfg,           /* config_file */
          false,                                   /* buildfile */
          "build2 core",                           /* who */
          verbosity);                              /* verbosity */


        // Write the rule source file.
        //
        of = path (pd / "rule.cxx");

        diag (of);

        ofs.open (of);

        ofs << "#include \"location.hxx\""                              << '\n'
            << '\n';

        // Include every header that can plausibly be needed by a rule.
        //
        // @@ TMP: any new headers to add? [Keep this note for review.]
        //
        ofs << "#include <libbuild2/types.hxx>"                         << '\n'
            << "#include <libbuild2/forward.hxx>"                       << '\n'
            << "#include <libbuild2/utility.hxx>"                       << '\n'
            << '\n'
            << "#include <libbuild2/file.hxx>"                          << '\n'
            << "#include <libbuild2/rule.hxx>"                          << '\n'
            << "#include <libbuild2/depdb.hxx>"                         << '\n'
            << "#include <libbuild2/scope.hxx>"                         << '\n'
            << "#include <libbuild2/target.hxx>"                        << '\n'
            << "#include <libbuild2/recipe.hxx>"                        << '\n'
            << "#include <libbuild2/dyndep.hxx>"                        << '\n'
            << "#include <libbuild2/context.hxx>"                       << '\n'
            << "#include <libbuild2/variable.hxx>"                      << '\n'
            << "#include <libbuild2/algorithm.hxx>"                     << '\n'
            << "#include <libbuild2/filesystem.hxx>"                    << '\n'
            << "#include <libbuild2/diagnostics.hxx>"                   << '\n'
            << "#include <libbuild2/adhoc-rule-cxx.hxx>"                << '\n'
            << '\n';

        // Write the global fragment, if any. Note that it always includes the
        // trailing newline.
        //
        if (frag.global_n != 0)
        {
          // Use the #line directive to point diagnostics to the code in the
          // buildfile. Note that there is no easy way to restore things to
          // point back to the source file (other than another #line with a
          // line and a file). Let's not bother for now.
          //
          ofs << "#line RECIPE_GLOBAL_LINE RECIPE_FILE"                 << '\n';
          ofs.write (code.c_str () + frag.global_p, frag.global_n);
          ofs << '\n';
        }

        // Normally the recipe code will have one level of indentation so
        // let's not indent the namespace level to match.
        //
        ofs << "namespace build2"                                       << '\n'
            << "{"                                                      << '\n'
            << '\n';

        // If we want the user to be able to supply a custom constuctor, then
        // we have to give the class a predictable name (i.e., we cannot use
        // id as part of its name) and put it into an unnamed namespace. One
        // clever idea is to call the class `constructor` but the name could
        // also be used for a custom destructor (still could work) or for name
        // qualification (would definitely look bizarre).
        //
        // In this light the most natural name is probable `rule`. The issue
        // is we already have this name in the build2 namespace (and its our
        // indirect base). In fact, any name that we choose could in the
        // future conflict with something in that namespace so maybe it makes
        // sense to bite the bullet and pick a name that is least likely to be
        // used by the user directly (can always use cxx_rule instead).
        //
        ofs << "namespace"                                              << '\n'
            << "{"                                                      << '\n'
            << "class rule: public cxx_rule_v1"                         << '\n'
            << "{"                                                      << '\n'
            << "public:"                                                << '\n'
            << '\n';

        // Inherit base constructor. This way the user may provide their own
        // but don't have to.
        //
        ofs << "  using cxx_rule_v1::cxx_rule_v1;"                      << '\n'
            << '\n';

        // An extern "C" function cannot throw which can happen in case of a
        // user-defined constructor. So we need an extra level of indirection.
        // We incorporate id to make sure it doesn't conflict with anything
        // user-defined.
        //
        ofs << "  static cxx_rule_v1*"                                  << '\n'
            << "  create_" << id << " (const location& l, target_state s, " <<
          "const adhoc_rule_pattern* p)"                                << '\n'
            << "  {"                                                    << '\n'
            << "    return new rule (l, s, p);"                         << '\n'
            << "  }"                                                    << '\n'
            << '\n';

        // Use the #line directive to point diagnostics to the code in the
        // buildfile similar to the global fragment above.
        //
        ofs << "#line RECIPE_LOCAL_LINE RECIPE_FILE"                    << '\n';

        // Note that the local fragment always includes the trailing newline.
        //
        ofs.write (code.c_str () + frag.local_p, frag.local_n);
        ofs << "};"                                                     << '\n'
            << '\n';

        // Add an alias that we can use unambiguously in the load function.
        //
        ofs << "using rule_" << id << " = rule;"                        << '\n'
            << "}"                                                      << '\n'
            << '\n';

        // Entry point.
        //
        ofs << "extern \"C\""                                           << '\n'
            << "#ifdef _WIN32"                                          << '\n'
            << "__declspec(dllexport)"                                  << '\n'
            << "#endif"                                                 << '\n'
            << "cxx_rule_v1* (*" << sym << " ()) (const location&, " <<
          "target_state, const adhoc_rule_pattern*)"                    << '\n'
            << "{"                                                      << '\n'
            << "  return &rule_" << id << "::create_" << id << ";"      << '\n'
            << "}"                                                      << '\n'
            << '\n';

        ofs << "}"                                                      << '\n';

        ofs.close ();


        // Write buildfile.
        //
        of = bf;

        diag (of);

        ofs.open (of);

        ofs << "import impl_libs += build2%lib{build2}"                 << '\n'
            << "libs{" << id << "}: cxx{rule} hxx{location} $impl_libs" << '\n'
            << '\n'
            << "if ($cxx.target.system == 'win32-msvc')"                << '\n'
            << "  cxx.poptions += -D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS" << '\n'
            << '\n'
            << "if ($cxx.class == 'msvc')"                              << '\n'
            << "  cxx.coptions += /wd4251 /wd4275 /wd4800"              << '\n'
            << '\n'
            << psig                                                     << '\n';

        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to write to " << of << ": " << e;
      }

      // Update the library target in the module context.
      //
      const target* l (nullptr);
      do // Breakout loop.
      {
        // Load the project in the module context.
        //
        // Note that it's possible it has already been loaded (see above about
        // the id calculation).
        //
        scope& rs (load_project (ctx, pd, pd, false /* forwarded */));

        auto find_target = [&ctx, &rs, &pd, &id] ()
        {
          const target_type* tt (rs.find_target_type ("libs"));
          assert (tt != nullptr);

          const target* t (
            ctx.targets.find (*tt, pd, dir_path () /* out */, id));
          assert (t != nullptr);

          return t;
        };

        // If the project has already been loaded then, as an optimization,
        // check if the target has already been updated (this will make a
        // difference we if we have identical recipes in several buildfiles,
        // especially to the location update that comes next).
        //
        if (!source_once (rs, rs, bf))
        {
          l = find_target ();

          if (l->executed_state (perform_update_id) != target_state::unknown)
            break;
        }

        // Create/update the recipe location header.
        //
        // For update, preserve the file timestamp in order not to render the
        // recipe out of date.
        //
        of = path (pd / "location.hxx");
        if (!check_sig (of, lsig))
        try
        {
          const fragments& frag (split ());

          entry_time et (file_time (of));

          diag (of);

          ofs.open (of);

          // Recipe file and line for the #line directive above. We also need
          // to escape backslashes (Windows paths).
          //
          ofs << "#define RECIPE_FILE \"" << sanitize_strlit (lf) << '"'<< '\n';

          if (frag.global_n != 0)
            ofs << "#define RECIPE_GLOBAL_LINE " << frag.global_l.line  << '\n';

          ofs << "#define RECIPE_LOCAL_LINE " << frag.local_l.line      << '\n'
              << '\n'
              << lsig                                                   << '\n';

          ofs.close ();

          if (et.modification != timestamp_nonexistent)
            file_time (of, et);
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << of << ": " << e;
        }
        catch (const system_error& e)
        {
          fail << "unable to get/set timestamp for " << of << ": " << e;
        }

        if (nested)
        {
          // This means there is a perform update action already in progress
          // in this context. So we are going to switch the phase and
          // perform direct match and update (similar how we do this for
          // generated headers).
          //
          // Note that since neither match nor execute are serial phases, it
          // means other targets in this context can be matched and executed
          // in paralellel with us.
          //
          if (l == nullptr)
            l = find_target ();

          phase_switch mp (ctx, run_phase::match);
          if (match_sync (perform_update_id, *l) != target_state::unchanged)
          {
            phase_switch ep (ctx, run_phase::execute);
            execute_sync (a, *l);
          }
        }
        else
        {
          // Cutoff the existing diagnostics stack and push our own entry.
          //
          diag_frame::stack_guard diag_cutoff (nullptr);

          auto df = make_diag_frame (
            [this, &t] (const diag_record& dr)
            {
              dr << info (loc) << "while updating ad hoc recipe for target "
                 << t;
            });

          l = &update_in_module_context (
            ctx, rs, names {name (pd, "libs", id)},
            loc, bf);
        }
      } while (false);

      // Load the library.
      //
      const path& lib (l->as<file> ().path ());

      // Note again that it's possible the library has already been loaded
      // (see above about the id calculation).
      //
      string err;
      pair<void*, void*> hs (load_module_library (lib, sym, err));

      // These normally shouldn't happen unless something is seriously broken.
      //
      if (hs.first == nullptr)
        fail (loc) << "unable to load recipe library " << lib << ": " << err;

      if (hs.second == nullptr)
        fail (loc) << "unable to lookup " << sym << " in recipe library "
                   << lib << ": " << err;

      {
        auto df = make_diag_frame (
          [this](const diag_record& dr)
          {
            if (verb != 0)
              dr << info (loc) << "while initializing ad hoc recipe";
          });

        load_function* lf (function_cast<load_function*> (hs.second));
        create_function* cf (lf ());

        impl = cf (loc, l->executed_state (perform_update_id), pattern);
        this->impl.store (impl, memory_order_relaxed); // Still in load phase.
      }
    }

    return impl->match (a, xt, hint, me);
  }
#endif // BUILD2_BOOTSTRAP || LIBBUILD2_STATIC_BUILD

  recipe adhoc_cxx_rule::
  apply (action a, target& t, match_extra& me) const
  {
    // Handle matching explicit group member (see adhoc_rule::match() for
    // background).
    //
    if (const group* g = (t.group != nullptr
                          ? t.group->is_a<group> ()
                          : nullptr))
    {
      // @@ Hm, this looks very similar to how we handle ad hoc group members.
      //    Shouldn't impl be given a chance to translate options or some
      //    such?
      //
      match_sync (a, *g, 0 /* options */);
      return group_recipe; // Execute the group's recipe.
    }

    // Note that while we probably could call pattern's apply_group_members()
    // here, apply_group_prerequisites() is normally called after adding
    // prerequisites but before matching, which can only be done from the
    // rule's implementation. Also, for apply_group_members(), there is the
    // explicit group special case which may also require custom logic.
    // So it feels best to leave both to the implementation.

    return impl.load (memory_order_relaxed)->apply (a, t, me);
  }

  void adhoc_cxx_rule::
  reapply (action a, target& t, match_extra& me) const
  {
    return impl.load (memory_order_relaxed)->reapply (a, t, me);
  }
}
