// file      : build2/cc/compile.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/compile.hxx>

#include <cstdlib>  // exit()
#include <cstring>  // strlen()

#include <build2/depdb.hxx>
#include <build2/scope.hxx>
#include <build2/context.hxx>
#include <build2/variable.hxx>
#include <build2/algorithm.hxx>
#include <build2/diagnostics.hxx>

#include <build2/bin/target.hxx>

#include <build2/cc/parser.hxx>
#include <build2/cc/target.hxx>  // h
#include <build2/cc/module.hxx>
#include <build2/cc/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // module_info string serialization.
    //
    // The string representation is a space-separated list of module names
    // with the following rules:
    //
    // 1. If this is a module interface unit, then the first name is the
    //    module name intself following by either '!' for an interface unit or
    //    by '+' for an implementation unit.
    //
    // 2. If an imported module is re-exported, then the module name is
    //    followed by '*'.
    //
    // For example:
    //
    // foo! foo.core* foo.base* foo.impl
    // foo.base+ foo.impl
    // foo.base foo.impl
    //
    static string
    to_string (const module_info& m)
    {
      string s;

      if (!m.name.empty ())
      {
        s += m.name;
        s += m.iface ? '!' : '+';
      }

      for (const module_import& i: m.imports)
      {
        if (!s.empty ())
          s += ' ';

        s += i.name;

        if (i.exported)
          s += '*';
      }

      return s;
    }

    static module_info
    to_module_info (const string& s)
    {
      module_info m;

      for (size_t b (0), e (0), n; (n = next_word (s, b, e, ' ')) != 0; )
      {
        char c (s[e - 1]);
        switch (c)
        {
        case '!':
        case '+':
        case '*': break;
        default:  c = '\0';
        }

        string w (s, b, n - (c == '\0' ? 0 : 1));

        if (c == '!' || c == '+')
        {
          m.name = move (w);
          m.iface = (c == '!');
        }
        else
          m.imports.push_back (module_import {move (w), c == '*', 0});
      }

      return m;
    }

    // preprocessed
    //
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
      match_data (translation_type t, const prerequisite_member& s)
          : type (t), src (s) {}

      translation_type type;
      preprocessed pp = preprocessed::none;
      bool symexport = false;                // Target uses __symexport.
      bool touch = false;                    // Target needs to be touched.
      timestamp mt = timestamp_unknown;      // Target timestamp.
      prerequisite_member src;
      auto_rmfile psrc;                      // Preprocessed source, if any.
      path dd;                               // Dependency database path.
      module_positions mods = {0, 0, 0};
    };

    compile::
    compile (data&& d)
        : common (move (d)),
          rule_id (string (x) += ".compile 4")
    {
      static_assert (sizeof (compile::match_data) <= target::data_size,
                     "insufficient space");
    }

    const char* compile::
    langopt (const match_data& md) const
    {
      bool m (md.type == translation_type::module_iface);
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

    inline void compile::
    append_symexport_options (cstrings& args, const target& t) const
    {
      // With VC if a BMI is compiled with dllexport, then when such BMI is
      // imported, it is auto-magically treated as dllimport. Let's hope
      // other compilers follow suit.
      //
      args.push_back (t.is_a<bmis> () && tclass == "windows"
                      ? "-D__symexport=__declspec(dllexport)"
                      : "-D__symexport=");
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
        t.group = &search (t,
                           mod ? bmi::static_type : obj::static_type,
                           t.dir, t.out, t.name);

      // See if we have a source file. Iterate in reverse so that a source
      // file specified for a member overrides the one specified for the
      // group. Also "see through" groups.
      //
      for (prerequisite_member p: reverse_group_prerequisite_members (act, t))
      {
        if (p.is_a (mod ? *x_mod : x_src))
        {
          // Save in the target's auxiliary storage. Translation type will
          // be refined in apply().
          //
          t.data (match_data (mod
                              ? translation_type::module_iface
                              : translation_type::plain,
                              p));
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
                        linfo li) const
    {
      // See through utility libraries.
      //
      auto imp = [] (const file& l, bool la) {return la && l.is_a<libux> ();};

      auto opt = [&args, this] (
        const file& l, const string& t, bool com, bool exp)
      {
        // Note that in our model *.export.poptions are always "interface",
        // even if set on liba{}/libs{}, unlike loptions.
        //
        if (!exp) // Ignore libux.
          return;

        const variable& var (
          com
          ? c_export_poptions
          : (t == x ? x_export_poptions : var_pool[t + ".export.poptions"]));

        append_options (args, l, var);
      };

      // In case we don't have the "small function object" optimization.
      //
      const function<bool (const file&, bool)> impf (imp);
      const function<void (const file&, const string&, bool, bool)> optf (opt);

      for (prerequisite_member p: group_prerequisite_members (act, t))
      {
        // Should be already searched and matched for libraries.
        //
        if (const target* pt = p.load ())
        {
          if (const libx* l = pt->is_a<libx> ())
            pt = &link_member (*l, act, li);

          bool a;
          if (!((a = pt->is_a<liba> ())  ||
                (a = pt->is_a<libux> ()) ||
                pt->is_a<libs> ()))
            continue;

          process_libraries (act, bs, li, sys_lib_dirs,
                             pt->as<file> (), a, 0, // Hack: lflags unused.
                             impf, nullptr, optf);
        }
      }
    }

    void compile::
    hash_lib_options (const scope& bs,
                      sha256& cs,
                      const target& t,
                      action act,
                      linfo li) const
    {
      auto imp = [] (const file& l, bool la) {return la && l.is_a<libux> ();};

      auto opt = [&cs, this] (
        const file& l, const string& t, bool com, bool exp)
      {
        if (!exp)
          return;

        const variable& var (
          com
          ? c_export_poptions
          : (t == x ? x_export_poptions : var_pool[t + ".export.poptions"]));

        hash_options (cs, l, var);
      };

      // The same logic as in append_lib_options().
      //
      const function<bool (const file&, bool)> impf (imp);
      const function<void (const file&, const string&, bool, bool)> optf (opt);

      for (prerequisite_member p: group_prerequisite_members (act, t))
      {
        if (const target* pt = p.load ())
        {
          if (const libx* l = pt->is_a<libx> ())
            pt = &link_member (*l, act, li);

          bool a;
          if (!((a = pt->is_a<liba> ())  ||
                (a = pt->is_a<libux> ()) ||
                pt->is_a<libs> ()))
            continue;

          process_libraries (act, bs, li, sys_lib_dirs,
                             pt->as<file> (), a, 0, // Hack: lflags unused.
                             impf, nullptr, optf);
        }
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
                         linfo li) const
    {
      auto imp = [] (const file& l, bool la) {return la && l.is_a<libux> ();};

      auto opt = [&m, this] (
        const file& l, const string& t, bool com, bool exp)
      {
        if (!exp)
          return;

        const variable& var (
          com
          ? c_export_poptions
          : (t == x ? x_export_poptions : var_pool[t + ".export.poptions"]));

        append_prefixes (m, l, var);
      };

      // The same logic as in append_lib_options().
      //
      const function<bool (const file&, bool)> impf (imp);
      const function<void (const file&, const string&, bool, bool)> optf (opt);

      for (prerequisite_member p: group_prerequisite_members (act, t))
      {
        if (const target* pt = p.load ())
        {
          if (const libx* l = pt->is_a<libx> ())
            pt = &link_member (*l, act, li);

          bool a;
          if (!((a = pt->is_a<liba> ())  ||
                (a = pt->is_a<libux> ()) ||
                pt->is_a<libs> ()))
            continue;

          process_libraries (act, bs, li, sys_lib_dirs,
                             pt->as<file> (), a, 0, // Hack: lflags unused.
                             impf, nullptr, optf);
        }
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
      bool mod (md.type == translation_type::module_iface);

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      otype ot (compile_type (t, mod));
      linfo li (link_info (bs, ot)); // Link info for selecting libraries.
      compile_target_types tt (compile_types (ot));

      // Derive file name from target name.
      //
      string e; // Primary target extension (module or object).
      {
        const char* o ("o"); // Object extension (.o or .obj).

        if (tsys == "win32-msvc")
        {
          switch (ot)
          {
          case otype::e: e = "exe."; break;
          case otype::a: e = "lib."; break;
          case otype::s: e = "dll."; break;
          }
          o = "obj";
        }
        else if (tsys == "mingw32")
        {
          switch (ot)
          {
          case otype::e: e = "exe."; break;
          case otype::a: e = "a.";   break;
          case otype::s: e = "dll."; break;
          }
        }
        else if (tsys == "darwin")
        {
          switch (ot)
          {
          case otype::e: e = "";       break;
          case otype::a: e = "a.";     break;
          case otype::s: e = "dylib."; break;
          }
        }
        else
        {
          switch (ot)
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
        if (p.is_a<libx> () ||
            p.is_a<liba> () ||
            p.is_a<libs> () ||
            p.is_a<libux> ())
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

            if (const libx* l = pt->is_a<libx> ())
              pt = &link_member (*l, act, li);
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
        if (build2::match (
              act,
              *pt,
              pt->is_a<liba> () || pt->is_a<libs> () || pt->is_a<libux> ()
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

        // Figure out if __symexport is used. While normally it is specified
        // on the project root (which we cached), it can be overridden with
        // a target-specific value for installed modules (which we sidebuild
        // as part of our project).
        //
        if (modules && src.is_a (*x_mod))
        {
          lookup l (src.vars[x_symexport]);
          md.symexport = l ? cast<bool> (l) : symexport;
        }

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

        // Note: the leading '@' is reserved for the module map prefix (see
        // extract_modules()) and no other line must start with it.
        //
        md.dd = tp + ".d";
        depdb dd (md.dd);

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
        {
          sha256 cs;

          // These flags affect how we compile the source and/or the format of
          // depdb so factor them in.
          //
          cs.append (&md.pp, sizeof (md.pp));
          cs.append (&md.symexport, sizeof (md.symexport));

          if (md.pp != preprocessed::all)
          {
            hash_options (cs, t, c_poptions);
            hash_options (cs, t, x_poptions);

            // Hash *.export.poptions from prerequisite libraries.
            //
            hash_lib_options (bs, cs, t, act, li);

            // Extra system header dirs (last).
            //
            assert (sys_inc_dirs_extra <= sys_inc_dirs.size ());
            hash_option_values (
              cs, "-I",
              sys_inc_dirs.begin () + sys_inc_dirs_extra, sys_inc_dirs.end (),
              [] (const dir_path& d) {return d.string ();});
          }

          hash_options (cs, t, c_coptions);
          hash_options (cs, t, x_coptions);
          hash_options (cs, tstd);

          if (ot == otype::s)
          {
            // On Darwin, Win32 -fPIC is the default.
            //
            if (tclass == "linux" || tclass == "bsd")
              cs.append ("-fPIC");
          }

          if (dd.expect (cs.string ()) != nullptr)
            l4 ([&]{trace << "options mismatch forcing update of " << t;});
        }

        // Finally the source file.
        //
        if (dd.expect (src.path ()) != nullptr)
          l4 ([&]{trace << "source file mismatch forcing update of " << t;});

        // If any of the above checks resulted in a mismatch (different
        // compiler, options, or source file) or if the depdb is newer than
        // the target (interrupted update), then do unconditional update.
        //
        timestamp mt;
        bool u (dd.writing () || dd.mtime () > (mt = file_mtime (tp)));
        if (u)
          mt = timestamp_nonexistent; // Treat as if it doesn't exist.

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
        // Note: must be set of the src target.
        //
        if (const string* v = cast_null<string> (src[x_preprocessed]))
        try
        {
          md.pp = to_preprocessed (*v);
        }
        catch (const invalid_argument& e)
        {
          fail << "invalid " << x_preprocessed.name << " variable value "
               << "for target " << src << ": " << e;
        }

        // If we have no #include directives, then skip header dependency
        // extraction.
        //
        pair<auto_rmfile, bool> psrc (auto_rmfile (), false);
        if (md.pp < preprocessed::includes)
          psrc = extract_headers (act, bs, t, li, src, md, dd, u, mt);

        // Next we "obtain" the translation unit information. What exactly
        // "obtain" entails is tricky: If things changed, then we re-parse the
        // translation unit. Otherwise, we re-create this information from
        // depdb. We, however, have to do it here and now in case the database
        // is invalid and we still have to fallback to re-parse.
        //
        // Store a translation unit's checksum to detect ignorable changes
        // (whitespaces, comments, etc).
        //
        {
          string cs;
          if (string* l = dd.read ())
            cs = move (*l);
          else
            u = true; // Database is invalid, force re-parse.

          translation_unit tu;
          for (bool f (true);; f = false)
          {
            if (u)
            {
              auto p (parse_unit (act, t, li, src, psrc.first, md));

              if (cs != p.second)
              {
                assert (f); // Unchanged TU has a different checksum?
                dd.write (p.second);
              }
              else if (f) // Don't clear if it was forced.
              {
                // Clear the update flag and set the touch flag. Unless there
                // is no object file, of course. See also the md.mt logic
                // below.
                //
                if (mt != timestamp_nonexistent)
                {
                  u = false;
                  md.touch = true;
                }
              }

              tu = move (p.first);
            }

            if (modules)
            {
              if (u || !f)
              {
                string s (to_string (tu.mod));

                if (f)
                  dd.expect (s);
                else
                  dd.write (s);
              }
              else
              {
                if (string* l = dd.read ())
                  tu.mod = to_module_info (*l);
                else
                {
                  u = true; // Database is invalid, force re-parse.
                  continue;
                }
              }
            }

            break;
          }

          md.type = tu.type ();

          // Extract the module dependency information in addition to header
          // dependencies.
          //
          // NOTE: assumes that no further targets will be added into
          //       t.prerequisite_targets!
          //
          extract_modules (act, bs, t, li, tt, src, md, move (tu.mod), dd, u);
        }

        // If anything got updated, then we didn't rely on the cache. However,
        // the cached data could actually have been valid and the compiler run
        // in extract_headers() as well as the code above merely validated it.
        //
        // We do need to update the database timestamp, however. Failed that,
        // we will keep re-validating the cached data over and over again.
        //
        if (u && dd.reading ())
          dd.touch ();

        dd.close ();

        // If the preprocessed output is suitable for compilation and is not
        // disabled, then pass it along.
        //
        if (psrc.second && !cast_false<bool> (t[c_reprocess]))
        {
          md.psrc = move (psrc.first);

          // Without modules keeping the (partially) preprocessed output
          // around doesn't buy us much: if the source/headers haven't changed
          // then neither will the object file. Modules make things more
          // interesting: now we may have to recompile an otherwise unchanged
          // translation unit because a BMI it depends on has changed. In this
          // case re-processing the translation unit would be a waste and
          // compiling the original source would break distributed
          // compilation.
          //
          // Note also that the long term trend will (hopefully) be for
          // modularized projects to get rid of #include's which means the
          // need for producing this partially preprocessed output will
          // (hopefully) gradually disappear.
          //
          if (modules)
            md.psrc.active = false; // Keep.
        }

        // Above we may have ignored changes to the translation unit. The
        // problem is, unless we also update the target's timestamp, we will
        // keep re-checking this on subsequent runs and it is not cheap.
        // Updating the target's timestamp is not without problems either: it
        // will cause a re-link on a subsequent run. So, essentially, we
        // somehow need to remember two timestamps: one for checking
        // "preprocessor prerequisites" above and one for checking other
        // prerequisites (like modules) below. So what we are going to do is
        // store the first in the target file (so we do touch it) and the
        // second in depdb (which is never newer that the target).
        //
        md.mt = u ? timestamp_nonexistent : dd.mtime ();
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
      const scope& bs (t.base_scope ());
      const scope* rs (bs.root_scope ());
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

          l6 ([&]{trace << "-I " << d;});

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

          // We use the target's directory as out_base but that doesn't work
          // well for targets that are stashed in subdirectories. So as a
          // heuristics we are going to also enter the outer directories of
          // the original prefix. It is, however, possible, that another -I
          // option after this one will produce one of these outer prefixes as
          // its original prefix in which case we should override it.
          //
          // So we are going to assign the original prefix priority value 0
          // (highest) and then increment it for each outer prefix.
          //
          auto enter = [&trace, &m] (dir_path p, dir_path d, size_t prio)
          {
            auto j (m.find (p));

            if (j != m.end ())
            {
              prefix_value& v (j->second);

              // We used to reject duplicates but it seems this can be
              // reasonably expected to work according to the order of the
              // -I options.
              //
              // Seeing that we normally have more "specific" -I paths first,
              // (so that we don't pick up installed headers, etc), we ignore
              // it.
              //
              if (v.directory == d)
              {
                if (v.priority > prio)
                  v.priority = prio;
              }
              else if (v.priority <= prio)
              {
                if (verb >= 4)
                  trace << "ignoring dependency prefix " << p << '\n'
                        << "  existing mapping to " << v.directory
                        << " priority " << v.priority << '\n'
                        << "  another mapping to  " << d
                        << " priority " << prio;
              }
              else
              {
                if (verb >= 4)
                  trace << "overriding dependency prefix " << p << '\n'
                        << "  existing mapping to " << v.directory
                        << " priority " << v.priority << '\n'
                        << "  new mapping to      " << d
                        << " priority " << prio;

                v.directory = move (d);
                v.priority = prio;
              }
            }
            else
            {
              l6 ([&]{trace << p << " -> " << d << " priority " << prio;});
              m.emplace (move (p), prefix_value {move (d), prio});
            }
          };

          size_t prio (0);
          for (bool e (false); !e; ++prio)
          {
            dir_path n (p.directory ());
            e = n.empty ();
            enter ((e ? move (p) : p), (e ? move (d) : d), prio);
            p = move (n);
          }
        }
      }
    }

    auto compile::
    build_prefix_map (const scope& bs,
                      target& t,
                      action act,
                      linfo li) const -> prefix_map
    {
      prefix_map m;

      // First process our own.
      //
      append_prefixes (m, t, c_poptions);
      append_prefixes (m, t, x_poptions);

      // Then process the include directories from prerequisite libraries.
      //
      append_lib_prefixes (bs, m, t, act, li);

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

    // Sense whether this is an include note (return npos) or a diagnostics
    // line (return postion of the NNNN code in CNNNN).
    //
    static inline size_t
    next_show_sense (const string& l)
    {
      size_t p (l.find (':'));

      for (size_t n (l.size ());
           p != string::npos;
           p = ++p != n ? l.find (':', p) : string::npos)
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

      return p;
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

      size_t p (next_show_sense (l));
      if (p == string::npos)
      {
        // Include note. We assume the path is always at the end but need to
        // handle both absolute Windows and POSIX ones.
        //
        // Note that VC appears to always write the absolute path to the
        // included file even if it is ""-included and the source path is
        // relative. Aren't we lucky today?
        //
        p = l.rfind (':');

        if (p != string::npos)
        {
          // See if this one is part of the Windows drive letter.
          //
          if (p > 1 && p + 1 < l.size () && // 2 chars before, 1 after.
              l[p - 2] == ' '            &&
              alpha (l[p - 1])           &&
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
                     const scope& bs,
                     file& t,
                     linfo li,
                     const file& src,
                     const match_data& md,
                     depdb& dd,
                     bool& updating,
                     timestamp mt) const
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
      environment env;
      cstrings args;
      string out; // Storage.

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
      // this fixes the error (so it was a generated header after all), then
      // we have to restart at which point we go back to -E and no -MG. And we
      // keep yo-yoing like this. Missing generated headers will probably be
      // fairly rare occurrence so this shouldn't be too expensive.
      //
      // Actually, there is another error case we would like to handle: an
      // outdated generated header that is now causing an error (e.g., because
      // of a check that is now triggering #error or some such). So there are
      // actually three error cases: outdated generated header, missing
      // generated header, and some other error. To handle the outdated case
      // we need the compiler to produce the dependency information even in
      // case of an error. Clang does it, for VC we parse diagnostics
      // ourselves, but GCC does not (but a patch has been submitted).
      //
      // So the final plan is then as follows:
      //
      // 1. Start wothout -MG and with suppressed diagnostics.
      // 2. If error but we've updated a header, then repeat step 1.
      // 3. Otherwise, restart with -MG and diagnostics.
      //
      // Note that below we don't even check if the compiler supports the
      // dependency info on error. We just try to use it and if it's not
      // there we ignore the io error since the compiler has failed.
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

      // And here is another problem: if we have an already generated header
      // in src and the one in out does not yet exist, then the compiler will
      // pick the one in src and we won't even notice. Note that this is not
      // only an issue with mixing in- and out-of-tree builds (which does feel
      // wrong but is oh so convenient): this is also a problem with
      // pre-generated headers, a technique we use to make installing the
      // generator by end-users optional by shipping pre-generated headers.
      //
      // This is a nasty problem that doesn't seem to have a perfect solution
      // (except, perhaps, C++ modules). So what we are going to do is try to
      // rectify the situation by detecting and automatically remapping such
      // mis-inclusions. It works as follows.
      //
      // First we will build a map of src/out pairs that were specified with
      // -I. Here, for performance and simplicity, we will assume that they
      // always come in pairs with out first and src second. We build this
      // map lazily only if we are running the preprocessor and reuse it
      // between restarts.
      //
      // With the map in hand we can then check each included header for
      // potentially having a doppelganger in the out tree. If this is the
      // case, then we calculate a corresponding header in the out tree and,
      // (this is the most important part), check if there is a target for
      // this header in the out tree. This should be fairly accurate and not
      // require anything explicit from the user except perhaps for a case
      // where the header is generated out of nothing (so there is no need to
      // explicitly mention its target in the buildfile). But this probably
      // won't be very common.
      //
      // One tricky area in this setup are target groups: if the generated
      // sources are mentioned in the buildfile as a group, then there might
      // be no header target (yet). The way we solve this is by requiring code
      // generator rules to cooperate and create at least the header target as
      // part of the group creation. While not all members of the group may be
      // generated depending on the options (e.g., inline files might be
      // suppressed), headers are usually non-optional.
      //
      // Note that we use path_map instead of dir_path_map to allow searching
      // using path (file path).
      //
      using srcout_map = path_map<dir_path>;
      srcout_map so_map;

      // The gen argument to init_args() is in/out. The caller signals whether
      // to force the generated header support and on return it signals
      // whether this support is enabled. The first call to init_args is
      // expected to have gen false.
      //
      // Return NULL if the dependency information goes to stdout and a
      // pointer to the temporary file path otherwise.
      //
      auto init_args = [&t, act, li,
                        &src, &md, &psrc, &sense_diag,
                        &rs, &bs,
                        pp, &env, &args, &args_gen, &args_i, &out, &drm,
                        &so_map, this]
        (bool& gen) -> const path*
      {
        const path* r (nullptr);

        if (args.empty ()) // First call.
        {
          assert (!gen);

          // We use absolute/relative paths in the dependency output to
          // distinguish existing headers from (missing) generated. Which
          // means we have to (a) use absolute paths in -I and (b) pass
          // absolute source path (for ""-includes). That (b) is a problem:
          // if we use an absolute path, then all the #line directives will be
          // absolute and all the diagnostics will have long, noisy paths
          // (actually, we will still have long paths for diagnostics in
          // headers).
          //
          // To work around this we used to pass a relative path to the source
          // file and then check every relative path in the dependency output
          // for existence in the source file's directory. This is not without
          // issues: it is theoretically possible for a generated header that
          // is <>-included and found via -I to exist in the source file's
          // directory. Note, however, that this is a lot more likely to
          // happen with prefix-less inclusion (e.g., <foo>) and in this case
          // we assume the file is in the project anyway. And if there is a
          // conflict with a prefixed include (e.g., <bar/foo>), then, well,
          // we will just have to get rid of quoted includes (which are
          // generally a bad idea, anyway).
          //
          // But then this approach (relative path) fell apart further when we
          // tried to implement precise changed detection: the preprocessed
          // output would change depending from where it was compiled because
          // of #line (which we could work around) and __FILE__/assert()
          // (which we can't really do anything about). So it looks like using
          // the absolute path is the lesser of all the evils (and there are
          // many).
          //
          // Note that we detect and diagnose relative -I directories lazily
          // when building the include prefix map.
          //
          args.push_back (cpath.recall_string ());

          // Add *.export.poptions from prerequisite libraries.
          //
          append_lib_options (bs, args, t, act, li);

          append_options (args, t, c_poptions);
          append_options (args, t, x_poptions);

          // Populate the src-out with the -I$out_base -I$src_base pairs.
          //
          {
            // Try to be fast and efficient by reusing buffers as much as
            // possible.
            //
            string ds;

            // Previous -I innermost scope if out_base plus the difference
            // between the scope path and the -I path (normally empty).
            //
            const scope* s (nullptr);
            dir_path p;

            for (auto i (args.begin ()), e (args.end ()); i != e; ++i)
            {
              // -I can either be in the "-Ifoo" or "-I foo" form. For VC it
              // can also be /I.
              //
              const char* o (*i);
              size_t n (strlen (o));

              if (n < 2 || (o[0] != '-' && o[0] != '/') || o[1] != 'I')
              {
                s = nullptr;
                continue;
              }

              if (n == 2)
              {
                if (++i == e)
                  break; // Let the compiler complain.

                ds = *i;
              }
              else
                ds.assign (o + 2, n - 2);

              if (!ds.empty ())
              {
                // Note that we don't normalize the paths since it would be
                // quite expensive and normally the pairs we are inerested in
                // are already normalized (since they are usually specified as
                // -I$src/out_*). We just need to add a trailing directory
                // separator if it's not already there.
                //
                if (!dir_path::traits::is_separator (ds.back ()))
                  ds += dir_path::traits::directory_separator;

                dir_path d (move (ds), dir_path::exact); // Move the buffer in.

                // Ignore invalid paths (buffer is not moved).
                //
                if (!d.empty ())
                {
                  // Ignore any paths containing '.', '..' components. Allow
                  // any directory separators thought (think -I$src_root/foo
                  // on Windows).
                  //
                  if (d.absolute () && d.normalized (false))
                  {
                    // If we have a candidate out_base, see if this is its
                    // src_base.
                    //
                    if (s != nullptr)
                    {
                      const dir_path& bp (s->src_path ());

                      if (d.sub (bp))
                      {
                        if (p.empty () || d.leaf (bp) == p)
                        {
                          // We've got a pair.
                          //
                          so_map.emplace (move (d), s->out_path () / p);
                          s = nullptr; // Taken.
                          continue;
                        }
                      }

                      // Not a pair. Fall through to consider as out_base.
                      //
                      s = nullptr;
                    }

                    // See if this path is inside a project with an out-of-
                    // tree build and is in the out directory tree.
                    //
                    const scope& bs (scopes.find (d));
                    if (bs.root_scope () != nullptr)
                    {
                      const dir_path& bp (bs.out_path ());
                      if (bp != bs.src_path ())
                      {
                        bool e;
                        if ((e = (d == bp)) || d.sub (bp))
                        {
                          s = &bs;
                          if (e)
                            p.clear ();
                          else
                            p = d.leaf (bp);
                        }
                      }
                    }
                  }
                  else
                    s = nullptr;

                  ds = move (d).string (); // Move the buffer out.
                }
                else
                  s = nullptr;
              }
              else
                s = nullptr;
            }
          }

          // Extra system header dirs (last).
          //
          assert (sys_inc_dirs_extra <= sys_inc_dirs.size ());
          append_option_values (
            args, "-I",
            sys_inc_dirs.begin () + sys_inc_dirs_extra, sys_inc_dirs.end (),
            [] (const dir_path& d) {return d.string ().c_str ();});

          if (md.symexport)
            append_symexport_options (args, t);

          // Some compile options (e.g., -std, -m) affect the preprocessor.
          //
          // Currently Clang supports importing "header modules" even when in
          // the TS mode. And "header modules" support macros which means
          // imports have to be resolved during preprocessing. Which poses a
          // bit of a chicken and egg problem for us. For now, the workaround
          // is to remove the -fmodules-ts option when preprocessing. Hopefully
          // there will be a "pure modules" mode at some point.
          //

          // Don't treat warnings as errors.
          //
          const char* werror (cid == compiler_id::msvc ? "/WX" : "-Werror");

          append_options (args, t, c_coptions, werror);
          append_options (args, t, x_coptions, werror);
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
              args.push_back (psrc.path.string ().c_str ());
            }
            else
            {
              out = "/Fi" + psrc.path.string ();
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
                r = &(drm = auto_rmfile (t.path () + ".t")).path;
                args.push_back (r->string ().c_str ());

                sense_diag = true;
              }
              else
                args.push_back ("-");

              // Preprocessor output.
              //
              psrc = auto_rmfile (t.path () + x_pext);
              args.push_back ("-o");
              args.push_back (psrc.path.string ().c_str ());
            }
            else
            {
              args.push_back ("-M");
              args.push_back ("-MG"); // Treat missing headers as generated.
            }

            gen = args_gen = (pp == nullptr);
          }

          args.push_back (src.path ().string ().c_str ());
          args.push_back (nullptr);

          // Note: only doing it here.
          //
          if (!env.empty ())
            env.push_back (nullptr);
        }
        else
        {
          assert (gen != args_gen);

          size_t i (args_i);

          if (gen)
          {
            // Overwrite.
            //
            args[i++] = "-M";
            args[i++] = "-MG";
            args[i++] = src.path ().string ().c_str ();
            args[i]   = nullptr;

            if (cid == compiler_id::gcc)
            {
              sense_diag = false;
            }
          }
          else
          {
            // Restore.
            //
            args[i++] = "-MD";
            args[i++] = "-E";
            args[i++] = pp;
            args[i]   = "-MF";

            if (cid == compiler_id::gcc)
            {
              r = &drm.path;
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
      optional<prefix_map> pfx_map;

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
      size_t skip_count (0);

      // Update and add a header file to the list of prerequisite targets.
      // Depending on the cache flag, the file is assumed to either have come
      // from the depdb cache or from the compiler run. Return whether the
      // extraction process should be restarted.
      //
      auto add = [&trace, &pfx_map, &so_map,
                  act, &t, li,
                  &dd, &updating, &skip_count,
                  &bs, this]
        (path f, bool cache, timestamp mt) -> bool
      {
        // Find or maybe insert the target. The directory is only moved
        // from if insert is true.
        //
        auto find = [&trace, &t, this]
          (dir_path&& d, path&& f, bool insert) -> const path_target*
        {
          // Split the file into its name part and extension. Here we can
          // assume the name part is a valid filesystem name.
          //
          // Note that if the file has no extension, we record an empty
          // extension rather than NULL (which would signify that the default
          // extension should be added).
          //
          string e (f.extension ());
          string n (move (f).string ());

          if (!e.empty ())
            n.resize (n.size () - e.size () - 1); // One for the dot.

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

        // If it's not absolute then it either does not (yet) exist or is
        // a relative ""-include (see init_args() for details). Reduce the
        // second case to absolute.
        //
        // Note: we now always use absolute path to the translation unit so
        // this no longer applies.
        //
#if 0
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
#endif

        const path_target* pt (nullptr);

        // If still relative then it does not exist.
        //
        if (f.relative ())
        {
          f.normalize ();

          // This is probably as often an error as an auto-generated file, so
          // trace at level 4.
          //
          l4 ([&]{trace << "non-existent header '" << f << "'";});

          if (!pfx_map)
            pfx_map = build_prefix_map (bs, t, act, li);

          // First try the whole file. Then just the directory.
          //
          // @@ Has to be a separate map since the prefix can be the same as
          //    the file name.
          //
          // auto i (pfx_map->find (f));

          // Find the most qualified prefix of which we are a sub-path.
          //
          if (!pfx_map->empty ())
          {
            dir_path d (f.directory ());
            auto i (pfx_map->find_sup (d));

            if (i != pfx_map->end ())
            {
              const dir_path& pd (i->second.directory);

              // If this is a prefixless mapping, then only use it if we can
              // resolve it to an existing target (i.e., it is explicitly
              // spelled out in a buildfile).
              //
              // Note that at some point we will probably have a list of
              // directories.
              //
              pt = find (pd / d, f.leaf (), !i->first.empty ());
              if (pt != nullptr)
              {
                f = pd / f;
                l4 ([&]{trace << "mapped as auto-generated " << f;});
              }
            }
          }

          if (pt == nullptr)
          {
            diag_record dr (fail);
            dr << "header '" << f << "' not found and cannot be generated";
            //for (const auto& p: pm)
            //  dr << info << p.first.string () << " -> " << p.second.string ();
          }
        }
        else
        {
          // We used to just normalize the path but that could result in an
          // invalid path (e.g., on CentOS 7 with Clang 3.4) because of the
          // symlinks. So now we realize (i.e., realpath(3)) it instead.
          // Unless it comes from the depdb, in which case we've already done
          // that. This is also where we handle src-out remap (again, not
          // needed if cached)
          //
          if (!cache)
          {
            // While we can reasonably expect this path to exit, things do
            // go south from time to time (like compiling under wine with
            // file wlantypes.h included as WlanTypes.h).
            //
            try
            {
              f.realize ();
            }
            catch (const invalid_path&)
            {
              fail << "invalid header path '" << f << "'";
            }
            catch (const system_error& e)
            {
              fail << "invalid header path '" << f << "': " << e;
            }

            if (!so_map.empty ())
            {
              // Find the most qualified prefix of which we are a sub-path.
              //
              auto i (so_map.find_sup (f));
              if (i != so_map.end ())
              {
                // Ok, there is an out tree for this headers. Remap to a path
                // from the out tree and see if there is a target for it.
                //
                dir_path d (i->second);
                d /= f.leaf (i->first).directory ();
                pt = find (move (d), f.leaf (), false); // d is not moved from.

                if (pt != nullptr)
                {
                  path p (d / f.leaf ());
                  l4 ([&]{trace << "remapping " << f << " to " << p;});
                  f = move (p);
                }
              }
            }
          }

          if (pt == nullptr)
          {
            l6 ([&]{trace << "injecting " << f;});
            pt = find (f.directory (), f.leaf (), true);
          }
        }

        // Cache the path.
        //
        const path& pp (pt->path (move (f)));

        // Match to a rule.
        //
        // If we are reading the cache, then it is possible the file has since
        // been removed (think of a header in /usr/local/include that has been
        // uninstalled and now we need to use one from /usr/include). This
        // will lead to the match failure which we translate to a restart.
        //
        if (!cache)
          build2::match (act, *pt);
        else if (!build2::try_match (act, *pt).first)
        {
          dd.write (); // Invalidate this line.
          updating = true;
          return true;
        }

        // Update.
        //
        bool restart (update (trace, act, *pt, mt));

        // Verify/add it to the dependency database. We do it after update in
        // order not to add bogus files (non-existent and without a way to
        // update).
        //
        if (!cache)
          dd.expect (pp);

        // Add to our prerequisite target list.
        //
        t.prerequisite_targets.push_back (pt);
        skip_count++;

        updating = updating || restart;
        return restart;
      };

      // If nothing so far has invalidated the dependency database, then try
      // the cached data before running the compiler.
      //
      bool cache (!updating);

      // See init_args() above for details on generated header support.
      //
      bool gen (false);
      optional<bool>   force_gen;
      optional<size_t> force_gen_skip; // Skip count at last force_gen run.

      const path* drmp (nullptr); // Points to drm.path () if active.

      for (bool restart (true); restart; cache = false)
      {
        restart = false;

        if (cache)
        {
          // If any, this is always the first run.
          //
          assert (skip_count == 0);

          // We should always end with a blank line.
          //
          for (;;)
          {
            string* l (dd.read ());

            // If the line is invalid, run the compiler.
            //
            if (l == nullptr)
            {
              restart = true;
              break;
            }

            if (l->empty ()) // Done, nothing changed.
            {
              // If modules are enabled, then we keep the preprocessed output
              // around (see apply() for details).
              //
              return modules
                ? make_pair (auto_rmfile (t.path () + x_pext, false), true)
                : make_pair (auto_rmfile (), false);
            }

            // If this header came from the depdb, make sure it is no older
            // than the target (if it has changed since the target was
            // updated, then the cached data is stale).
            //
            restart = add (path (move (*l)), true, mt);

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

              // Save the timestamp just before we start preprocessing. If
              // we depend on any header that has been updated since, then
              // we should assume we've "seen" the old copy and re-process.
              //
              timestamp pmt (system_clock::now ());

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
                              cid == compiler_id::msvc ? 1 : gen ? 2 : -2,
                              nullptr, // CWD
                              env.empty () ? nullptr : env.data ());
              }
              else
              {
                // Dependency info goes to a temporary file.
                //
                pr = process (cpath,
                              args.data (),
                              0,
                              2, // Send stdout to stderr.
                              gen ? 2 : sense_diag ? -1 : -2,
                              nullptr, // CWD
                              env.empty () ? nullptr : env.data ());

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
              string l; // Reuse.
              for (bool first (true), second (false); !restart; )
              {
                if (eof (getline (is, l)))
                  break;

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
                    if (l != src.path ().leaf ().string ())
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
                    restart = add (path (move (f)), false, pmt);

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
                    // Empty/invalid output should mean the wait() call below
                    // will return false.
                    //
                    if (l.empty () ||
                        l[0] != '^' || l[1] != ':' || l[2] != ' ')
                    {
                      if (!l.empty ())
                        text << l;

                      bad_error = true;
                      break;
                    }

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

                    restart = add (path (move (f)), false, pmt);

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
              if (bad_error && cid == compiler_id::msvc)
              {
                // We used to just dump the whole rdbuf but it turns out VC
                // may continue writing include notes interleaved with the
                // diagnostics. So we have to filter them out.
                //
                for (; !eof (getline (is, l)); )
                {
                  size_t p (next_show_sense (l));
                  if (p != string::npos && l.compare (p, 4, "1083") != 0)
                    diag_stream_lock () << l << endl;
                }
              }

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
              // If this run was with the generated header support then we
              // have issued diagnostics and it's time to give up.
              //
              if (gen)
                throw failed ();

              // Just to recap, being here means something is wrong with the
              // source: it can be a missing generated header, it can be an
              // outdated generated header (e.g., some check triggered #error
              // which will go away if only we updated the generated header),
              // or it can be a real error that is not going away.
              //
              // So this is what we are going to do here: if anything got
              // updated on this run (i.e., the compiler has produced valid
              // dependency information even though there were errors and we
              // managed to find and update a header based on this
              // informaion), then we restart in the same mode hoping that
              // this fixes things. Otherwise, we force the generated header
              // support which will either uncover a missing generated header
              // or will issue diagnostics.
              //
              if (restart)
                l6 ([&]{trace << "trying again without generated headers";});
              else
              {
                // In some pathological situations (e.g., we are out of disk
                // space) we may end up switching back and forth indefinitely
                // without making any headway. So we use skip_count to track
                // our progress.
                //
                if (force_gen_skip && *force_gen_skip == skip_count)
                  fail << "inconsistent " << x_lang << " compiler behavior" <<
                    info << "perhaps you are running out of disk space";

                restart = true;
                force_gen = true;
                force_gen_skip = skip_count;
                l6 ([&]{trace << "restarting with forced generated headers";});
              }
              continue;
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

      // Add the terminating blank line (we are updated depdb).
      //
      dd.expect ("");

      puse = puse && !psrc.path.empty ();
      return make_pair (move (psrc), puse);
    }

    pair<translation_unit, string> compile::
    parse_unit (action act,
                file& t,
                linfo lo,
                const file& src,
                auto_rmfile& psrc,
                const match_data& md) const
    {
      tracer trace (x, "compile::parse_unit");

      // If things go wrong give the user a bit extra context.
      //
      auto df = make_diag_frame (
        [&src](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while parsing " << src;
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
      environment env;
      cstrings args;
      const path* sp; // Source path.

      bool ps; // True if extracting from psrc.
      if (md.pp < preprocessed::modules)
      {
        ps = !psrc.path.empty ();
        sp = &(ps ? psrc.path : src.path ());

        // VC's preprocessed output, if present, is fully preprocessed.
        //
        if (cid != compiler_id::msvc || !ps)
        {
          // This should match with how we setup preprocessing and is pretty
          // similar to init_args() from extract_headers().
          //
          args.push_back (cpath.recall_string ());

          append_lib_options (t.base_scope (), args, t, act, lo);

          append_options (args, t, c_poptions);
          append_options (args, t, x_poptions);

          assert (sys_inc_dirs_extra <= sys_inc_dirs.size ());
          append_option_values (
            args, "-I",
            sys_inc_dirs.begin () + sys_inc_dirs_extra, sys_inc_dirs.end (),
            [] (const dir_path& d) {return d.string ().c_str ();});

          if (md.symexport)
            append_symexport_options (args, t);

          // Make sure we don't fail because of warnings.
          //
          // @@ Can be both -WX and /WX.
          //
          const char* werror (cid == compiler_id::msvc ? "/WX" : "-Werror");

          append_options (args, t, c_coptions, werror);
          append_options (args, t, x_coptions, werror);
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

          args.push_back (sp->string ().c_str ());
          args.push_back (nullptr);
        }

        if (!env.empty ())
          env.push_back (nullptr);
      }
      else
      {
        // Extracting directly from source.
        //
        ps = false;
        sp = &src.path ();
      }

      // Preprocess and parse.
      //
      for (;;) // Breakout loop.
      try
      {
        // Disarm the removal of the preprocessed file in case of an error.
        // We re-arm it below.
        //
        if (ps)
          psrc.active = false;

        process pr;

        try
        {
          if (args.empty ())
          {
            pr = process (process_exit (0)); // Successfully exited.
            pr.in_ofd = fdopen (*sp, fdopen_mode::in);
          }
          else
          {
            if (verb >= 3)
              print_process (args);

            // We don't want to see warnings multiple times so ignore all
            // diagnostics.
            //
            pr = process (cpath,
                          args.data (),
                          0, -1, -2,
                          nullptr, // CWD
                          env.empty () ? nullptr : env.data ());
          }

          // Use binary mode to obtain consistent positions.
          //
          ifdstream is (move (pr.in_ofd),
                        fdstream_mode::binary | fdstream_mode::skip);

          parser p;
          translation_unit tu (p.parse (is, *sp));

          is.close ();

          if (pr.wait ())
          {
            if (ps)
              psrc.active = true; // Re-arm.

            // VC15 is not (yet) using the 'export module' syntax so use the
            // preprequisite type to distinguish between interface and
            // implementation units.
            //
            if (cid == compiler_id::msvc && src.is_a (*x_mod))
            {
              // It's quite painful to guard the export with an #if/#endif so
              // if it is present, "fixup" the (temporary) preprocessed output
              // by getting rid of the keyword.
              //
              // Note: when removing this also remember to remove the test.
              //
              if (tu.mod.iface)
              {
                // We can only fixup a temporary file.
                //
                if (!ps)
                  fail (relative (src)) << "fixup requires preprocessor";

                // Stomp out the export keyword with spaces. We are using
                // std::fstream since our fdstream does not yet support
                // seeking.
                //
                fstream os (psrc.path.string (), fstream::out | fstream::in);
                auto pos (static_cast<fstream::pos_type> (p.export_pos));

                if (!os.is_open ()  ||
                    !os.seekp (pos) ||
                    !os.write ("      ", 6))
                  fail << "unable to overwrite preprocessor output";
              }
              else
                tu.mod.iface = true;
            }

            return pair<translation_unit, string> (move (tu), p.checksum);
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
        // the compiler's diagnostics. We used to issue a warning and continue
        // with the assumption that the compilation step will fail with
        // diagnostics. The problem with this approach is that we may fail
        // before that because the information we return (e.g., module name)
        // is bogus. So looks like failing is the only option.
        //
        if (e.normal ())
        {
          fail << "unable to preprocess " << src <<
            info << "re-run with -s -V to display failing command" <<
            info << "then run failing command to display compiler diagnostics";
        }
        else
          fail << args[0] << " terminated abnormally: " << e.description ();
      }
      catch (const process_error& e)
      {
        error << "unable to execute " << args[0] << ": " << e;

        if (e.child)
          exit (1);
      }

      throw failed ();
    }

    // Extract and inject module dependencies.
    //
    void compile::
    extract_modules (action act,
                     const scope& bs,
                     file& t,
                     linfo li,
                     const compile_target_types& tt,
                     const file& src,
                     match_data& md,
                     module_info&& mi,
                     depdb& dd,
                     bool& updating) const
    {
      tracer trace (x, "compile::extract_modules");
      l5 ([&]{trace << "target: " << t;});

      // If things go wrong, give the user a bit extra context.
      //
      auto df = make_diag_frame (
        [&src](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while extracting module dependencies from " << src;
        });

      if (!modules)
      {
        if (!mi.name.empty () || !mi.imports.empty ())
          fail (relative (src)) << "modules support not enabled/available";

        return;
      }

      // Sanity checks.
      //
      // If we are compiling a module interface unit, make sure it has the
      // necessary declarations.
      //
      if (src.is_a (*x_mod) && (mi.name.empty () || !mi.iface))
        fail << src << " is not a module interface unit";

      // Search and match all the modules we depend on. If this is a module
      // implementation unit, then treat the module itself as if it was
      // imported (we insert it first since for some compilers we have to
      // differentiate between this special module and real imports). Note:
      // move.
      //
      if (!mi.iface && !mi.name.empty ())
        mi.imports.insert (mi.imports.begin (),
                           module_import {move (mi.name), false, 0});

      // The change to the set of imports would have required a change to
      // source code (or options). Changes to the bmi{}s themselves will be
      // detected via the normal prerequisite machinery. However, the same set
      // of imports could be resolved to a different set of bmi{}s (in a sense
      // similar to changing the source file). To detect this we calculate and
      // store a hash of all (not just direct) bmi{}'s paths.
      //
      sha256 cs;

      if (!mi.imports.empty ())
        md.mods = search_modules (act, bs, t, li, tt.bmi, src, mi.imports, cs);

      if (dd.expect (cs.string ()) != nullptr)
        updating = true;

#if 0
      // Save the module map for compilers that use it.
      //
      if (md.mods.start != 0)
      {
        switch (cid)
        {
        case compiler_id::gcc:
        case compiler_id::clang:
          {
            // We don't need to redo this if the above hash hasn't changed and
            // the database is valid.
            //
            if (dd.writing () || !dd.skip ())
            {
              const auto& pts (t.prerequisite_targets);

              for (size_t i (md.mods.start); i != pts.size (); ++i)
              {
                if (const target* m = pts[i])
                {
                  // Save a variable lookup by getting the module name from
                  // the import list (see search_modules()).
                  //
                  dd.write ('@', false);
                  dd.write (mi.imports[i - md.mods.start].name, false);
                  dd.write ('=', false);
                  dd.write (m->as<file> ().path ());
                }
              }
            }
            break;
          }
        default:
          break;
        }
      }
#endif

      // Set the cc.module_name variable if this is an interface unit. Note
      // that it may seem like a good idea to set it on the bmi{} group to
      // avoid duplication. We, however, cannot do it MT-safely since we don't
      // match the group.
      //
      if (mi.iface)
      {
        if (value& v = t.vars.assign (c_module_name))
          assert (cast<string> (v) == mi.name);
        else
          v = move (mi.name); // Note: move.
      }
    }

    inline bool
    std_module (const string& m)
    {
      size_t n (m.size ());
      return (n >= 3 &&
              m[0] == 's' && m[1] == 't' && m[2] == 'd' &&
              (n == 3 || m[3] == '.'));
    };

    // Resolve imported modules to bmi*{} targets.
    //
    module_positions compile::
    search_modules (action act,
                    const scope& bs,
                    file& t,
                    linfo li,
                    const target_type& mtt,
                    const file& src,
                    module_imports& imports,
                    sha256& cs) const
    {
      tracer trace (x, "compile::search_modules");

      // So we have a list of imports and a list of "potential" module
      // prerequisites. They are potential in the sense that they may or may
      // not be required by this translation unit. In other words, they are
      // the pool where we can resolve actual imports.
      //
      // Because we may not need all of these prerequisites, we cannot just go
      // ahead and match all of them (and they can even have cycles; see rule
      // synthesis). This poses a bit of a problem: the only way to discover
      // the module's actual name (see cc.module_name) is by matching it.
      //
      // One way to solve this would be to make the user specify the module
      // name for each mxx{} explicitly. This will be a major pain, however.
      // Another would be to require encoding of the module name in the
      // interface unit file name. For example, hello.core -> hello-core.mxx.
      // This is better but still too restrictive: some will want to call it
      // hello_core.mxx or HelloCore.mxx (because that's their file naming
      // convention) or place it in a subdirectory, say, hello/core.mxx.
      //
      // In the above examples one common theme about all the file names is
      // that they contain, in one form or another, the "tail" of the module
      // name ('core'). So what we are going to do is require that the
      // interface file names contain enough of the module name tail to
      // unambiguously resolve all the module imports. On our side we are
      // going to implement a "fuzzy" module name to file name match. This
      // should be reliable enough since we will always verify our guesses
      // once we match the target and extract the actual module name. Plus,
      // the user will always have the option of resolving any impasses by
      // specifying the module name explicitly.
      //
      // So, the fuzzy match: the idea is that each match gets a score, the
      // number of characters in the module name that got matched. A match
      // with the highest score is used. And we use the (length + 1) for a
      // match against an actual module name.
      //
      // For std.* modules we only accept non-fuzzy matches (think std.core vs
      // some core.mxx). And if such a module is unresolved, then we assume it
      // is pre-built and will be found by some other means (e.g., VC's
      // IFCPATH).
      //
      auto match = [] (const string& f, const string& m) -> size_t
      {
        size_t fi (f.size ());
        size_t mi (m.size ());

        // Scan backwards for as long as we match. Keep track of the previous
        // character for case change detection.
        //
        for (char fc, mc, fp ('\0'), mp ('\0');
             fi != 0 && mi != 0;
             fp = fc, mp = mc, --fi, --mi)
        {
          fc = f[fi - 1];
          mc = m[mi - 1];

          if (casecmp (fc, mc) == 0)
            continue;

          // We consider all separators equal and character case change being
          // a separators. Some examples of the latter:
          //
          // foo.bar
          //  fooBAR
          //  FOObar
          //
          bool fs (fc == '_' || fc == '-' || fc == '.' ||
                   path::traits::is_separator (fc));
          bool ms (mc == '_' || mc == '.');

          if (fs && ms)
            continue;

          // Only if one is a real separator do we consider case change.
          //
          if (fs || ms)
          {
            auto cc = [] (char c1, char c2) -> bool
            {
              return (alpha (c1) &&
                      alpha (c2) &&
                      (ucase (c1) == c1) != (ucase (c2) == c2));
            };

            bool fa (false), ma (false);
            if ((fs || (fa = cc (fp, fc))) && (ms || (ma = cc (mp, mc))))
            {
              // Stay on this character if imaginary punctuation (note: cannot
              // be both true).
              //
              if (fa) ++fi;
              if (ma) ++mi;
              continue;
            }
          }

          break; // No match.
        }

        // Return the number of characters matched in the module name and not
        // in the file (this may not be the same because of the imaginary
        // separators).
        //
        return m.size () - mi;
      };

      auto& pts (t.prerequisite_targets);
      size_t start (pts.size ()); // Index of the first to be added.

      // We have two parallel vectors: module names/scores in imports and
      // targets in prerequisite_targets (offset with start). Pre-allocate
      // NULL entries in the latter.
      //
      size_t n (imports.size ());
      pts.resize (start + n, nullptr);

      // Oh, yes, there is one "minor" complication. It's the last one, I
      // promise. It has to do with module re-exporting (export import M;).
      // In this case (currently) all implementations simply treat it as a
      // shallow (from the BMI's point of view) reference to the module (or an
      // implicit import, if you will). Do you see where it's going? Nowever
      // good, that's right. This shallow reference means that the compiler
      // should be able to find BMIs for all the re-exported modules,
      // recursive. The good news is we are actually in a pretty good shape to
      // handle this: after match all our prerequisite BMIs will have their
      // prerequisite BMIs known, recursively. The only bit that is missing is
      // the re-export flag of some sorts. As well as deciding where to handle
      // it: here or in append_modules(). After some meditation it became
      // clear handling it here will be simpler: We need to weed out
      // duplicates for which we can re-use the imports vector. And we may
      // also need to save this "flattened" list of modules in depdb.
      //
      // Ok, so, here is the plan:
      //
      // 1. There is no good place in prerequisite_targets to store the
      //    exported flag (no, using the marking facility across match/execute
      //    is a bad idea). So what we are going to do is put re-exported
      //    bmi{}s at the back and store (in the target's data pad) the start
      //    position. One bad aspect about this part is that we assume those
      //    bmi{}s have been matched by the same rule. But let's not kid
      //    ourselves, there will be no other rule that matches bmi{}s.
      //
      // 2. Once we have matched all the bmi{}s we are importing directly
      //    (with all the re-exported by us at the back), we will go over them
      //    and copy all of their re-exported bmi{}s (using the position we
      //    saved on step #1). The end result will be a recursively-explored
      //    list of imported bmi{}s that append_modules() can simply convert
      //    to the list of options.
      //
      //    One issue with this approach is that these copied targets will be
      //    executed which means we need to adjust their dependent counts
      //    (which is normally done by match). While this seems conceptually
      //    correct (especially if you view re-exports as implicit imports),
      //    it's just extra overhead (we know they will be updated). So what
      //    we are going to do is save another position, that of the start of
      //    these copied-over targets, and will only execute up to this point.
      //
      // And after implementing this came the reality check: all the current
      // implementations require access to all the imported BMIs, not only
      // re-exported. Some (like Clang) store references to imported BMI files
      // so we actually don't need to pass any extra options (unless things
      // get moved) but they still need access to the BMIs (and things will
      // most likely have to be done differenly for distributed compilation).
      //
      // So the revised plan: on the off chance that some implementation will
      // do it differently we will continue maintaing the imported/re-exported
      // split and how much to copy-over can be made compiler specific.
      //
      // As a first sub-step of step #1, move all the re-exported imports to
      // the end of the vector. This will make sure they end up at the end
      // of prerequisite_targets. Note: the special first import, if any,
      // should be unaffected.
      //
      sort (imports.begin (), imports.end (),
            [] (const module_import& x, const module_import& y)
            {
              return !x.exported && y.exported;
            });

      // Go over the prerequisites once.
      //
      // For (direct) library prerequisites, check their prerequisite bmi{}s
      // (which should be searched and matched with module names discovered;
      // see the library meta-information protocol for details).
      //
      // For our own bmi{} prerequisites, checking if each (better) matches
      // any of the imports.

      // For fuzzy check if a file name (better) resolves any of our imports
      // and if so make it the new selection. For exact the name is the actual
      // module name and it can only resolve one import (there are no
      // duplicates).
      //
      // Set done to true if all the imports have now been resolved to actual
      // module names (which means we can stop searching). This will happens
      // if all the modules come from libraries. Which will be fairly common
      // (think of all the tests) so it's worth optimizing for.
      //
      bool done (false);

      auto check_fuzzy = [&trace, &imports, &pts, &match, start, n]
        (const target* pt, const string& name)
      {
        for (size_t i (0); i != n; ++i)
        {
          module_import& m (imports[i]);

          if (std_module (m.name)) // No fuzzy std.* matches.
            continue;

          size_t n (m.name.size ());

          if (m.score > n) // Resolved to module name.
            continue;

          size_t s (match (name, m.name));

          l5 ([&]{trace << name << " ~ " << m.name << ": " << s;});

          if (s > m.score)
          {
            pts[start + i] = pt;
            m.score = s;
          }
        }
      };

      // If resolved, return the "slot" in pts (we don't want to create a
      // side build until we know we match; see below for details).
      //
      auto check_exact = [&trace, &imports, &pts, start, n, &done]
        (const string& name) -> const target**
      {
        const target** r (nullptr);
        done = true;

        for (size_t i (0); i != n; ++i)
        {
          module_import& m (imports[i]);

          size_t n (m.name.size ());

          if (m.score > n) // Resolved to module name (no effect on done).
            continue;

          if (r == nullptr)
          {
            size_t s (name == m.name ? n + 1 : 0);

            l5 ([&]{trace << name << " ~ " << m.name << ": " << s;});

            if (s > m.score)
            {
              r = &pts[start + i].target;
              m.score = s;
              continue; // Scan the rest to detect if all done.
            }
          }

          done = false;
        }

        return r;
      };

      for (prerequisite_member p: group_prerequisite_members (act, t))
      {
        const target* pt (p.load ()); // Should be cached for libraries.

        if (pt != nullptr)
        {
          const target* lt (nullptr);

          if (const libx* l = pt->is_a<libx> ())
            lt = &link_member (*l, act, li);
          else if (pt->is_a<liba> () || pt->is_a<libs> () || pt->is_a<libux> ())
            lt = pt;

          // If this is a library, check its bmi{}s and mxx{}s.
          //
          if (lt != nullptr)
          {
            for (const target* bt: lt->prerequisite_targets)
            {
              if (bt == nullptr)
                continue;

              // Note that here we (try) to use whatever flavor of bmi*{} is
              // available.
              //
              // @@ MOD: BMI compatibility check.
              // @@ UTL: we need to (recursively) see through libux{} (and
              //    also in pkgconfig_save()).
              //
              if (bt->is_a<bmis> () ||
                  bt->is_a<bmia> () ||
                  bt->is_a<bmie> ())
              {
                const string& n (cast<string> (bt->vars[c_module_name]));

                if (const target** p = check_exact (n))
                  *p = bt;
              }
              else if (bt->is_a (*x_mod))
              {
                // This is an installed library with a list of module sources
                // (the source are specified as prerequisites but the fallback
                // file rule puts them into prerequisite_targets for us).
                //
                // The module names should be specified but if not assume
                // something else is going on and ignore.
                //
                const string* n (cast_null<string> (bt->vars[c_module_name]));
                if (n == nullptr)
                  continue;

                if (const target** p = check_exact (*n))
                  *p = &make_module_sidebuild (act, bs, *lt, *bt, *n);
              }
              else
                continue;

              if (done)
                break;
            }

            if (done)
              break;

            continue;
          }

          // Fall through.
        }

        // While it would have been even better not to search for a target, we
        // need to get hold of the corresponding mxx{} (unlikely but possible
        // for bmi{} to have a different name).
        //
        if (p.is_a<bmi> ())
          pt = &search (t, mtt, p.key ()); // Same logic as in picking obj*{}.
        else if (p.is_a (mtt))
        {
          if (pt == nullptr)
            pt = &p.search (t);
        }
        else
          continue;

        // Find the mxx{} prerequisite and extract its "file name" for the
        // fuzzy match unless the user specified the module name explicitly.
        //
        for (prerequisite_member p: group_prerequisite_members (act, *pt))
        {
          if (p.is_a (*x_mod))
          {
            // Check for an explicit module name. Only look for an existing
            // target (which means the name can only be specified on the
            // target itself, no target type/pattern-spec).
            //
            const target* t (p.search_existing ());
            const string* n (t != nullptr
                             ? cast_null<string> (t->vars[c_module_name])
                             : nullptr);
            if (n != nullptr)
            {
              if (const target** p = check_exact (*n))
                *p = pt;
            }
            else
            {
              // Fuzzy match.
              //
              string f;

              // Add the directory part if it is relative. The idea is to
              // include it into the module match, say hello.core vs
              // hello/mxx{core}.
              //
              // @@ MOD: Why not for absolute? Good question. What if it
              // contains special components, say, ../mxx{core}?
              //
              const dir_path& d (p.dir ());

              if (!d.empty () && d.relative ())
                f = d.representation (); // Includes trailing slash.

              f += p.name ();
              check_fuzzy (pt, f);
            }
            break;
          }
        }

        if (done)
          break;
      }

      // Diagnose unresolved modules.
      //
      if (!done)
      {
        for (size_t i (0); i != n; ++i)
        {
          if (pts[start + i] == nullptr && !std_module (imports[i].name))
          {
            // It would have been nice to print the location of the import
            // declaration. And we could save it during parsing at the expense
            // of a few paths (that can be pooled). The question is what to do
            // when we re-create this information from depdb? We could have
            // saved the location information there but the relative paths
            // (e.g., from the #line directives) could end up being wrong if
            // the we re-run from a different working directory.
            //
            // It seems the only workable approach is to extract full location
            // info during parse, not save it in depdb, when re-creating,
            // fallback to just src path without any line/column information.
            // This will probably cover the majority of case (most of the time
            // it will be a misspelled module name, not a removal of module
            // from buildfile).
            //
            // But at this stage this doesn't seem worth the trouble.
            //
            fail (relative (src)) << "unable to resolve module "
                                  << imports[i].name;
          }
        }
      }

      // Match in parallel and wait for completion.
      //
      match_members (act, t, pts, start);

      // Post-process the list of our (direct) imports. While at it, calculate
      // the checksum of all (direct and indirect) bmi{} paths.
      //
      size_t exported (n);
      size_t copied (pts.size ());

      for (size_t i (0); i != n; ++i)
      {
        const module_import& m (imports[i]);

        // Determine the position of the first re-exported bmi{}.
        //
        if (m.exported && exported == n)
          exported = i;

        const target* bt (pts[start + i]);

        if (bt == nullptr)
          continue; // Unresolved (std.*).

        // Verify our guesses against extracted module names but don't waste
        // time if it was a match against the actual module name.
        //
        const string& in (m.name);

        if (m.score <= in.size ())
        {
          const string& mn (cast<string> (bt->vars[c_module_name]));

          if (in != mn)
          {
            for (prerequisite_member p: group_prerequisite_members (act, *bt))
            {
              if (p.is_a (*x_mod)) // Got to be there.
              {
                fail (relative (src)) << "failed to correctly guess module "
                                      << "name from " << p <<
                  info << "guessed: " << in <<
                  info << "actual:  " << mn <<
                  info << "consider adjusting module interface file names or" <<
                  info << "consider specifying module name with cc.module_name";
              }
            }
          }
        }

        // Hash (we know it's a file).
        //
        cs.append (static_cast<const file&> (*bt).path ().string ());

        // Copy over bmi{}s from our prerequisites weeding out duplicates.
        //
        if (size_t j = bt->data<match_data> ().mods.start)
        {
          // Hard to say whether we should reserve or not. We will probably
          // get quite a bit of duplications.
          //
          for (size_t m (bt->prerequisite_targets.size ()); j != m; ++j)
          {
            const target* et (bt->prerequisite_targets[j]);

            if (et == nullptr)
              continue; // Unresolved (std.*).

            const string& mn (cast<string> (et->vars[c_module_name]));

            if (find_if (imports.begin (), imports.end (),
                         [&mn] (const module_import& i)
                         {
                           return i.name == mn;
                         }) == imports.end ())
            {
              pts.push_back (et);
              cs.append (static_cast<const file&> (*et).path ().string ());

              // Add to the list of imports for further duplicate suppression.
              // We could have probably stored reference to the name (e.g., in
              // score) but it's probably not worth it if we have a small
              // string optimization.
              //
              imports.push_back (module_import {mn, true, 0});
            }
          }
        }
      }

      if (copied == pts.size ()) // No copied tail.
        copied = 0;

      if (exported == n) // No (own) re-exported imports.
        exported = copied;
      else
        exported += start; // Rebase.

      return module_positions {start, exported, copied};
    }

    // Synthesize a dependency for building a module binary interface on
    // the side.
    //
    const target& compile::
    make_module_sidebuild (action act,
                           const scope& bs,
                           const target& lt,
                           const target& mt,
                           const string& mn) const
    {
      tracer trace (x, "compile::make_module_sidebuild");

      // First figure out where we are going to build. We want to avoid
      // multiple sidebuilds so the outermost scope that has loaded the module
      // capable of compiling things and that is within our amalgmantion seems
      // like a good place.
      //
      // @@ TODO: this is actually pretty restrictive: we need cxx and with
      // modules enabled! Which means things like bpkg configurations won't
      // work (only loads cc.config).
      //
      const scope* as (bs.root_scope ());
      {
        const scope* ws (as->weak_scope ());
        if (as != ws)
        {
          const scope* s (as);
          do
          {
            s = s->parent_scope ()->root_scope ();

            const module* m (s->modules.lookup<module> ("cxx"));
            if (m != nullptr && m->modules)
              as = s;

          } while (s != ws);
        }
      }

      // Next we need to come up with a file/target name that will be unique
      // enough not to conflict with other modules. If we assume that within
      // an amalgamation there is only one "version" of each module, then the
      // module name itself seems like a good fit. We just replace '.' with
      // '-'.
      //
      string mf;
      transform (mn.begin (), mn.end (),
                 back_inserter (mf),
                 [] (char c) {return c == '.' ? '-' : c;});

      // Store the BMI target in the build/<mod>/modules/ subdirectory.
      //
      dir_path md (as->out_path ());
      md /= "build";
      md /= x;
      md /= "modules";

      // It seems natural to build a BMI type that corresponds to the library
      // type. After all, this is where the object file part of the BMI is
      // going to come from (though things will probably be different for
      // module-only libraries).
      //
      const target_type* tt (nullptr);
      switch (link_type (lt).type)
      {
      case otype::a: tt = &bmia::static_type; break;
      case otype::s: tt = &bmis::static_type; break;
      case otype::e: assert (false);
      }

      // If the target already exists then we assume all this is already done
      // (otherwise why would someone have created such a target).
      //
      if (const target* bt = targets.find (
            *tt,
            md,
            dir_path (), // Always in the out tree.
            mf,
            nullopt,     // Use default extension.
            trace))
        return *bt;

      // Make sure the output directory exists. This is not strictly necessary
      // if out != src since inject_fsdir() will take care of it. For out ==
      // src we initially tried to add an explicit fsdir{} preprequisite but
      // that didn't work out since this is a nested directory. So now we keep
      // it simple and just create it. The proper way to handle this as well
      // as cleanup is probably at the cxx module level, which is @@ TODO.
      //
      mkdir_p (md, 3);

      prerequisites ps;
      ps.push_back (prerequisite (mt));

      // We've added the mxx{} but it may import other modules from this
      // library. Or from (direct) dependencies of this library. We add them
      // all as prerequisites so that the standard module search logic can
      // sort things out. This is pretty similar to what we do in link when
      // synthesizing dependencies for bmi{}'s.
      //
      ps.push_back (prerequisite (lt));
      for (prerequisite_member p: group_prerequisite_members (act, lt))
      {
        // @@ TODO: will probably need revision if using sidebuild for
        //    non-installed libraries (e.g., direct BMI dependencies
        //    will probably have to be translated to mxx{} or some such).
        //
        if (p.is_a<libx> () ||
            p.is_a<liba> () || p.is_a<libs> () || p.is_a<libux> ())
        {
          ps.push_back (p.as_prerequisite ());
        }
      }

      auto p (targets.insert_locked (*tt,
                                     move (md),
                                     dir_path (), // Always in the out tree.
                                     move (mf),
                                     nullopt,     // Use default extension.
                                     true,        // Implied.
                                     trace));
      const target& bt (p.first);

      // Note that this is racy and someone might have created this target
      // while we were preparing the prerequisite list.
      //
      if (p.second.owns_lock ())
        bt.prerequisites (move (ps));

      return bt;
    }

    // Filter cl.exe noise (msvc.cxx).
    //
    void
    msvc_filter_cl (ifdstream&, const path& src);

    void compile::
    append_modules (environment& env,
                    cstrings& args,
                    strings& stor,
                    const file& t,
                    const match_data& md) const
    {
      const module_positions& ms (md.mods);
      assert (ms.start != 0);

      dir_path stdifc; // See the VC case below.

#if 0
      switch (cid)
      {
      case compiler_id::gcc:
        {
          // Use the module map stored in depdb.
          //
          string s (relative (md.dd).string ());
          s.insert (0, "-fmodule-file-map=@=");
          stor.push_back (move (s));
          break;
        }
      case compiler_id::clang:
        {
          // In Clang the module implementation's unit .pcm is special and
          // must be "loaded".
          //
          if (md.type == translation_type::module_impl)
          {
            const file& f (t.prerequisite_targets[ms.start]->as<file> ());
            string s (relative (f.path ()).string ());
            s.insert (0, "-fmodule-file=");
            stor.push_back (move (s));
          }

          // Use the module map stored in depdb for others.
          //
          string s (relative (md.dd).string ());
          s.insert (0, "-fmodule-file-map=@=");
          stor.push_back (move (s));
          break;
        }
      case compiler_id::msvc:
        {
          for (size_t i (ms.start), n (t.prerequisite_targets.size ());
               i != n;
               ++i)
          {
            const target* pt (t.prerequisite_targets[i]);

            if (pt == nullptr)
              continue;

            // Here we use whatever bmi type has been added. And we know all
            // of these are bmi's.
            //
            const file& f (pt->as<file> ());

            // In VC std.* modules can only come from a single directory
            // specified with the IFCPATH environment variable or the
            // /module:stdIfcDir option.
            //
            if (std_module (cast<string> (f.vars[c_module_name])))
            {
              dir_path d (f.path ().directory ());

              if (stdifc.empty ())
              {
                // Go one directory up since /module:stdIfcDir will look in
                // either Release or Debug subdirectories. Keeping the result
                // absolute feels right.
                //
                stor.push_back ("/module:stdIfcDir");
                stor.push_back (d.directory ().string ());
                stdifc = move (d);
              }
              else if (d != stdifc) // Absolute and normalized.
                fail << "multiple std.* modules in different directories";
            }
            else
            {
              stor.push_back ("/module:reference");
              stor.push_back (relative (f.path ()).string ());
            }
          }
          break;
        }
      case compiler_id::icc:
        assert (false);
      }
#else
      size_t n (t.prerequisite_targets.size ());

      // Clang embeds module file references so we only need to specify
      // our direct imports.
      //
      // If/when we get the ability to specify the mapping in a file, we
      // should probably pass the whole list.
      //
      switch (cid)
      {
      case compiler_id::gcc:   break; // All of them.
      case compiler_id::clang: n = ms.copied != 0 ? ms.copied : n; break;
      case compiler_id::msvc:  break; // All of them.
      case compiler_id::icc:   assert (false);
      }

      for (size_t i (ms.start); i != n; ++i)
      {
        const target* pt (t.prerequisite_targets[i]);

        if (pt == nullptr)
          continue;

        // Here we use whatever bmi type has been added. And we know all of
        // these are bmi's.
        //
        const file& f (pt->as<file> ());
        string s (relative (f.path ()).string ());

        switch (cid)
        {
        case compiler_id::gcc:
          {
            s.insert (0, 1, '=');
            s.insert (0, cast<string> (f.vars[c_module_name]));
            s.insert (0, "-fmodule-file=");
            break;
          }
        case compiler_id::clang:
          {
            // In Clang the module implementation's unit .pcm is special and
            // must be "loaded".
            //
            if (md.type == translation_type::module_impl && i == ms.start)
              s.insert (0, "-fmodule-file=");
            else
            {
              s.insert (0, 1, '=');
              s.insert (0, cast<string> (f.vars[c_module_name]));
              s.insert (0, "-fmodule-file=");
            }
            break;
          }
        case compiler_id::msvc:
          {
            // In VC std.* modules can only come from a single directory
            // specified with the IFCPATH environment variable or the
            // /module:stdIfcDir option.
            //
            if (std_module (cast<string> (f.vars[c_module_name])))
            {
              dir_path d (f.path ().directory ());

              if (stdifc.empty ())
              {
                // Go one directory up since /module:stdIfcDir will look in
                // either Release or Debug subdirectories. Keeping the result
                // absolute feels right.
                //
                s = d.directory ().string ();
                stor.push_back ("/module:stdIfcDir");
                stdifc = move (d);
              }
              else
              {
                if (d != stdifc) // Absolute and normalized.
                  fail << "multiple std.* modules in different directories";

                continue; // Skip.
              }
            }
            else
              stor.push_back ("/module:reference");

            break;
          }
        case compiler_id::icc:
          assert (false);
        }

        stor.push_back (move (s));
      }
#endif

      // Shallow-copy storage to args. Why not do it as we go along pushing
      // into storage? Because of potential reallocations.
      //
      for (const string& a: stor)
        args.push_back (a.c_str ());

      // VC's IFCPATH takes precedence over /module:stdIfcDir so unset it
      // if we are using our own std modules.
      //
      if (!stdifc.empty ())
        env.push_back ("IFCPATH");
    }

    target_state compile::
    perform_update (action act, const target& xt) const
    {
      const file& t (xt.as<file> ());
      const path& tp (t.path ());

      match_data md (move (t.data<match_data> ()));
      bool mod (md.type == translation_type::module_iface);

      // While all our prerequisites are already up-to-date, we still have to
      // execute them to keep the dependency counts straight. Actually, no, we
      // may also have to update the modules.
      //
      auto pr (
        execute_prerequisites<file> (
          (mod ? *x_mod : x_src),
          act, t,
          md.mt,
          [s = md.mods.start] (const target&, size_t i)
          {
            return s != 0 && i >= s; // Only compare timestamps for modules.
          },
          md.mods.copied)); // See search_modules() for details.

      const file& s (pr.second);
      const path* sp (&s.path ());

      if (pr.first)
      {
        if (md.touch)
        {
          touch (tp, false, 2);
          skip_count.fetch_add (1, memory_order_relaxed);
        }

        t.mtime (md.mt);
        return *pr.first;
      }

      // Make sure depdb is no older than any of our prerequisites.
      //
      touch (md.dd, false, verb_never);

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      otype ot (compile_type (t, mod));
      linfo li (link_info (bs, ot));

      environment env;
      cstrings args {cpath.recall_string ()};

      // If we are building a module, then the target is bmi*{} and its ad hoc
      // member is obj*{}.
      //
      path relo, relm;
      if (mod)
      {
        relm = relative (tp);
        relo = relative (t.member->is_a<file> ()->path ());
      }
      else
        relo = relative (tp);

      // Build the command line.
      //
      if (md.pp != preprocessed::all)
      {
        append_options (args, t, c_poptions);
        append_options (args, t, x_poptions);

        // Add *.export.poptions from prerequisite libraries.
        //
        append_lib_options (bs, args, t, act, li);

        // Extra system header dirs (last).
        //
        assert (sys_inc_dirs_extra <= sys_inc_dirs.size ());
        append_option_values (
          args, "-I",
          sys_inc_dirs.begin () + sys_inc_dirs_extra, sys_inc_dirs.end (),
          [] (const dir_path& d) {return d.string ().c_str ();});

        if (md.symexport)
          append_symexport_options (args, t);
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

        if (md.mods.start != 0)
          append_modules (env, args, mods, t, md);

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

        args.push_back ("/c");                   // Compile only.
        args.push_back (langopt (md));           // Compile as.
        args.push_back (sp->string ().c_str ()); // Note: relied on being last.
      }
      else
      {
        if (ot == otype::s)
        {
          // On Darwin, Win32 -fPIC is the default.
          //
          if (tclass == "linux" || tclass == "bsd")
            args.push_back ("-fPIC");
        }

        if (md.mods.start != 0)
          append_modules (env, args, mods, t, md);

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

              // Without this option Clang's .pcm will reference source files.
              // In our case this file may be transient (.ii). Plus, it won't
              // play nice with distributed compilation.
              //
              args.push_back ("-Xclang");
              args.push_back ("-fmodules-embed-all-files");

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

        args.push_back (sp->string ().c_str ());
      }

      args.push_back (nullptr);

      if (!env.empty ())
        env.push_back (nullptr);

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
      bool psrc (!md.psrc.path.empty ());
      bool pact (md.psrc.active);
      if (psrc)
      {
        args.pop_back (); // nullptr
        args.pop_back (); // sp

        sp = &md.psrc.path;

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

        args.push_back (sp->string ().c_str ());
        args.push_back (nullptr);

        // Let's keep the preprocessed file in case of an error but only at
        // verbosity level 3 and up (when one actually sees it mentioned on
        // the command line). We also have to re-arm on success (see below).
        //
        if (pact && verb >= 3)
          md.psrc.active = false;
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

        process pr (cpath,
                    args.data (),
                    0, (filter ? -1 : 2), 2,
                    nullptr, // CWD
                    env.empty () ? nullptr : env.data ());

        if (filter)
        {
          try
          {
            ifdstream is (
              move (pr.in_ofd), fdstream_mode::text, ifdstream::badbit);

            msvc_filter_cl (is, *sp);

            // If anything remains in the stream, send it all to stderr. Note
            // that the eof check is important: if the stream is at eof, this
            // and all subsequent writes to the diagnostics stream will fail
            // (and you won't see a thing).
            //
            if (is.peek () != ifdstream::traits_type::eof ())
              diag_stream_lock () << is.rdbuf ();

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

      if (pact && verb >= 3)
        md.psrc.active = true;

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
        args.push_back ("-Wno-unused-command-line-argument");
        args.push_back (relm.string ().c_str ());
        args.push_back (nullptr);

        if (verb >= 2)
          print_process (args);

        try
        {
          process pr (cpath,
                      args.data (),
                      0, 2, 2,
                      nullptr, // CWD
                      env.empty () ? nullptr : env.data ());

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
