// file      : build2/cc/compile.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/compile.hxx>

#include <cstdlib>  // exit()
#include <iostream> // cerr

#include <build2/depdb.hxx>
#include <build2/scope.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>
#include <build2/algorithm.hxx>
#include <build2/diagnostics.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/parser.hxx>
#include <build2/cc/target.hxx>  // h
#include <build2/cc/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    template <typename T>
    inline bool
    operator< (preprocessed l, T r) // Template because of VC14 bug.
    {
      return static_cast<uint8_t> (l) < static_cast<uint8_t> (r);
    }

    preprocessed
    to_preprocessed (const string& s)
    {
      if (s == "none")     return preprocessed::none;
      if (s == "includes") return preprocessed::includes;
      if (s == "modules")  return preprocessed::modules;
      if (s == "all")      return preprocessed::all;
      throw invalid_argument ("invalid preprocessed value '" + s + "'");
    }

    struct compile::match_data
    {
      explicit
      match_data (bool m, const prerequisite_member& s)
          : mod (m),
            src (s),
            pp (preprocessed::none),
            mt (timestamp_unknown) {}

      bool mod;                  // Target is bmi*{} and src is x_mod.
      prerequisite_member src;
      preprocessed pp;
      auto_rmfile psrc;          // Preprocessed source, if any.
      timestamp mt;              // Target timestamp.
    };

    compile::
    compile (data&& d)
        : common (move (d)),
          rule_id (string (x) += ".compile 1")
    {
      static_assert (sizeof (compile::match_data) <= target::data_size,
                     "insufficient space");
    }

    const char* compile::
    langopt (const match_data& md) const
    {
      bool m (md.mod);
      //preprocessed p (md.pp);

      switch (cid)
      {
      case compiler_id::gcc:
        {
          // Ignore the preprocessed value since for GCC it is handled via
          // -fpreprocessed -fdirectives-only.
          //
          switch (x_lang)
          {
          case lang::c:   return "c";
          case lang::cxx: return "c++";
          }
        }
      case compiler_id::clang:
        {
          // Clang has *-cpp-output (but not c++-module-cpp-output) and they
          // handle comments and line continuations. However, currently this
          // is only by accident since these modes are essentially equivalent
          // to their cpp-output-less versions.
          //
          switch (x_lang)
          {
          case lang::c:   return "c";
          case lang::cxx: return m ? "c++-module" : "c++";
          }
        }
      case compiler_id::msvc:
        {
          switch (x_lang)
          {
          case lang::c:   return "/TC";
          case lang::cxx: return "/TP";
          }
        }
      case compiler_id::icc:
        {
          switch (x_lang)
          {
          case lang::c:   return "c";
          case lang::cxx: return "c++";
          }
        }
      }

      return nullptr;
    }

    match_result compile::
    match (action act, target& t, const string&) const
    {
      tracer trace (x, "compile::match");

      bool mod (t.is_a<bmie> () || t.is_a<bmia> () || t.is_a<bmis> ());

      // Link-up to our group (this is the obj/bmi{} target group protocol
      // which means this can be done whether we match or not).
      //
      if (t.group == nullptr)
      {
        const target_type& tt (mod ? bmi::static_type : obj::static_type);
        t.group = targets.find (tt, t.dir, t.out, t.name);
      }

      // See if we have a source file. Iterate in reverse so that a source
      // file specified for a member overrides the one specified for the
      // group. Also "see through" groups.
      //
      for (prerequisite_member p: reverse_group_prerequisite_members (act, t))
      {
        if (p.is_a (mod ? *x_mod : x_src))
        {
          // Save in the target's auxilary storage.
          //
          t.data (match_data (mod, p));
          return true;
        }
      }

      l4 ([&]{trace << "no " << x_lang << " source file for target " << t;});
      return false;
    }

    // Append or hash library options from a pair of *.export.* variables
    // (first one is cc.export.*) recursively, prerequisite libraries first.
    //
    void compile::
    append_lib_options (const scope& bs,
                        cstrings& args,
                        const target& t,
                        action act,
                        lorder lo) const
    {
      auto opt = [&args, this] (
        const file& l, const string& t, bool com, bool exp)
      {
        // Note that in our model *.export.poptions are always "interface",
        // even if set on liba{}/libs{}, unlike loptions.
        //
        assert (exp);

        const variable& var (
          com
          ? c_export_poptions
          : (t == x ? x_export_poptions : var_pool[t + ".export.poptions"]));

        append_options (args, l, var);
      };

      // In case we don't have the "small function object" optimization.
      //
      const function<void (const file&, const string&, bool, bool)> optf (opt);

      // Note that here we don't need to see group members.
      //
      for (const prerequisite& p: group_prerequisites (t))
      {
        // Should be already searched and matched.
        //
        const target* pt (p.target.load (memory_order_consume));

        bool a;

        if (const lib* l = pt->is_a<lib> ())
          a = (pt = &link_member (*l, act, lo))->is_a<liba> ();
        else if (!(a = pt->is_a<liba> ()) && !pt->is_a<libs> ())
          continue;

        process_libraries (act, bs, lo, sys_lib_dirs,
                           pt->as<file> (), a,
                           nullptr, nullptr, optf);
      }
    }

    void compile::
    hash_lib_options (const scope& bs,
                      sha256& cs,
                      const target& t,
                      action act,
                      lorder lo) const
    {
      auto opt = [&cs, this] (
        const file& l, const string& t, bool com, bool exp)
      {
        assert (exp);

        const variable& var (
          com
          ? c_export_poptions
          : (t == x ? x_export_poptions : var_pool[t + ".export.poptions"]));

        hash_options (cs, l, var);
      };

      // In case we don't have the "small function object" optimization.
      //
      const function<void (const file&, const string&, bool, bool)> optf (opt);

      for (const prerequisite& p: group_prerequisites (t))
      {
        // Should be already searched and matched.
        //
        const target* pt (p.target.load (memory_order_consume));

        bool a;

        if (const lib* l = pt->is_a<lib> ())
          a = (pt = &link_member (*l, act, lo))->is_a<liba> ();
        else if (!(a = pt->is_a<liba> ()) && !pt->is_a<libs> ())
          continue;

        process_libraries (act, bs, lo, sys_lib_dirs,
                           pt->as<file> (), a,
                           nullptr, nullptr, optf);
      }
    }

    // Append library prefixes based on the *.export.poptions variables
    // recursively, prerequisite libraries first.
    //
    void compile::
    append_lib_prefixes (const scope& bs,
                         prefix_map& m,
                         target& t,
                         action act,
                         lorder lo) const
    {
      auto opt = [&m, this] (
        const file& l, const string& t, bool com, bool exp)
      {
        assert (exp);

        const variable& var (
          com
          ? c_export_poptions
          : (t == x ? x_export_poptions : var_pool[t + ".export.poptions"]));

        append_prefixes (m, l, var);
      };

      // In case we don't have the "small function object" optimization.
      //
      const function<void (const file&, const string&, bool, bool)> optf (opt);

      for (const prerequisite& p: group_prerequisites (t))
      {
        // Should be already searched and matched.
        //
        const target* pt (p.target.load (memory_order_consume));

        bool a;

        if (const lib* l = pt->is_a<lib> ())
          a = (pt = &link_member (*l, act, lo))->is_a<liba> ();
        else if (!(a = pt->is_a<liba> ()) && !pt->is_a<libs> ())
          continue;

        process_libraries (act, bs, lo, sys_lib_dirs,
                           pt->as<file> (), a,
                           nullptr, nullptr, optf);
      }
    }

    // Update the target during the match phase. Return true if it has changed
    // or if the passed timestamp is not timestamp_unknown and is older than
    // the target.
    //
    // This function is used to make sure header dependencies are up to date.
    //
    // There would normally be a lot of headers for every source file (think
    // all the system headers) and just calling execute_direct() on all of
    // them can get expensive. At the same time, most of these headers are
    // existing files that we will never be updating (again, system headers,
    // for example) and the rule that will match them is the fallback
    // file_rule. That rule has an optimization: it returns noop_recipe (which
    // causes the target state to be automatically set to unchanged) if the
    // file is known to be up to date. So we do the update "smartly".
    //
    static bool
    update (tracer& trace, action act, const target& t, timestamp ts)
    {
      const path_target* pt (t.is_a<path_target> ());

      if (pt == nullptr)
        ts = timestamp_unknown;

      target_state os (t.matched_state (act));

      if (os == target_state::unchanged)
      {
        if (ts == timestamp_unknown)
          return false;
        else
        {
          // We expect the timestamp to be known (i.e., existing file).
          //
          timestamp mt (pt->mtime ()); // @@ MT perf: know target state.
          assert (mt != timestamp_unknown);
          return mt > ts;
        }
      }
      else
      {
        // We only want to return true if our call to execute() actually
        // caused an update. In particular, the target could already have been
        // in target_state::changed because of a dependency extraction run for
        // some other source file.
        //
        // @@ MT perf: so we are going to switch the phase and execute for
        //    any generated header.
        //
        phase_switch ps (run_phase::execute);
        target_state ns (execute_direct (act, t));

        if (ns != os && ns != target_state::unchanged)
        {
          l6 ([&]{trace << "updated " << t
                        << "; old state " << os
                        << "; new state " << ns;});
          return true;
        }
        else
          return ts != timestamp_unknown ? pt->newer (ts) : false;
      }
    }

    recipe compile::
    apply (action act, target& xt) const
    {
      tracer trace (x, "compile::apply");

      file& t (xt.as<file> ()); // Either obj*{} or bmi*{}.

      match_data& md (t.data<match_data> ());
      bool mod (md.mod);

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      otype ct (compile_type (t, mod));
      lorder lo (link_order (bs, ct));
      compile_target_types tt (compile_types (ct));

      // Derive file name from target name.
      //
      string e; // Primary target extension (module or object).
      {
        const char* o ("o"); // Object extension (.o or .obj).

        if (tsys == "win32-msvc")
        {
          switch (ct)
          {
          case otype::e: e = "exe."; break;
          case otype::a: e = "lib."; break;
          case otype::s: e = "dll."; break;
          }
          o = "obj";
        }
        else if (tsys == "mingw32")
        {
          switch (ct)
          {
          case otype::e: e = "exe."; break;
          case otype::a: e = "a.";   break;
          case otype::s: e = "dll."; break;
          }
        }
        else if (tsys == "darwin")
        {
          switch (ct)
          {
          case otype::e: e = "";       break;
          case otype::a: e = "a.";     break;
          case otype::s: e = "dylib."; break;
          }
        }
        else
        {
          switch (ct)
          {
          case otype::e: e = "";    break;
          case otype::a: e = "a.";  break;
          case otype::s: e = "so."; break;
          }
        }

        switch (cid)
        {
        case compiler_id::gcc:
          {
            e += mod ? "nms" : o;
            break;
          }
        case compiler_id::clang:
          {
            e += mod ? "pcm" : o;
            break;
          }
        case compiler_id::msvc:
        case compiler_id::icc:
          {
            e += mod ? "ifc" : o;
            break;
          }
        }

        // If we are compiling a module, then the obj*{} is an ad hoc member
        // of bmi*{}.
        //
        if (mod)
        {
          // The module interface unit can be the same as an implementation
          // (e.g., foo.mxx and foo.cxx) which means obj*{} targets could
          // collide. So we add the module extension to the target name.
          //
          target_lock obj (add_adhoc_member (act, t, tt.obj, e.c_str ()));
          obj.target->as<file> ().derive_path (o);
          match_recipe (obj, group_recipe); // Set recipe and unlock.
        }
      }

      const path& tp (t.derive_path (e.c_str ()));

      // Inject dependency on the output directory.
      //
      const fsdir* dir (inject_fsdir (act, t));

      // Match all the existing prerequisites. The injection code takes care
      // of the ones it is adding.
      //
      // When cleaning, ignore prerequisites that are not in the same or a
      // subdirectory of our project root.
      //
      auto& pts (t.prerequisite_targets);
      optional<dir_paths> usr_lib_dirs; // Extract lazily.

      // Start asynchronous matching of prerequisites. Wait with unlocked
      // phase to allow phase switching.
      //
      wait_guard wg (target::count_busy (), t.task_count, true);

      size_t start (pts.size ()); // Index of the first to be added.
      for (prerequisite_member p: group_prerequisite_members (act, t))
      {
        const target* pt (nullptr);

        // A dependency on a library is there so that we can get its
        // *.export.poptions, modules, etc. This is the "library
        // meta-information protocol". See also append_lib_options().
        //
        if (p.is_a<lib> () || p.is_a<liba> () || p.is_a<libs> ())
        {
          if (act.operation () == update_id)
          {
            // Handle (phase two) imported libraries. We know that for such
            // libraries we don't need to do match() in order to get options
            // (if any, they would be set by search_library()).
            //
            if (p.proj ())
            {
              if (search_library (act,
                                  sys_lib_dirs,
                                  usr_lib_dirs,
                                  p.prerequisite) != nullptr)
                continue;
            }

            pt = &p.search (t);

            if (const lib* l = pt->is_a<lib> ())
              pt = &link_member (*l, act, lo);
          }
          else
            continue;
        }
        //
        // For modules we pick only what we import which is done below so
        // skip it here. One corner case is clean: we assume that someone
        // else (normally library/executable) also depends on it and will
        // clean it up.
        //
        else if (p.is_a<bmi> () || p.is_a (tt.bmi))
          continue;
        else
        {
          pt = &p.search (t);

          if (act.operation () == clean_id && !pt->dir.sub (rs.out_path ()))
            continue;
        }

        match_async (act, *pt, target::count_busy (), t.task_count);
        pts.push_back (pt);
      }

      wg.wait ();

      // Finish matching all the targets that we have started.
      //
      for (size_t i (start), n (pts.size ()); i != n; ++i)
      {
        const target*& pt (pts[i]);

        // Making sure a library is updated before us will only restrict
        // parallelism. But we do need to match it in order to get its imports
        // resolved and prerequisite_targets populated. So we match it but
        // then unmatch if it is safe. And thanks to the two-pass prerequisite
        // match in link::apply() it will be safe unless someone is building
        // an obj?{} target directory.
        //
        if (build2::match (act,
                           *pt,
                           pt->is_a<liba> () || pt->is_a<libs> ()
                           ? unmatch::safe
                           : unmatch::none))
          pt = nullptr; // Ignore in execute.
      }

      // Inject additional prerequisites. We only do it when performing update
      // since chances are we will have to update some of our prerequisites in
      // the process (auto-generated source code).
      //
      if (act == perform_update_id)
      {
        // The cached prerequisite target should be the same as what is in
        // t.prerequisite_targets since we used standard search() and match()
        // above.
        //
        const file& src (*md.src.search (t).is_a<file> ());

        // Make sure the output directory exists.
        //
        // Is this the right thing to do? It does smell a bit, but then we do
        // worse things in inject_prerequisites() below. There is also no way
        // to postpone this until update since we need to extract and inject
        // header dependencies now (we don't want to be calling search() and
        // match() in update), which means we need to cache them now as well.
        // So the only alternative, it seems, is to cache the updates to the
        // database until later which will sure complicate (and slow down)
        // things.
        //
        if (dir != nullptr)
        {
          // We can do it properly by using execute_direct(). But this means
          // we will be switching to the execute phase with all the associated
          // overheads. At the same time, in case of update, creation of a
          // directory is not going to change the external state in any way
          // that would affect any parallel efforts in building the internal
          // state. So we are just going to create the directory directly.
          // Note, however, that we cannot modify the fsdir{} target since
          // this can very well be happening in parallel. But that's not a
          // problem since fsdir{}'s update is idempotent.
          //
          fsdir_rule::perform_update_direct (act, t);
        }

        depdb dd (tp + ".d");

        // First should come the rule name/version.
        //
        if (dd.expect (rule_id) != nullptr)
          l4 ([&]{trace << "rule mismatch forcing update of " << t;});

        // Then the compiler checksum. Note that here we assume it
        // incorporates the (default) target so that if the compiler changes
        // but only in what it targets, then the checksum will still change.
        //
        if (dd.expect (cast<string> (rs[x_checksum])) != nullptr)
          l4 ([&]{trace << "compiler mismatch forcing update of " << t;});

        // Then the options checksum.
        //
        // The idea is to keep them exactly as they are passed to the compiler
        // since the order may be significant.
        //
        sha256 cs;

        // This affects how we compile the source so factor it in.
        //
        cs.append (&md.pp, sizeof (md.pp));

        if (md.pp != preprocessed::all)
        {
          hash_options (cs, t, c_poptions);
          hash_options (cs, t, x_poptions);

          // Hash *.export.poptions from prerequisite libraries.
          //
          hash_lib_options (bs, cs, t, act, lo);

          // Extra system header dirs (last).
          //
          for (const dir_path& d: sys_inc_dirs)
            cs.append (d.string ());
        }

        hash_options (cs, t, c_coptions);
        hash_options (cs, t, x_coptions);
        hash_options (cs, tstd);

        if (ct == otype::s)
        {
          // On Darwin, Win32 -fPIC is the default.
          //
          if (tclass == "linux" || tclass == "bsd")
            cs.append ("-fPIC");
        }

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "options mismatch forcing update of " << t;});

        // Finally the source file.
        //
        if (dd.expect (src.path ()) != nullptr)
          l4 ([&]{trace << "source file mismatch forcing update of " << t;});

        // If any of the above checks resulted in a mismatch (different
        // compiler, options, or source file) or if the depdb is newer than
        // the target, then do unconditional update.
        //
        timestamp mt;
        bool u (dd.writing () || dd.mtime () > (mt = file_mtime (tp)));

        // Update prerequisite targets (normally just the source file).
        //
        // This is an unusual place and time to do it. But we have to do it
        // before extracting dependencies. The reasoning for source file is
        // pretty clear. What other prerequisites could we have? While
        // normally they will be some other sources (as in, static content
        // from src_root), it's possible they are some auto-generated stuff.
        // And it's possible they affect the preprocessor result. Say some ad
        // hoc/out-of-band compiler input file that is passed via the command
        // line. So, to be safe, we make sure everything is up to date.
        //
        for (const target* pt: pts)
        {
          if (pt == nullptr || pt == dir)
            continue;

          u = update (trace, act, *pt, u ? timestamp_unknown : mt) || u;
        }

        // Check if the source is already preprocessed to a certain degree.
        // This determines which of the following steps we perform and on
        // what source (original or preprocessed).
        //
        if (const string* v = cast_null<string> (t[c_preprocessed]))
        try
        {
          md.pp = to_preprocessed (*v);
        }
        catch (const invalid_argument& e)
        {
          fail << "invalid " << c_preprocessed.name << " variable value "
               << "for target " << t << ": " << e;
        }

        // If we have no #include directives, then skip header dependency
        // extraction.
        //
        pair<auto_rmfile, bool> p (auto_rmfile (), false);
        if (md.pp < preprocessed::includes)
          p = extract_headers (act, t, lo, src, md, dd, u);

        // If anything got updated, then we didn't rely on the cache. However,
        // the cached data could actually have been valid and the compiler run
        // in extract_headers() merely validated it.
        //
        // We do need to update the database timestamp, however. Failed
        // that, we will keep re-validating the cached data over and over
        // again.
        //
        if (u && dd.reading ())
          dd.touch ();

        dd.close ();

        // Extract the module dependency information in addition to header
        // dependencies above.
        //
        if (u) // @@ TMP (depdb validation similar to extract_headers()).
        {
          extract_modules (act, t, lo, src, p.first, md, dd, u);
          search_modules (bs, act, t, lo, tt.bmi);
        }

        // If the preprocessed output is suitable for compilation and is not
        // disabled, then pass it along.
        //
        if (p.second && !cast_false<bool> (t[c_reprocess]))
          md.psrc = move (p.first);

        md.mt = u ? timestamp_nonexistent : mt;
      }

      switch (act)
      {
      case perform_update_id: return [this] (action a, const target& t)
        {
          return perform_update (a, t);
        };
      case perform_clean_id: return [this] (action a, const target& t)
        {
          return perform_clean (a, t);
        };
      default: return noop_recipe; // Configure update.
      }
    }

    // Reverse-lookup target type from extension.
    //
    const target_type* compile::
    map_extension (const scope& s, const string& n, const string& e) const
    {
      // We will just have to try all of the possible ones, in the "most
      // likely to match" order.
      //
      auto test = [&s, &n, &e] (const target_type& tt) -> bool
      {
        // Call the extension derivation function. Here we know that it will
        // only use the target type and name from the target key so we can
        // pass bogus values for the rest.
        //
        target_key tk {&tt, nullptr, nullptr, &n, nullopt};

        // This is like prerequisite search.
        //
        if (optional<string> de = tt.extension (tk, s, true))
          if (*de == e)
            return true;

        return false;
      };

      for (const target_type* const* p (x_inc); *p != nullptr; ++p)
        if (test (**p)) return *p;

      return nullptr;
    }

    void compile::
    append_prefixes (prefix_map& m, const target& t, const variable& var) const
    {
      tracer trace (x, "compile::append_prefixes");

      // If this target does not belong to any project (e.g, an "imported as
      // installed" library), then it can't possibly generate any headers for
      // us.
      //
      const scope* rs (t.base_scope ().root_scope ());
      if (rs == nullptr)
        return;

      const dir_path& out_base (t.dir);
      const dir_path& out_root (rs->out_path ());

      if (auto l = t[var])
      {
        const auto& v (cast<strings> (l));

        for (auto i (v.begin ()), e (v.end ()); i != e; ++i)
        {
          // -I can either be in the "-Ifoo" or "-I foo" form. For VC it can
          // also be /I.
          //
          const string& o (*i);

          if (o.size () < 2 || (o[0] != '-' && o[0] != '/') || o[1] != 'I')
            continue;

          dir_path d;
          if (o.size () == 2)
          {
            if (++i == e)
              break; // Let the compiler complain.

            d = dir_path (*i);
          }
          else
            d = dir_path (*i, 2, string::npos);

          l6 ([&]{trace << "-I '" << d << "'";});

          if (d.relative ())
            fail << "relative -I directory " << d
                 << " in variable " << var.name
                 << " for target " << t;

          // If we are not inside our project root, then ignore.
          //
          if (!d.sub (out_root))
            continue;

          // If the target directory is a sub-directory of the include
          // directory, then the prefix is the difference between the
          // two. Otherwise, leave it empty.
          //
          // The idea here is to make this "canonical" setup work auto-
          // magically:
          //
          // 1. We include all files with a prefix, e.g., <foo/bar>.
          // 2. The library target is in the foo/ sub-directory, e.g.,
          //    /tmp/foo/.
          // 3. The poptions variable contains -I/tmp.
          //
          dir_path p (out_base.sub (d) ? out_base.leaf (d) : dir_path ());

          auto j (m.find (p));

          if (j != m.end ())
          {
            if (j->second != d)
            {
              // We used to reject duplicates but it seems this can be
              // reasonably expected to work according to the order of the -I
              // options.
              //
              // Seeing that we normally have more "specific" -I paths first,
              // (so that we don't pick up installed headers, etc), we ignore
              // it.
              //
              if (verb >= 4)
                trace << "ignoring dependency prefix '" << p << "'\n"
                      << "  existing mapping to " << j->second << "\n"
                      << "  another mapping to  " << d;
            }
          }
          else
          {
            l6 ([&]{trace << "'" << p << "' = '" << d << "'";});
            m.emplace (move (p), move (d));
          }
        }
      }
    }

    auto compile::
    build_prefix_map (const scope& bs,
                      target& t,
                      action act, lorder lo) const -> prefix_map
    {
      prefix_map m;

      // First process our own.
      //
      append_prefixes (m, t, c_poptions);
      append_prefixes (m, t, x_poptions);

      // Then process the include directories from prerequisite libraries.
      //
      append_lib_prefixes (bs, m, t, act, lo);

      return m;
    }

    // Return the next make prerequisite starting from the specified
    // position and update position to point to the start of the
    // following prerequisite or l.size() if there are none left.
    //
    static string
    next_make (const string& l, size_t& p)
    {
      size_t n (l.size ());

      // Skip leading spaces.
      //
      for (; p != n && l[p] == ' '; p++) ;

      // Lines containing multiple prerequisites are 80 characters max.
      //
      string r;
      r.reserve (n);

      // Scan the next prerequisite while watching out for escape sequences.
      //
      for (; p != n && l[p] != ' '; p++)
      {
        char c (l[p]);

        if (p + 1 != n)
        {
          if (c == '$')
          {
            // Got to be another (escaped) '$'.
            //
            if (l[p + 1] == '$')
              ++p;
          }
          else if (c == '\\')
          {
            // This may or may not be an escape sequence depending on whether
            // what follows is "escapable".
            //
            switch (c = l[++p])
            {
            case '\\': break;
            case ' ': break;
            default: c = '\\'; --p; // Restore.
            }
          }
        }

        r += c;
      }

      // Skip trailing spaces.
      //
      for (; p != n && l[p] == ' '; p++) ;

      // Skip final '\'.
      //
      if (p == n - 1 && l[p] == '\\')
        p++;

      return r;
    }

    // Extract the include path from the VC /showIncludes output line. Return
    // empty string if the line is not an include note or include error. Set
    // the good_error flag if it is an include error (which means the process
    // will terminate with the error status that needs to be ignored).
    //
    static string
    next_show (const string& l, bool& good_error)
    {
      // The include error should be the last line that we handle.
      //
      assert (!good_error);

      // VC /showIncludes output. The first line is the file being compiled
      // (handled by our caller). Then we have the list of headers, one per
      // line, in this form (text can presumably be translated):
      //
      // Note: including file: C:\Program Files (x86)\[...]\iostream
      //
      // Finally, if we hit a non-existent header, then we end with an error
      // line in this form:
      //
      // x.cpp(3): fatal error C1083: Cannot open include file: 'd/h.hpp':
      // No such file or directory
      //

      // Distinguishing between the include note and the include error is
      // easy: we can just check for C1083. Distinguising between the note and
      // other errors/warnings is harder: an error could very well end with
      // what looks like a path so we cannot look for the note but rather have
      // to look for an error. Here we assume that a line containing ' CNNNN:'
      // is an error. Should be robust enough in the face of language
      // translation, etc.
      //
      size_t p (l.find (':'));
      size_t n (l.size ());

      for (; p != string::npos; p = ++p != n ? l.find (':', p) : string::npos)
      {
        auto isnum = [](char c) {return c >= '0' && c <= '9';};

        if (p > 5 &&
            l[p - 6] == ' '  &&
            l[p - 5] == 'C'  &&
            isnum (l[p - 4]) &&
            isnum (l[p - 3]) &&
            isnum (l[p - 2]) &&
            isnum (l[p - 1]))
        {
          p -= 4; // Start of the error code.
          break;
        }
      }

      if (p == string::npos)
      {
        // Include note. We assume the path is always at the end but need to
        // handle both absolute Windows and POSIX ones.
        //
        // Note that VC appears to always write the absolute path to the
        // included file even if it is ""-included and the source path is
        // relative. Aren't we lucky today?
        //
        size_t p (l.rfind (':'));

        if (p != string::npos)
        {
          // See if this one is part of the Windows drive letter.
          //
          if (p > 1 && p + 1 < n && // 2 chars before, 1 after.
              l[p - 2] == ' ' &&
              alpha (l[p - 1]) &&
              path::traits::is_separator (l[p + 1]))
            p = l.rfind (':', p - 2);
        }

        if (p != string::npos)
        {
          // VC uses indentation to indicate the include nesting so there
          // could be any number of spaces after ':'. Skip them.
          //
          p = l.find_first_not_of (' ', p + 1);
        }

        if (p == string::npos)
          fail << "unable to parse /showIncludes include note line";

        return string (l, p);
      }
      else if (l.compare (p, 4, "1083") == 0)
      {
        // Include error. The path is conveniently quoted with ''.
        //
        size_t p2 (l.rfind ('\''));

        if (p2 != string::npos && p2 != 0)
        {
          size_t p1 (l.rfind ('\'', p2 - 1));

          if (p1 != string::npos)
          {
            good_error = true;
            return string (l, p1 + 1 , p2 - p1 - 1);
          }
        }

        fail << "unable to parse /showIncludes include error line" << endf;
      }
      else
      {
        // Some other error.
        //
        return string ();
      }
    }

    // Extract and inject header dependencies. Return the preprocessed source
    // file as well as an indication if it is usable for compilation (see
    // below for details).
    //
    pair<auto_rmfile, bool> compile::
    extract_headers (action act,
                     file& t,
                     lorder lo,
                     const file& src,
                     const match_data& md,
                     depdb& dd,
                     bool& updating) const
    {
      tracer trace (x, "compile::extract_headers");

      l5 ([&]{trace << "target: " << t;});

      auto_rmfile psrc;
      bool puse (true);

      // If things go wrong (and they often do in this area), give the user a
      // bit extra context.
      //
      auto df = make_diag_frame (
        [&src](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while extracting header dependencies from " << src;
        });

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      // Preprocess mode that preserves as much information as possible while
      // still performing inclusions. Also serves as a flag indicating whether
      // this compiler uses the separate preprocess and compile setup.
      //
      const char* pp (nullptr);

      switch (cid)
      {
      case compiler_id::gcc:
        {
          // -fdirectives-only is available since GCC 4.3.0.
          //
          if (cmaj > 4 || (cmaj == 4 && cmin >= 3))
            pp = "-fdirectives-only";

          break;
        }
      case compiler_id::clang:
        {
          // -frewrite-includes is available since vanilla Clang 3.2.0.
          //
          // Apple Clang 5.0 is based on LLVM 3.3svn so it should have this
          // option (4.2 is based on 3.2svc so it may or may not have it and,
          // no, we are not going to try to find out).
          //
          if (cvar == ""      ? (cmaj > 3 || (cmaj == 3 && cmin >= 2)) :
              cvar == "apple" ? (cmaj >= 5) : false)
            pp = "-frewrite-includes";

          break;
        }
      case compiler_id::msvc:
        {
          pp = "/C";
          break;
        }
      case compiler_id::icc:
        break;
      }

      // Initialize lazily, only if required.
      //
      cstrings args;
      string out; // Storage.
      path rels;

      // Some compilers in certain modes (e.g., when also producing the
      // preprocessed output) are incapable of writing the dependecy
      // information to stdout. In this case we use a temporary file.
      //
      auto_rmfile drm;

      // Here is the problem: neither GCC nor Clang allow -MG (treat missing
      // header as generated) when we produce any kind of other output (-MD).
      // And that's probably for the best since otherwise the semantics gets
      // pretty hairy (e.g., what is the exit code and state of the output)?
      //
      // One thing to note about generated headers: if we detect one, then,
      // after generating it, we re-run the compiler since we need to get
      // this header's dependencies.
      //
      // So this is how we are going to work around this problem: we first run
      // with -E but without -MG. If there are any errors (maybe because of
      // generated headers maybe not), we restart with -MG and without -E. If
      // this fixes the error (so it was a generate header after all), then we
      // have to restart at which point we go back to -E and no -MG. And we
      // keep yo-yoing like this. Missing generated headers will probably be
      // fairly rare occurrence so this shouldn't be too expensive.
      //
      bool args_gen; // Current state of args.
      size_t args_i; // Start of the -M/-MD "tail".

      // Ok, all good then? Not so fast, the rabbit hole is deeper than it
      // seems: When we run with -E we have to discard diagnostics. This is
      // not a problem for errors since they will be shown on the re-run but
      // it is for (preprocessor) warnings.
      //
      // Clang's -frewrite-includes is nice in that it preserves the warnings
      // so they will be shown during the compilation of the preprocessed
      // source. They are also shown during -E but that we discard. And unlike
      // GCC, in Clang -M does not imply -w (disable warnings) so it would
      // have been shown in -M -MG re-runs but we suppress that with explicit
      // -w. All is good in the Clang land then (even -Werror works nicely).
      //
      // GCC's -fdirective-only, on the other hand, processes all the
      // directives so they are gone from the preprocessed source. Here is
      // what we are going to do to work around this: we will detect if any
      // diagnostics has been written to stderr on the -E run. If that's the
      // case (but the compiler indicated success) then we assume they are
      // warnings and disable the use of the preprocessed output for
      // compilation. This in turn will result in compilation from source
      // which will display the warnings. Note that we may still use the
      // preprocessed output for other things (e.g., C++ module dependency
      // discovery). BTW, another option would be to collect all the
      // diagnostics and then dump it if the run is successful, similar to
      // the VC semantics (and drawbacks) described below.
      //
      // Finally, for VC, things are completely different: there is no -MG
      // equivalent and we handle generated headers by analyzing the
      // diagnostics. This means that unlike in the above two cases, the
      // preprocessor warnings are shown during dependency extraction, not
      // compilation. Not ideal but that's the best we can do. Or is it -- we
      // could implement ad hoc diagnostics sensing... It appears warnings are
      // in the C4000-C4999 code range though there can also be note lines
      // which don't have any C-code.
      //
      // BTW, triggering a warning in the VC preprocessor is not easy; there
      // is no #warning and pragmas are passed through to the compiler. One
      // way to do it is to redefine a macro, for example:
      //
      // hello.cxx(4): warning C4005: 'FOO': macro redefinition
      // hello.cxx(3): note: see previous definition of 'FOO'
      //
      // So seeing that it is hard to trigger a legitimate VC preprocessor
      // warning, for now, we will just treat them as errors by adding /WX.
      //
      // Note: diagnostics sensing is currently only supported if dependency
      // info is written to a file (see above).
      //
      bool sense_diag (false);

      // The gen argument to init_args() is in/out. The caller signals whether
      // to force the generated header support and on return it signals
      // whether this support is enabled. The first call to init_args is
      // expected to have gen false.
      //
      // Return NULL if the dependency information goes to stdout and a
      // pointer to the temporary file path otherwise.
      //
      auto init_args = [&t, act, lo,
                        &src, &md, &rels, &psrc, &sense_diag,
                        &rs, &bs,
                        pp, &args, &args_gen, &args_i, &out, &drm, this]
        (bool& gen) -> const path*
      {
        const path* r (nullptr);

        if (args.empty ())
        {
          assert (!gen);

          // We use absolute/relative paths in the dependency output to
          // distinguish existing headers from (missing) generated. Which
          // means we have to (a) use absolute paths in -I and (b) pass
          // absolute source path (for ""-includes). That (b) is a problem:
          // if we use an absolute path, then all the #line directives will be
          // absolute and all the diagnostics will have long, noisy paths.
          //
          // To work around this we are going to pass a relative path to the
          // source file and then check every relative path in the dependency
          // output for existence in the source file's directory. This is not
          // without issues: it is theoretically possible for a generated
          // header that is <>-included and found via -I to exist in the
          // source file's directory. Note, however, that this is a lot more
          // likely to happen with prefix-less inclusion (e.g., <foo>) and in
          // this case we assume the file is in the project anyway. And if
          // there is a conflict with a prefixed include (e.g., <bar/foo>),
          // then, well, we will just have to get rid of quoted includes
          // (which are generally a bad idea, anyway).
          //
          // Note that we detect and diagnose relative -I directories lazily
          // when building the include prefix map.
          //
          rels = relative (src.path ());

          args.push_back (cpath.recall_string ());

          // Add *.export.poptions from prerequisite libraries.
          //
          append_lib_options (bs, args, t, act, lo);

          append_options (args, t, c_poptions);
          append_options (args, t, x_poptions);

          // Extra system header dirs (last).
          //
          for (const dir_path& d: sys_inc_dirs)
          {
            args.push_back ("-I");
            args.push_back (d.string ().c_str ());
          }

          // Some compile options (e.g., -std, -m) affect the preprocessor.
          //
          // Currently Clang supports importing "header modules" even when in
          // the TS mode. And "header modules" support macros which means
          // imports have to be resolved during preprocessing. Which poses a
          // bit of a chicken and egg problem for us. For now, the workaround
          // is to remove the -fmodules-ts option when preprocessing. Hopefully
          // there will be a "pure modules" mode at some point.
          //
          append_options (args, t, c_coptions);
          append_options (args, t, x_coptions);
          append_options (args, tstd,
                          tstd.size () -
                          (modules && cid == compiler_id::clang ? 1 : 0));

          if (cid == compiler_id::msvc)
          {
            assert (pp != nullptr);

            args.push_back ("/nologo");

            // See perform_update() for details on overriding the default
            // exceptions and runtime.
            //
            if (x_lang == lang::cxx && !find_option_prefix ("/EH", args))
              args.push_back ("/EHsc");

            if (!find_option_prefixes ({"/MD", "/MT"}, args))
              args.push_back ("/MD");

            args.push_back ("/P");            // Preprocess to file.
            args.push_back ("/showIncludes"); // Goes to stdout (with diag).
            args.push_back (pp);              // /C (preserve comments).
            args.push_back ("/WX");           // Warning as error (see above).

            psrc = auto_rmfile (t.path () + x_pext);

            if (cast<uint64_t> (rs[x_version_major]) >= 18)
            {
              args.push_back ("/Fi:");
              args.push_back (psrc.path ().string ().c_str ());
            }
            else
            {
              out = "/Fi" + psrc.path ().string ();
              args.push_back (out.c_str ());
            }

            args.push_back (langopt (md)); // Compile as.
            gen = args_gen = true;
          }
          else
          {
            if (t.is_a<objs> ())
            {
              // On Darwin, Win32 -fPIC is the default.
              //
              if (tclass == "linux" || tclass == "bsd")
                args.push_back ("-fPIC");
            }

            // Depending on the compiler, decide whether (and how) we can
            // produce preprocessed output as a side effect of dependency
            // extraction.
            //
            // Note: -MM -MG skips missing <>-included.

            // Clang's -M does not imply -w (disable warnings). We also don't
            // need them in the -MD case (see above) so disable for both.
            //
            if (cid == compiler_id::clang)
              args.push_back ("-w");

            // Previously we used '*' as a target name but it gets expanded to
            // the current directory file names by GCC (4.9) that comes with
            // MSYS2 (2.4). Yes, this is the (bizarre) behavior of GCC being
            // executed in the shell with -MQ '*' option and not just -MQ *.
            //
            args.push_back ("-MQ"); // Quoted target name.
            args.push_back ("^");   // Old versions can't do empty target name.

            args.push_back ("-x");
            args.push_back (langopt (md));

            if (pp != nullptr)
            {
              // Note that the options are carefully laid out to be easy to
              // override (see below).
              //
              args_i = args.size ();

              args.push_back ("-MD");
              args.push_back ("-E");
              args.push_back (pp);

              // Dependency output.
              //
              args.push_back ("-MF");

              // GCC is not capable of writing the dependency info to stdout.
              // We also need to sense the diagnostics on the -E runs.
              //
              if (cid == compiler_id::gcc)
              {
                // Use the .t extension (for "temporary"; .d is taken).
                //
                r = &(drm = auto_rmfile (t.path () + ".t")).path ();
                args.push_back (drm.path ().string ().c_str ());

                sense_diag = true;
              }
              else
                args.push_back ("-");

              // Preprocessor output.
              //
              psrc = auto_rmfile (t.path () + x_pext);
              args.push_back ("-o");
              args.push_back (psrc.path ().string ().c_str ());
            }
            else
            {
              args.push_back ("-M");
              args.push_back ("-MG"); // Treat missing headers as generated.
            }

            gen = args_gen = (pp == nullptr);
          }

          args.push_back (rels.string ().c_str ());
          args.push_back (nullptr);
        }
        else
        {
          assert (gen != args_gen);

          size_t i (args_i);

          if (gen)
          {
            // Overwrite.
            //
            args[i]     = "-M";
            args[i + 1] = "-MG";
            args[i + 2] = rels.string ().c_str ();
            args[i + 3] = nullptr;

            if (cid == compiler_id::gcc)
            {
              sense_diag = false;
            }
          }
          else
          {
            // Restore.
            //
            args[i]     = "-MD";
            args[i + 1] = "-E";
            args[i + 2] = pp;
            args[i + 3] = "-MF";

            if (cid == compiler_id::gcc)
            {
              r = &drm.path ();
              sense_diag = true;
            }
          }

          args_gen = gen;
        }

        return r;
      };

      // Build the prefix map lazily only if we have non-existent files.
      // Also reuse it over restarts since it doesn't change.
      //
      prefix_map pm;

      // If any prerequisites that we have extracted changed, then we have to
      // redo the whole thing. The reason for this is auto-generated headers:
      // the updated header may now include a yet-non-existent header. Unless
      // we discover this and generate it (which, BTW, will trigger another
      // restart since that header, in turn, can also include auto-generated
      // headers), we will end up with an error during compilation proper.
      //
      // One complication with this restart logic is that we will see a
      // "prefix" of prerequisites that we have already processed (i.e., they
      // are already in our prerequisite_targets list) and we don't want to
      // keep redoing this over and over again. One thing to note, however, is
      // that the prefix that we have seen on the previous run must appear
      // exactly the same in the subsequent run. The reason for this is that
      // none of the files that it can possibly be based on have changed and
      // thus it should be exactly the same. To put it another way, the
      // presence or absence of a file in the dependency output can only
      // depend on the previous files (assuming the compiler outputs them as
      // it encounters them and it is hard to think of a reason why would
      // someone do otherwise). And we have already made sure that all those
      // files are up to date. And here is the way we are going to exploit
      // this: we are going to keep track of how many prerequisites we have
      // processed so far and on restart skip right to the next one.
      //
      // And one more thing: most of the time this list of headers would stay
      // unchanged and extracting them by running the compiler every time is a
      // bit wasteful. So we are going to cache them in the depdb. If the db
      // hasn't been invalidated yet (e.g., because the compiler options have
      // changed), then we start by reading from it. If anything is out of
      // date then we use the same restart and skip logic to switch to the
      // compiler run.
      //

      // Update and add a header file to the list of prerequisite targets.
      // Depending on the cache flag, the file is assumed to either have come
      // from the depdb cache or from the compiler run. Return whether the
      // extraction process should be restarted.
      //
      auto add = [&trace, &pm, act, &t, lo, &dd, &updating, &bs, &rels, this]
        (path f, bool cache) -> bool
      {
        // Find or maybe insert the target.
        //
        auto find = [&trace, &t, this] (
          const path& f, bool insert) -> const path_target*
        {
          // Split the name into its directory part, the name part, and
          // extension. Here we can assume the name part is a valid filesystem
          // name.
          //
          // Note that if the file has no extension, we record an empty
          // extension rather than NULL (which would signify that the default
          // extension should be added).
          //
          dir_path d (f.directory ());
          string n (f.leaf ().base ().string ());
          string e (f.extension ());

          // Determine the target type.
          //
          const target_type* tt (nullptr);

          // See if this directory is part of any project out_root hierarchy.
          // Note that this will miss all the headers that come from src_root
          // (so they will be treated as generic C headers below). Generally,
          // we don't have the ability to determine that some file belongs to
          // src_root of some project. But that's not a problem for our
          // purposes: it is only important for us to accurately determine
          // target types for headers that could be auto-generated.
          //
          // While at it also try to determine if this target is from the src
          // or out tree of said project.
          //
          dir_path out;

          const scope& bs (scopes.find (d));
          if (const scope* rs = bs.root_scope ())
          {
            tt = map_extension (bs, n, e);

            if (bs.out_path () != bs.src_path () && d.sub (bs.src_path ()))
              out = out_src (d, *rs);
          }

          // If it is outside any project, or the project doesn't have such an
          // extension, assume it is a plain old C header.
          //
          if (tt == nullptr)
          {
            // If the project doesn't "know" this extension then we won't
            // possibly find an explicit target of this type.
            //
            if (!insert)
              return nullptr;

            tt = &h::static_type;
          }

          // Find or insert target.
          //
          // @@ OPT: move d, out, n
          //
          const target* r;
          if (insert)
            r = &search (t, *tt, d, out, n, &e, nullptr);
          else
          {
            // Note that we skip any target type-specific searches (like for
            // an existing file) and go straight for the target object since
            // we need to find the target explicitly spelled out.
            //
            r = targets.find (*tt, d, out, n, e, trace);
          }

          return static_cast<const path_target*> (r);
        };

        const path_target* pt (nullptr);

        // If it's not absolute then it either does not (yet) exist or is
        // a relative ""-include (see init_args() for details). Reduce the
        // second case to absolute.
        //
        if (f.relative () && rels.relative ())
        {
          // If the relative source path has a directory component, make sure
          // it matches since ""-include will always start with that (none of
          // the compilers we support try to normalize this path). Failed that
          // we may end up searching for a generated header in a random
          // (working) directory.
          //
          const string& fs (f.string ());
          const string& ss (rels.string ());

          size_t p (path::traits::rfind_separator (ss));

          if (p == string::npos || // No directory.
              (fs.size () > p + 1 &&
               path::traits::compare (fs.c_str (), p, ss.c_str (), p) == 0))
          {
            path t (work / f); // The rels path is relative to work.

            if (exists (t))
              f = move (t);
          }
        }

        // If still relative then it does not exist.
        //
        if (f.relative ())
        {
          f.normalize ();

          // This is probably as often an error as an auto-generated file, so
          // trace at level 4.
          //
          l4 ([&]{trace << "non-existent header '" << f << "'";});

          // If we already did this and build_prefix_map() returned empty,
          // then we would have failed below.
          //
          if (pm.empty ())
            pm = build_prefix_map (bs, t, act, lo);

          // First try the whole file. Then just the directory.
          //
          // @@ Has to be a separate map since the prefix can be
          //    the same as the file name.
          //
          // auto i (pm.find (f));

          // Find the most qualified prefix of which we are a sub-path.
          //
          auto i (pm.end ());

          if (!pm.empty ())
          {
            const dir_path& d (f.directory ());
            i = pm.upper_bound (d);

            // Get the greatest less than, if any. We might still not be a
            // sub. Note also that we still have to check the last element if
            // upper_bound() returned end().
            //
            if (i == pm.begin () || !d.sub ((--i)->first))
              i = pm.end ();
          }

          if (i != pm.end ())
          {
            // If this is a prefixless mapping, then only use it if we can
            // resolve it to an existing target (i.e., it is explicitly
            // spelled out in a buildfile).
            //
            // Note that at some point we will probably have a list of
            // directories.
            //
            if (i->first.empty ())
            {
              path p (i->second / f);
              l4 ([&]{trace << "trying as auto-generated " << p;});
              pt = find (p, false);
              if (pt != nullptr)
                f = move (p);
            }
            else
            {
              f = i->second / f;
              l4 ([&]{trace << "mapped as auto-generated " << f;});
              pt = find (f, true);
            }
          }

          if (pt == nullptr)
            fail << "header '" << f << "' not found and cannot be generated";
        }
        else
        {
          // We used to just normalize the path but that could result in an
          // invalid path (e.g., on CentOS 7 with Clang 3.4) because of the
          // symlinks. So now we realize (i.e., realpath(3)) it instead. If
          // it comes from the depdb, in which case we've already done that.
          //
          if (!cache)
            f.realize ();

          l6 ([&]{trace << "injecting " << f;});
          pt = find (f, true);
        }

        // Cache the path.
        //
        const path& pp (pt->path (move (f)));

        // Match to a rule.
        //
        build2::match (act, *pt);

        // Update.
        //
        // If this header came from the depdb, make sure it is no older than
        // the db itself (if it has changed since the db was written, then
        // chances are the cached data is stale).
        //
        bool restart (
          update (
            trace, act, *pt, cache ? dd.mtime () : timestamp_unknown));

        updating = updating || restart;

        // Verify/add it to the dependency database. We do it after update in
        // order not to add bogus files (non-existent and without a way to
        // update).
        //
        if (!cache)
          dd.expect (pp);

        // Add to our prerequisite target list.
        //
        t.prerequisite_targets.push_back (pt);

        return restart;
      };

      // If nothing so far has invalidated the dependency database, then try
      // the cached data before running the compiler.
      //
      bool cache (!updating);

      // See init_args() above for details on generated header support.
      //
      bool gen (false);
      optional<bool> force_gen;

      const path* drmp (nullptr); // Points to drm.path () if active.

      size_t skip_count (0);
      for (bool restart (true); restart; cache = false)
      {
        restart = false;

        if (cache)
        {
          // If any, this is always the first run.
          //
          assert (skip_count == 0);

          while (dd.more ())
          {
            string* l (dd.read ());

            // If the line is invalid, run the compiler.
            //
            if (l == nullptr)
            {
              restart = true;
              break;
            }

            restart = add (path (move (*l)), true);
            skip_count++;

            if (restart)
            {
              l6 ([&]{trace << "restarting";});
              break;
            }
          }
        }
        else
        {
          try
          {
            if (force_gen)
              gen = *force_gen;

            if (args.empty () || gen != args_gen)
              drmp = init_args (gen);

            if (verb >= 3)
              print_process (args.data ()); // Disable pipe mode.

            process pr;

            try
            {
              // Assume the preprocessed output (if produced) is usable
              // until proven otherwise.
              //
              puse = true;

              // If we have no generated header support, then suppress all
              // diagnostics (if things go badly we will restart with this
              // support).
              //
              if (drmp == nullptr)
              {
                // Dependency info goes to stdout.
                //
                assert (!sense_diag);

                // For VC with /P the dependency info and diagnostics all go
                // to stderr so redirect it to stdout.
                //
                pr = process (cpath,
                              args.data (),
                              0,
                              -1,
                              cid == compiler_id::msvc ? 1 : gen ? 2 : -2);
              }
              else
              {
                // Dependency info goes to a temporary file.
                //
                pr = process (cpath,
                              args.data (),
                              0,
                              2, // Send stdout to stderr.
                              gen ? 2 : sense_diag ? -1 : -2);

                // If requested, monitor for diagnostics and if detected, mark
                // the preprocessed output as unusable for compilation.
                //
                if (sense_diag)
                {
                  ifdstream is (move (pr.in_efd), fdstream_mode::skip);
                  puse = puse && (is.peek () == ifdstream::traits_type::eof ());
                  is.close ();
                }

                // The idea is to reduce it to the stdout case.
                //
                pr.wait ();
                pr.in_ofd = fdopen (*drmp, fdopen_mode::in);
              }

              // We may not read all the output (e.g., due to a restart).
              // Before we used to just close the file descriptor to signal to
              // the other end that we are not interested in the rest. This
              // works fine with GCC but Clang (3.7.0) finds this impolite and
              // complains, loudly (broken pipe). So now we are going to skip
              // until the end.
              //
              ifdstream is (move (pr.in_ofd),
                            fdstream_mode::text | fdstream_mode::skip,
                            ifdstream::badbit);

              // In some cases we may need to ignore the error return status.
              // The good_error flag keeps track of that. Similarly we
              // sometimes expect the error return status based on the output
              // we see. The bad_error flag is for that.
              //
              bool good_error (false), bad_error (false);

              size_t skip (skip_count);
              for (bool first (true), second (false); !(restart || is.eof ());)
              {
                string l;
                getline (is, l);

                if (is.fail ())
                {
                  if (is.eof ()) // Trailing newline.
                    break;

                  throw io_error ("");
                }

                l6 ([&]{trace << "header dependency line '" << l << "'";});

                // Parse different dependency output formats.
                //
                if (cid == compiler_id::msvc)
                {
                  if (first)
                  {
                    // The first line should be the file we are compiling. If
                    // it is not, then something went wrong even before we
                    // could compile anything (e.g., file does not exist). In
                    // this case the first line (and everything after it) is
                    // presumably diagnostics.
                    //
                    if (l != rels.leaf ().string ())
                    {
                      text << l;
                      bad_error = true;
                      break;
                    }

                    first = false;
                    continue;
                  }

                  string f (next_show (l, good_error));

                  if (f.empty ()) // Some other diagnostics.
                  {
                    text << l;
                    bad_error = true;
                    break;
                  }

                  // Skip until where we left off.
                  //
                  if (skip != 0)
                  {
                    // We can't be skipping over a non-existent header.
                    //
                    assert (!good_error);
                    skip--;
                  }
                  else
                  {
                    restart = add (path (move (f)), false);
                    skip_count++;

                    // If the header does not exist (good_error) then restart
                    // must be true. Except that it is possible that someone
                    // running in parallel has already updated it. In this
                    // case we must force a restart since we haven't yet seen
                    // what's after this at-that-time-non-existent header.
                    //
                    // We also need to force the target update (normally done
                    // by add()).
                    //
                    if (good_error)
                      restart = updating = true;

                    if (restart)
                      l6 ([&]{trace << "restarting";});
                  }
                }
                else
                {
                  // Make dependency declaration.
                  //
                  size_t pos (0);

                  if (first)
                  {
                    // Empty output should mean the wait() call below will
                    // return false.
                    //
                    if (l.empty ())
                    {
                      bad_error = true;
                      break;
                    }

                    assert (l[0] == '^' && l[1] == ':' && l[2] == ' ');

                    first = false;
                    second = true;

                    // While normally we would have the source file on the
                    // first line, if too long, it will be moved to the next
                    // line and all we will have on this line is "^: \".
                    //
                    if (l.size () == 4 && l[3] == '\\')
                      continue;
                    else
                      pos = 3; // Skip "^: ".

                    // Fall through to the 'second' block.
                  }

                  if (second)
                  {
                    second = false;
                    next_make (l, pos); // Skip the source file.
                  }

                  while (pos != l.size ())
                  {
                    string f (next_make (l, pos));

                    // Skip until where we left off.
                    //
                    if (skip != 0)
                    {
                      skip--;
                      continue;
                    }

                    restart = add (path (move (f)), false);
                    skip_count++;

                    if (restart)
                    {
                      l6 ([&]{trace << "restarting";});
                      break;
                    }
                  }
                }
              }

              // In case of VC, we are parsing stderr and if things go south,
              // we need to copy the diagnostics for the user to see.
              //
              // Note that the eof check is important: if the stream is at
              // eof, this and all subsequent writes to cerr will fail (and
              // you won't see a thing).
              //
              if (bad_error                                 &&
                  cid == compiler_id::msvc                  &&
                  is.peek () != ifdstream::traits_type::eof ())
                cerr << is.rdbuf ();

              is.close ();

              // This is tricky: it is possible that in parallel someone has
              // generated all our missing headers and we wouldn't restart
              // normally.
              //
              // In this case we also need to force the target update
              // (normally done by add()).
              //
              if (force_gen && *force_gen)
              {
                restart = updating = true;
                force_gen = false;
              }

              if (pr.wait ())
              {
                if (!bad_error)
                  continue;

                fail << "expected error exist status from " << x_lang
                     << " compiler";
              }
              else if (pr.exit->normal ())
              {
                if (good_error) // Ignore expected errors (restart).
                  continue;
              }

              // Fall through.
            }
            catch (const io_error&)
            {
              if (pr.wait ())
                fail << "unable to read " << x_lang << " compiler header "
                     << "dependency output";

              // Fall through.
            }

            assert (pr.exit && !*pr.exit);
            const process_exit& e (*pr.exit);

            // For normal exit we assume the child process issued some
            // diagnostics.
            //
            if (e.normal ())
            {
              // If this run was without the generated header support, force
              // it and restart.
              //
              if (!gen)
              {
                restart = true;
                force_gen = true;
                l6 ([&]{trace << "restarting with forced generated headers";});
                continue;
              }

              throw failed ();
            }
            else
              fail << args[0] << " terminated abnormally: " << e.description ();
          }
          catch (const process_error& e)
          {
            error << "unable to execute " << args[0] << ": " << e;

            // In a multi-threaded program that fork()'ed but did not exec(),
            // it is unwise to try to do any kind of cleanup (like unwinding
            // the stack and running destructors).
            //
            if (e.child)
            {
              drm.cancel ();
              exit (1);
            }

            throw failed ();
          }
        }
      }

      puse = puse && !psrc.path ().empty ();
      return make_pair (move (psrc), puse);
    }

    // Extract and inject module dependencies.
    //
    void compile::
    extract_modules (action act,
                     file& t,
                     lorder lo,
                     const file& src,
                     auto_rmfile& psrc,
                     const match_data& md,
                     depdb& /*dd*/,
                     bool& /*updating*/) const
    {
      tracer trace (x, "compile::extract_modules");

      l5 ([&]{trace << "target: " << t;});

      // If things go wrong (and they often do in this area), give the user a
      // bit extra context.
      //
      auto df = make_diag_frame (
        [&src](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while extracting module dependencies from " << src;
        });

      // For some compilers (GCC, Clang) the preporcessed output is only
      // partially preprocessed. For others (VC), it is already fully
      // preprocessed (well, almost: it still has comments but we can handle
      // that). Plus, the source file might already be (sufficiently)
      // preprocessed.
      //
      // So the plan is to start the compiler process that writes the fully
      // preprocessed output to stdout and reduce the already preprocessed
      // case to it.
      //
      cstrings args;
      path rels;

      bool ps; // True if extracting from psrc.
      if (md.pp < preprocessed::modules)
      {
        ps = !psrc.path ().empty ();
        rels = relative (ps ? psrc.path () : src.path ());

        // VC's preprocessed output, if present, is fully preprocessed.
        //
        if (cid != compiler_id::msvc || !ps)
        {
          // This should match with how we setup preprocessing and is pretty
          // similar to init_args() from extract_headers().
          //
          const scope& bs (t.base_scope ());

          args.push_back (cpath.recall_string ());

          append_lib_options (bs, args, t, act, lo);

          append_options (args, t, c_poptions);
          append_options (args, t, x_poptions);

          for (const dir_path& d: sys_inc_dirs)
          {
            args.push_back ("-I");
            args.push_back (d.string ().c_str ());
          }

          append_options (args, t, c_coptions);
          append_options (args, t, x_coptions);
          append_options (args, tstd,
                          tstd.size () -
                          (modules && cid == compiler_id::clang ? 1 : 0));

          if (cid == compiler_id::msvc)
          {
            args.push_back ("/nologo");

            if (x_lang == lang::cxx && !find_option_prefix ("/EH", args))
              args.push_back ("/EHsc");

            if (!find_option_prefixes ({"/MD", "/MT"}, args))
              args.push_back ("/MD");

            args.push_back ("/E");
            args.push_back ("/C");
            args.push_back (langopt (md)); // Compile as.
          }
          else
          {
            if (t.is_a<objs> ())
            {
              if (tclass == "linux" || tclass == "bsd")
                args.push_back ("-fPIC");
            }

            // Options that trigger preprocessing of partially preprocessed
            // output are a bit of a compiler-specific voodoo.
            //
            args.push_back ("-E");

            if (ps)
            {
              args.push_back ("-x");
              args.push_back (langopt (md));

              if (cid == compiler_id::gcc)
              {
                args.push_back ("-fpreprocessed");
                args.push_back ("-fdirectives-only");
              }
            }
          }

          args.push_back (rels.string ().c_str ());
          args.push_back (nullptr);
        }
      }
      else
      {
        // Extracting directly from source.
        //
        ps = false;
        rels = relative (src.path ());
      }

      // Preprocess and parse.
      //
      translation_unit tu;

      for (;;) // Breakout loop.
      try
      {
        // Disarm the removal of the preprocessed file in case of an error.
        // We re-arm it below.
        //
        if (ps)
          psrc.cancel ();

        process pr;

        try
        {
          if (args.empty ())
          {
            pr = process (process_exit (0)); // Successfully exited.
            pr.in_ofd = fdopen (rels, fdopen_mode::in);
          }
          else
          {
            if (verb >= 3)
              print_process (args);

            // We don't want to see warnings multiple times so ignore all
            // diagnostics.
            //
            pr = process (cpath, args.data (), 0, -1, -2);
          }

          ifdstream is (move (pr.in_ofd),
                        fdstream_mode::text | fdstream_mode::skip);

          parser p;
          tu = p.parse (is, rels);

          is.close ();

          if (pr.wait ())
          {
            if (ps)
              psrc = auto_rmfile (move (rels)); // Re-arm.

            break;
          }

          // Fall through.
        }
        catch (const io_error&)
        {
          if (pr.wait ())
            fail << "unable to read " << x_lang << " preprocessor output";

          // Fall through.
        }

        assert (pr.exit && !*pr.exit);
        const process_exit& e (*pr.exit);

        // What should we do with a normal error exit? Remember we suppressed
        // the compiler's diagnostics. Let's issue a warning and continue with
        // the assumption that the compilation step fails with diagnostics.
        //
        if (e.normal ())
        {
          warn << "unable to extract module dependency information from "
               << src;
          return;
        }
        else
          fail << args[0] << " terminated abnormally: " << e.description ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child)
          exit (1);

        throw failed ();
      }

      // Sanity checks.
      //
      if (modules)
      {
        // If we are compiling a module interface unit, make sure it has the
        // necessary declarations.
        //
        if (src.is_a (*x_mod))
        {
          // VC is not (yet) using the 'export module' syntax so use the
          // preprequisite type to distinguish between interface and
          // implementation units.
          //
          if (cid == compiler_id::msvc)
            tu.module_interface = true;

          if (tu.module_name.empty () || !tu.module_interface)
            fail << src << " is not a module interface unit";
        }
      }

      if (tu.module_name.empty () && tu.module_imports.empty ())
        return;

      // Modules are used by this translation unit. Make sure module support
      // is enabled.
      //
      if (!modules)
        fail << "modules support not enabled or unavailable";

      // Set the cc.module_name variable if this is an interface unit. If we
      // have the bmi{} group, set it there (in which case we have to lock).
      //
      if (tu.module_interface)
      {
        target_lock l;
        target* x (t.group == nullptr
                   ? &t
                   : (l = lock (act, *t.group)).target);
        assert (x != nullptr); // Should be lockable.

        if (value& v = x->vars.assign (c_module_name))
          assert (cast<string> (v) == tu.module_name);
        else
          v = move (tu.module_name); // Note: move.
      }
    }

    // Resolve imported modules to bmi*{} targets.
    //
    void compile::
    search_modules (const scope& /*bs*/,
                    action act,
                    file& t,
                    lorder /*lo*/,
                    const target_type& mtt) const
    {
      auto& pts (t.prerequisite_targets);
      size_t start (pts.size ());         // Index of the first to be added.

      for (prerequisite_member p: group_prerequisite_members (act, t))
      {
        const target* pt (nullptr);

        if (p.is_a<bmi> ())
          pt = &search (t, mtt, p.key ()); //@@ MOD: fuzzy...
        else if (p.is_a (mtt))
          pt = &p.search (t);

        if (pt != nullptr)
          pts.push_back (pt);
      }

      // Match in parallel and wait for completion.
      //
      match_members (act, t, pts, start);
    }

    // Filter cl.exe noise (msvc.cxx).
    //
    void
    msvc_filter_cl (ifdstream&, const path& src);

    void compile::
    append_modules (cstrings& args, strings& stor, const file& t) const
    {
      for (const target* pt: t.prerequisite_targets)
      {
        // Here we use whatever bmi type has been added.
        //
        const file* f;
        if ((f = pt->is_a<bmie> ()) == nullptr &&
            (f = pt->is_a<bmia> ()) == nullptr &&
            (f = pt->is_a<bmis> ()) == nullptr)
          continue;

        string s (relative (f->path ()).string ());

        switch (cid)
        {
        case compiler_id::gcc:
          {
            s.insert (0, 1, '=');
            s.insert (0, cast<string> ((*f)[c_module_name]));
            s.insert (0, "-fmodule-map=");
            break;
          }
        case compiler_id::clang:
          {
            s.insert (0, "-fmodule-file=");
            break;
          }
        case compiler_id::msvc:
          {
            stor.push_back ("/module:reference");
            break;
          }
        case compiler_id::icc:
          assert (false);
        }

        stor.push_back (move (s));
      }

      // Shallow-copy storage to args. Why not do it as we go along pushing
      // into storage?  Because of potential reallocations.
      //
      for (const string& a: stor)
        args.push_back (a.c_str ());
    }

    target_state compile::
    perform_update (action act, const target& xt) const
    {
      const file& t (xt.as<file> ());

      match_data md (move (t.data<match_data> ()));
      bool mod (md.mod);

      // While all our prerequisites are already up-to-date, we still have to
      // execute them to keep the dependency counts straight. Actually, no, we
      // may also have to update the modules.
      //
      auto pr (
        execute_prerequisites<file> (
          (mod ? *x_mod : x_src), act, t, md.mt));

      if (pr.first)
      {
        t.mtime (md.mt);
        return *pr.first;
      }

      const file& s (pr.second);

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      otype ct (compile_type (t, mod));
      lorder lo (link_order (bs, ct));

      cstrings args {cpath.recall_string ()};

      // Translate paths to relative (to working directory) ones. This results
      // in easier to read diagnostics.
      //
      path rels (relative (s.path ()));

      // If we are building a module, then the target is bmi*{} and its ad hoc
      // member is obj*{}.
      //
      path relo, relm;
      if (mod)
      {
        relm = relative (t.path ());
        relo = relative (t.member->is_a<file> ()->path ());
      }
      else
        relo = relative (t.path ());

      // Build the command line.
      //
      if (md.pp != preprocessed::all)
      {
        append_options (args, t, c_poptions);
        append_options (args, t, x_poptions);

        // Add *.export.poptions from prerequisite libraries.
        //
        append_lib_options (bs, args, t, act, lo);

        // Extra system header dirs (last).
        //
        for (const dir_path& d: sys_inc_dirs)
        {
          args.push_back ("-I");
          args.push_back (d.string ().c_str ());
        }
      }

      append_options (args, t, c_coptions);
      append_options (args, t, x_coptions);
      append_options (args, tstd);

      string out, out1; // Output options storage.
      strings mods;     // Module options storage.
      size_t out_i (0); // Index of the -o option.

      if (cid == compiler_id::msvc)
      {
        // The /F*: option variants with separate names only became available
        // in VS2013/12.0. Why do we bother? Because the command line suddenly
        // becomes readable.
        //
        uint64_t ver (cast<uint64_t> (rs[x_version_major]));

        args.push_back ("/nologo");

        // While we want to keep the low-level build as "pure" as possible,
        // the two misguided defaults, exceptions and runtime, just have to be
        // fixed. Otherwise the default build is pretty much unusable. But we
        // also make sure that the user can easily disable our defaults: if we
        // see any relevant options explicitly specified, we take our hands
        // off.
        //
        // For C looks like no /EH* (exceptions supported but no C++ objects
        // destroyed) is a reasonable default.
        //
        if (x_lang == lang::cxx && !find_option_prefix ("/EH", args))
          args.push_back ("/EHsc");

        // The runtime is a bit more interesting. At first it may seem like a
        // good idea to be a bit clever and use the static runtime if we are
        // building obja{}. And for obje{} we could decide which runtime to
        // use based on the library link order: if it is static-only, then we
        // could assume the static runtime. But it is indeed too clever: when
        // building liba{} we have no idea who is going to use it. It could be
        // an exe{} that links both static and shared libraries (and is
        // therefore built with the shared runtime). And to safely use the
        // static runtime, everything must be built with /MT and there should
        // be no DLLs in the picture. So we are going to play it safe and
        // always default to the shared runtime.
        //
        // In a similar vein, it would seem reasonable to use the debug runtime
        // if we are compiling with debug. But, again, there will be fireworks
        // if we have some projects built with debug and some without and then
        // we try to link them together (which is not an unreasonable thing to
        // do). So by default we will always use the release runtime.
        //
        if (!find_option_prefixes ({"/MD", "/MT"}, args))
          args.push_back ("/MD");

        if (modules)
          append_modules (args, mods, t);

        // The presence of /Zi or /ZI causes the compiler to write debug info
        // to the .pdb file. By default it is a shared file called vcNN.pdb
        // (where NN is the VC version) created (wait for it) in the current
        // working directory (and not the directory of the .obj file). Also,
        // because it is shared, there is a special Windows service that
        // serializes access. We, of course, want none of that so we will
        // create a .pdb per object file.
        //
        // Note that this also changes the name of the .idb file (used for
        // minimal rebuild and incremental compilation): cl.exe take the /Fd
        // value and replaces the .pdb extension with .idb.
        //
        // Note also that what we are doing here appears to be incompatible
        // with PCH (/Y* options) and /Gm (minimal rebuild).
        //
        if (find_options ({"/Zi", "/ZI"}, args))
        {
          if (ver >= 18)
            args.push_back ("/Fd:");
          else
            out1 = "/Fd";

          out1 += relo.string ();
          out1 += ".pdb";

          args.push_back (out1.c_str ());
        }

        if (ver >= 18)
        {
          args.push_back ("/Fo:");
          args.push_back (relo.string ().c_str ());
        }
        else
        {
          out = "/Fo" + relo.string ();
          args.push_back (out.c_str ());
        }

        if (mod)
        {
          args.push_back ("/module:interface");
          args.push_back ("/module:output");
          args.push_back (relm.string ().c_str ());
        }

        // Note: no way to indicate that the source if already preprocessed.

        args.push_back ("/c");                    // Compile only.
        args.push_back (langopt (md));            // Compile as.
        args.push_back (rels.string ().c_str ()); // Note: rely on being last.
      }
      else
      {
        if (ct == otype::s)
        {
          // On Darwin, Win32 -fPIC is the default.
          //
          if (tclass == "linux" || tclass == "bsd")
            args.push_back ("-fPIC");
        }

        if (modules)
          append_modules (args, mods, t);

        // Note: the order of the following options is relied upon below.
        //
        out_i = args.size (); // Index of the -o option.

        if (mod)
        {
          switch (cid)
          {
          case compiler_id::gcc:
            {
              args.push_back ("-o");
              args.push_back (relo.string ().c_str ());

              out = "-fmodule-output=";
              out += relm.string ();
              args.push_back (out.c_str ());

              args.push_back ("-c");
              break;
            }
          case compiler_id::clang:
            {
              args.push_back ("-o");
              args.push_back (relm.string ().c_str ());
              args.push_back ("--precompile");

              // These should become the default at some point.
              //
              args.push_back ("-Xclang"); args.push_back ("-fmodules-codegen");
              args.push_back ("-Xclang"); args.push_back ("-fmodules-debuginfo");
              break;
            }
          case compiler_id::msvc:
          case compiler_id::icc:
            assert (false);
          }
        }
        else
        {
          args.push_back ("-o");
          args.push_back (relo.string ().c_str ());
          args.push_back ("-c");
        }

        args.push_back ("-x");
        args.push_back (langopt (md));

        if (md.pp == preprocessed::all)
        {
          // Note that the mode we select must still handle comments and line
          // continuations. So some more compiler-specific voodoo.
          //
          switch (cid)
          {
          case compiler_id::gcc:
            {
              // -fdirectives-only is available since GCC 4.3.0.
              //
              if (cmaj > 4 || (cmaj == 4 && cmin >= 3))
              {
                args.push_back ("-fpreprocessed");
                args.push_back ("-fdirectives-only");
              }
              break;
            }
          case compiler_id::clang:
            {
              // Clang handles comments and line continuations in the
              // preprocessed source (it does not have -fpreprocessed).
              //
              break;
            }
          case compiler_id::icc:
            break; // Compile as normal source for now.
          case compiler_id::msvc:
            assert (false);
          }
        }

        args.push_back (rels.string ().c_str ());
      }

      args.push_back (nullptr);

      // With verbosity level 2 print the command line as if we are compiling
      // the source file, not its preprocessed version (so that it's easy to
      // copy and re-run, etc). Only at level 3 and above print the real deal.
      //
      if (verb == 1)
        text << x_name << ' ' << s;
      else if (verb == 2)
        print_process (args);

      // If we have the (partially) preprocessed output, switch to that.
      //
      bool psrc (!md.psrc.path ().empty ());
      if (psrc)
      {
        args.pop_back (); // nullptr
        args.pop_back (); // rels

        rels = relative (md.psrc.path ());

        // This should match with how we setup preprocessing.
        //
        switch (cid)
        {
        case compiler_id::gcc:
          {
            // The -fpreprocessed is implied by .i/.ii.
            //
            args.pop_back (); // lang()
            args.pop_back (); // -x
            args.push_back ("-fdirectives-only");
            break;
          }
        case compiler_id::clang:
          {
            // Note that without -x Clang will treat .i/.ii as fully
            // preprocessed.
            //
            break;
          }
        case compiler_id::msvc:
          {
            // Nothing to do (/TP or /TC already there).
            //
            break;
          }
        case compiler_id::icc:
          assert (false);
        }

        args.push_back (rels.string ().c_str ());
        args.push_back (nullptr);

        // Let's keep the preprocessed file in case of an error but only at
        // verbosity level 3 and up (when one actually sees it mentioned on
        // the command line). We also have to re-arm on success (see below).
        //
        if (verb >= 3)
          md.psrc.cancel ();
      }

      if (verb >= 3)
        print_process (args);

      try
      {
        // VC cl.exe sends diagnostics to stdout. It also prints the file name
        // being compiled as the first line. So for cl.exe we redirect stdout
        // to a pipe, filter that noise out, and send the rest to stderr.
        //
        // For other compilers redirect stdout to stderr, in case any of them
        // tries to pull off something similar. For sane compilers this should
        // be harmless.
        //
        bool filter (cid == compiler_id::msvc);

        process pr (cpath, args.data (), 0, (filter ? -1 : 2));

        if (filter)
        {
          try
          {
            ifdstream is (
              move (pr.in_ofd), fdstream_mode::text, ifdstream::badbit);

            msvc_filter_cl (is, rels);

            // If anything remains in the stream, send it all to stderr. Note
            // that the eof check is important: if the stream is at eof, this
            // and all subsequent writes to cerr will fail (and you won't see
            // a thing).
            //
            if (is.peek () != ifdstream::traits_type::eof ())
              cerr << is.rdbuf ();

            is.close ();
          }
          catch (const io_error&) {} // Assume exits with error.
        }

        if (!pr.wait ())
          throw failed ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        // In a multi-threaded program that fork()'ed but did not exec(),
        // it is unwise to try to do any kind of cleanup (like unwinding
        // the stack and running destructors).
        //
        if (e.child)
          exit (1);

        throw failed ();
      }

      if (psrc && verb >= 3)
        md.psrc = auto_rmfile (move (rels));

      // Clang's module compilation requires two separate compiler
      // invocations.
      //
      if (mod && cid == compiler_id::clang)
      {
        // Remove the target file if this fails. If we don't do that, we will
        // end up with a broken build that is up-to-date.
        //
        auto_rmfile rm (relm);

        // Adjust the command line. First discard everything after -o then
        // build the new "tail".
        //
        args.resize (out_i + 1);
        args.push_back (relo.string ().c_str ()); // Produce .o.
        args.push_back ("-c");                    // By compiling .pcm.
        args.push_back ("-Wno-unused-command-line-argument"); //@@ MOD (-I).
        args.push_back (relm.string ().c_str ());
        args.push_back (nullptr);

        if (verb >= 2)
          print_process (args);

        try
        {
          process pr (cpath, args.data ());

          if (!pr.wait ())
            throw failed ();
        }
        catch (const process_error& e)
        {
          error << "unable to execute " << args[0] << ": " << e;

          if (e.child)
            exit (1);

          throw failed ();
        }

        rm.cancel ();
      }

      // Should we go to the filesystem and get the new mtime? We know the
      // file has been modified, so instead just use the current clock time.
      // It has the advantage of having the subseconds precision.
      //
      t.mtime (system_clock::now ());
      return target_state::changed;
    }

    target_state compile::
    perform_clean (action a, const target& xt) const
    {
      const file& t (xt.as<file> ());

      using id = compiler_id;

      switch (cid)
      {
      case id::gcc:   return clean_extra (a, t, {".d", x_pext, ".t"});
      case id::clang: return clean_extra (a, t, {".d", x_pext});
      case id::msvc:  return clean_extra (a, t, {".d", x_pext, ".idb", ".pdb"});
      case id::icc:   return clean_extra (a, t, {".d"});
      }

      assert (false);
      return target_state::unchanged;
    }
  }
}
