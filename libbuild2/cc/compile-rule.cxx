// file      : libbuild2/cc/compile-rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/compile-rule.hxx>

#include <cstdlib>  // exit()
#include <cstring>  // strlen(), strchr()

#include <libbuild2/file.hxx>
#include <libbuild2/depdb.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>  // mtime()
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/target.hxx>

#include <libbuild2/cc/parser.hxx>
#include <libbuild2/cc/target.hxx>  // h
#include <libbuild2/cc/module.hxx>
#include <libbuild2/cc/utility.hxx>

using std::exit;
using std::strlen;

using namespace butl;

namespace build2
{
  namespace cc
  {
    using namespace bin;

    // Module type/info string serialization.
    //
    // The string representation is a space-separated list of module names
    // or quoted paths for header units with the following rules:
    //
    // 1. If this is a module unit, then the first name is the module name
    //    intself following by either '!' for an interface, interface
    //    partition, or header unit and by '+' for an implementation or
    //    implementation partition unit.
    //
    // 2. If an imported module is re-exported, then the module name is
    //    followed by '*'.
    //
    // For example:
    //
    // foo! foo.core* foo.base* foo:intf! foo.impl
    // foo.base+ foo.impl
    // foo.base foo.impl
    // foo:impl+
    // foo:intf! foo:impl
    // "/usr/include/stdio.h"!
    // "/usr/include/stdio.h"! "/usr/include/stddef.h"
    //
    // NOTE: currently we omit the imported header units since we have no need
    //       for this information (everything is handled by the mapper). Plus,
    //       resolving an import declaration to an absolute path would require
    //       some effort.
    //
    static string
    to_string (unit_type ut, const module_info& mi)
    {
      string s;

      if (ut != unit_type::non_modular)
      {
        if (ut == unit_type::module_header) s += '"';
        s += mi.name;
        if (ut == unit_type::module_header) s += '"';

        s += (ut == unit_type::module_impl ||
              ut == unit_type::module_impl_part ? '+' : '!');
      }

      for (const module_import& i: mi.imports)
      {
        if (!s.empty ())
          s += ' ';

        if (i.type == import_type::module_header) s += '"';
        s += i.name;
        if (i.type == import_type::module_header) s += '"';

        if (i.exported)
          s += '*';
      }

      return s;
    }

    static unit_type
    to_module_info (const string& s, module_info& mi)
    {
      unit_type ut (unit_type::non_modular);

      for (size_t b (0), e (0), n (s.size ()), m; e < n; )
      {
        // Let's handle paths with spaces seeing that we already quote them.
        //
        char d (s[b = e] == '"' ? '"' : ' ');

        if ((m = next_word (s, n, b, e, d)) == 0)
          break;

        char c (d == ' '  ? s[e - 1] : // Before delimiter.
                e + 1 < n ? s[e + 1] : // After delimiter.
                '\0');

        switch (c)
        {
        case '!':
        case '+':
        case '*': break;
        default:  c = '\0';
        }

        string w (s, b, m - (d == ' ' && c != '\0' ? 1 : 0));

        if (c == '!' || c == '+')
        {
          if (d == ' ')
          {
            ut = w.find (':') != string::npos
              ? (c == '!'
                 ? unit_type::module_intf_part
                 : unit_type::module_impl_part)
              : (c == '!'
                 ? unit_type::module_intf
                 : unit_type::module_impl);
          }
          else
            ut = unit_type::module_header;

          mi.name = move (w);
        }
        else
        {
          import_type t (d == ' '
                         ? (w.find (':') != string::npos
                            ? import_type::module_part
                            : import_type::module_intf)
                         : import_type::module_header);

          mi.imports.push_back (module_import {t, move (w), c == '*', 0});
        }

        // Skip to the next word (quote and space or just space).
        //
        e += (d == '"' ? 2 : 1);
      }

      return ut;
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

    optional<path> compile_rule::
    find_system_header (const path& f) const
    {
      path p; // Reuse the buffer.
      for (const dir_path& d: sys_hdr_dirs)
      {
        if (file_exists ((p = d, p /= f),
                         true /* follow_symlinks */,
                         true /* ignore_errors */))
          return p;
      }

      return nullopt;
    }

    struct compile_rule::match_data
    {
      explicit
      match_data (unit_type t, const prerequisite_member& s)
          : type (t), src (s) {}

      unit_type type;
      preprocessed pp = preprocessed::none;
      bool deferred_failure = false;        // Failure deferred to compilation.
      bool symexport = false;               // Target uses __symexport.
      bool touch = false;                   // Target needs to be touched.
      timestamp mt = timestamp_unknown;     // Target timestamp.
      prerequisite_member src;
      file_cache::entry psrc;               // Preprocessed source, if any.
      path dd;                              // Dependency database path.
      size_t header_units = 0;              // Number of imported header units.
      module_positions modules = {0, 0, 0}; // Positions of imported modules.
    };

    compile_rule::
    compile_rule (data&& d)
        : common (move (d)),
          rule_id (string (x) += ".compile 5")
    {
      static_assert (sizeof (match_data) <= target::data_size,
                     "insufficient space");
    }

    template <typename T>
    void compile_rule::
    append_sys_hdr_options (T& args) const
    {
      assert (sys_hdr_dirs_extra <= sys_hdr_dirs.size ());

      // Note that the mode options are added as part of cmode.
      //
      auto b (sys_hdr_dirs.begin () + sys_hdr_dirs_mode);
      auto m (sys_hdr_dirs.begin () + sys_hdr_dirs_extra);
      auto e (sys_hdr_dirs.end ());

      // Note: starting from 15.6, MSVC gained /external:I option though it
      // doesn't seem to affect the order, only "system-ness".
      //
      append_option_values (
        args,
        cclass == compiler_class::gcc  ? "-idirafter" :
        cclass == compiler_class::msvc ? "/I" : "-I",
        m, e,
        [] (const dir_path& d) {return d.string ().c_str ();});

      // For MSVC if we have no INCLUDE environment variable set, then we
      // add all of them. But we want extras to come first. Note also that
      // clang-cl takes care of this itself.
      //
      if (ctype == compiler_type::msvc && cvariant != "clang")
      {
        if (!getenv ("INCLUDE"))
        {
          append_option_values (
            args, "/I",
            b, m,
            [] (const dir_path& d) {return d.string ().c_str ();});
        }
      }
    }

    size_t compile_rule::
    append_lang_options (cstrings& args, const match_data& md) const
    {
      size_t r (args.size ());

      // Normally there will be one or two options/arguments.
      //
      const char* o1 (nullptr);
      const char* o2 (nullptr);

      switch (cclass)
      {
      case compiler_class::msvc:
        {
          switch (x_lang)
          {
          case lang::c:   o1 = "/TC"; break;
          case lang::cxx: o1 = "/TP"; break;
          }
          break;
        }
      case compiler_class::gcc:
        {
          // For GCC we ignore the preprocessed value since it is handled via
          // -fpreprocessed -fdirectives-only.
          //
          // Clang has *-cpp-output (but not c++-module-cpp-output) and they
          // handle comments and line continuations. However, currently this
          // is only by accident since these modes are essentially equivalent
          // to their cpp-output-less versions.
          //
          switch (md.type)
          {
          case unit_type::non_modular:
          case unit_type::module_impl:
            {
              o1 = "-x";
              switch (x_lang)
              {
              case lang::c:   o2 = "c";   break;
              case lang::cxx: o2 = "c++"; break;
              }
              break;
            }
          case unit_type::module_intf:
          case unit_type::module_intf_part:
          case unit_type::module_impl_part:
          case unit_type::module_header:
            {
              // Here things get rather compiler-specific. We also assume
              // the language is C++.
              //
              bool h (md.type == unit_type::module_header);

              //@@ MODHDR TODO: should we try to distinguish c-header vs
              //   c++-header based on the source target type?

              switch (ctype)
              {
              case compiler_type::gcc:
                {
                  // In GCC compiling a header unit required -fmodule-header
                  // in addition to -x c/c++-header. Probably because relying
                  // on just -x would be ambigous with its PCH support.
                  //
                  if (h)
                    args.push_back ("-fmodule-header");

                  o1 = "-x";
                  o2 = h ? "c++-header" : "c++";
                  break;
                }
              case compiler_type::clang:
                {
                  o1 = "-x";
                  o2 =  h ? "c++-header" : "c++-module";
                  break;
                }
              default:
                  assert (false);
              }
              break;
            }
          }
          break;
        }
      }

      if (o1 != nullptr) args.push_back (o1);
      if (o2 != nullptr) args.push_back (o2);

      return args.size () - r;
    }

    inline void compile_rule::
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

    bool compile_rule::
    match (action a, target& t, const string&) const
    {
      tracer trace (x, "compile_rule::match");

      // Note: unit type will be refined in apply().
      //
      unit_type ut (t.is_a<hbmix> () ? unit_type::module_header :
                    t.is_a<bmix> ()  ? unit_type::module_intf  :
                    unit_type::non_modular);

      // Link-up to our group (this is the obj/bmi{} target group protocol
      // which means this can be done whether we match or not).
      //
      if (t.group == nullptr)
        t.group = &search (t,
                           (ut == unit_type::module_header ? hbmi::static_type:
                            ut == unit_type::module_intf   ? bmi::static_type :
                            obj::static_type),
                           t.dir, t.out, t.name);

      // See if we have a source file. Iterate in reverse so that a source
      // file specified for a member overrides the one specified for the
      // group. Also "see through" groups.
      //
      for (prerequisite_member p: reverse_group_prerequisite_members (a, t))
      {
        // If excluded or ad hoc, then don't factor it into our tests.
        //
        if (include (a, t, p) != include_type::normal)
          continue;

        // For a header unit we check the "real header" plus the C header.
        //
        if (ut == unit_type::module_header ? p.is_a (**x_hdr) || p.is_a<h> () :
            ut == unit_type::module_intf   ? p.is_a (*x_mod)                  :
            p.is_a (x_src))
        {
          // Save in the target's auxiliary storage.
          //
          t.data (match_data (ut, p));
          return true;
        }
      }

      l4 ([&]{trace << "no " << x_lang << " source file for target " << t;});
      return false;
    }

    // Append or hash library options from a pair of *.export.* variables
    // (first is x.* then cc.*) recursively, prerequisite libraries first.
    //
    template <typename T>
    void compile_rule::
    append_library_options (appended_libraries& ls, T& args,
                            const scope& bs,
                            action a, const file& l, bool la, linfo li) const
    {
      struct data
      {
        appended_libraries& ls;
        T& args;
      } d {ls, args};

      // See through utility libraries.
      //
      auto imp = [] (const target& l, bool la) {return la && l.is_a<libux> ();};

      auto opt = [&d, this] (const target& lt,
                             const string& t, bool com, bool exp)
      {
        const file& l (lt.as<file> ());

        // Note that in our model *.export.poptions are always "interface",
        // even if set on liba{}/libs{}, unlike loptions.
        //
        if (!exp) // Ignore libux.
          return;

        // Suppress duplicates.
        //
        // Compilation is the simple case: we can add the options on the first
        // occurrence of the library and ignore all subsequent occurrences.
        // See GitHub issue #114 for details.
        //
        if (find (d.ls.begin (), d.ls.end (), &l) != d.ls.end ())
          return;

        const variable& var (
          com
          ? c_export_poptions
          : (t == x
             ? x_export_poptions
             : l.ctx.var_pool[t + ".export.poptions"]));

        append_options (d.args, l, var);

        // From the process_libraries() semantics we know that the final call
        // is always for the common options.
        //
        if (com)
          d.ls.push_back (&l);
      };

      process_libraries (a, bs, li, sys_lib_dirs,
                         l, la, 0, // lflags unused.
                         imp, nullptr, opt);
    }

    void compile_rule::
    append_library_options (appended_libraries& ls, strings& args,
                            const scope& bs,
                            action a, const file& l, bool la, linfo li) const
    {
      append_library_options<strings> (ls, args, bs, a, l, la, li);
    }

    template <typename T>
    void compile_rule::
    append_library_options (T& args,
                            const scope& bs,
                            action a, const target& t, linfo li) const
    {
      appended_libraries ls;

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
          continue;

        // Should be already searched and matched for libraries.
        //
        if (const target* pt = p.load ())
        {
          if (const libx* l = pt->is_a<libx> ())
            pt = link_member (*l, a, li);

          bool la;
          const file* f;
          if ((la = (f = pt->is_a<liba> ()))  ||
              (la = (f = pt->is_a<libux> ())) ||
              (     (f = pt->is_a<libs> ())))
          {
            append_library_options (ls, args, bs, a, *f, la, li);
          }
        }
      }
    }

    // Append library prefixes based on the *.export.poptions variables
    // recursively, prerequisite libraries first.
    //
    void compile_rule::
    append_library_prefixes (prefix_map& m,
                             const scope& bs,
                             action a,
                             target& t,
                             linfo li) const
    {
      auto imp = [] (const target& l, bool la) {return la && l.is_a<libux> ();};

      auto opt = [&m, this] (
        const target& l, const string& t, bool com, bool exp)
      {
        if (!exp)
          return;

        const variable& var (
          com
          ? c_export_poptions
          : (t == x
             ? x_export_poptions
             : l.ctx.var_pool[t + ".export.poptions"]));

        append_prefixes (m, l, var);
      };

      // The same logic as in append_library_options().
      //
      const function<bool (const target&, bool)> impf (imp);
      const function<void (const target&, const string&, bool, bool)> optf (opt);

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
          continue;

        if (const target* pt = p.load ())
        {
          if (const libx* l = pt->is_a<libx> ())
            pt = link_member (*l, a, li);

          bool la;
          if (!((la = pt->is_a<liba> ())  ||
                (la = pt->is_a<libux> ()) ||
                pt->is_a<libs> ()))
            continue;

          process_libraries (a, bs, li, sys_lib_dirs,
                             pt->as<file> (), la, 0, // lflags unused.
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
    update (tracer& trace, action a, const target& t, timestamp ts)
    {
      const path_target* pt (t.is_a<path_target> ());

      if (pt == nullptr)
        ts = timestamp_unknown;

      target_state os (t.matched_state (a));

      if (os == target_state::unchanged)
      {
        if (ts == timestamp_unknown)
          return false;
        else
        {
          // We expect the timestamp to be known (i.e., existing file).
          //
          timestamp mt (pt->mtime ());
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
        phase_switch ps (t.ctx, run_phase::execute);
        target_state ns (execute_direct (a, t));

        if (ns != os && ns != target_state::unchanged)
        {
          l6 ([&]{trace << "updated " << t
                        << "; old state " << os
                        << "; new state " << ns;});
          return true;
        }
        else
          return ts != timestamp_unknown ? pt->newer (ts, ns) : false;
      }
    }

    recipe compile_rule::
    apply (action a, target& xt) const
    {
      tracer trace (x, "compile_rule::apply");

      file& t (xt.as<file> ()); // Either obj*{} or bmi*{}.

      match_data& md (t.data<match_data> ());

      context& ctx (t.ctx);

      // Note: until refined below, non-BMI-generating translation unit is
      // assumed non-modular.
      //
      unit_type ut (md.type);

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      otype ot (compile_type (t, ut));
      linfo li (link_info (bs, ot)); // Link info for selecting libraries.
      compile_target_types tts (compile_types (ot));

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

        switch (ctype)
        {
        case compiler_type::gcc:
          {
            e += (ut != unit_type::non_modular ? "gcm" : o);
            break;
          }
        case compiler_type::clang:
          {
            e += (ut != unit_type::non_modular ? "pcm" : o);
            break;
          }
        case compiler_type::msvc:
          {
            e += (ut != unit_type::non_modular ? "ifc" : o);
            break;
          }
        case compiler_type::icc:
          {
            assert (ut == unit_type::non_modular);
            e += o;
          }
        }

        // If we are compiling a BMI-producing module TU, then add obj*{} an
        // ad hoc member of bmi*{} unless we only need the BMI (see
        // config_data::b_binless for details).
        //
        // For now neither GCC nor Clang produce an object file for a header
        // unit (but something tells me this might change).
        //
        // Note: ut is still unrefined.
        //
        if (ut == unit_type::module_intf && cast_true<bool> (t[b_binless]))
        {
          // The module interface unit can be the same as an implementation
          // (e.g., foo.mxx and foo.cxx) which means obj*{} targets could
          // collide. So we add the module extension to the target name.
          //
          file& obj (add_adhoc_member<file> (t, tts.obj, e.c_str ()));

          if (obj.path ().empty ())
            obj.derive_path (o);
        }
      }

      const path& tp (t.derive_path (e.c_str ()));

      // Inject dependency on the output directory.
      //
      const fsdir* dir (inject_fsdir (a, t));

      // Match all the existing prerequisites. The injection code takes care
      // of the ones it is adding.
      //
      // When cleaning, ignore prerequisites that are not in the same or a
      // subdirectory of our project root.
      //
      auto& pts (t.prerequisite_targets[a]);
      optional<dir_paths> usr_lib_dirs; // Extract lazily.

      // Start asynchronous matching of prerequisites. Wait with unlocked
      // phase to allow phase switching.
      //
      wait_guard wg (ctx, ctx.count_busy (), t[a].task_count, true);

      target_state src_ts1 (target_state::unknown), src_ts2 (src_ts1);

      size_t src_i (~0);          // Index of src target.
      size_t start (pts.size ()); // Index of the first to be added.
      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        const target* pt (nullptr);
        include_type  pi (include (a, t, p));

        if (!pi)
          continue;

        // A dependency on a library is there so that we can get its
        // *.export.poptions, modules, importable headers, etc. This is the
        // library metadata protocol. See also append_library_options().
        //
        if (pi == include_type::normal &&
            (p.is_a<libx> () ||
             p.is_a<liba> () ||
             p.is_a<libs> () ||
             p.is_a<libux> ()))
        {
          if (a.operation () == update_id)
          {
            // Handle (phase two) imported libraries. We know that for such
            // libraries we don't need to do match() in order to get options
            // (if any, they would be set by search_library()). But we do need
            // to match it if we may need its modules or importable headers
            // (see search_modules(), make_header_sidebuild() for details).
            //
            if (p.proj ())
            {
              pt = search_library (a,
                                   sys_lib_dirs,
                                   usr_lib_dirs,
                                   p.prerequisite);

              if (pt != nullptr && !modules)
                continue;
            }

            if (pt == nullptr)
              pt = &p.search (t);

            if (const libx* l = pt->is_a<libx> ())
              pt = link_member (*l, a, li);
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
        else if (pi == include_type::normal &&
                 (p.is_a<bmi> ()  || p.is_a (tts.bmi) ||
                  p.is_a<hbmi> () || p.is_a (tts.hbmi)))
        {
          continue;
        }
        else
        {
          pt = &p.search (t);

          if (a.operation () == clean_id && !pt->dir.sub (rs.out_path ()))
            continue;
        }

        target_state ts (
          match_async (a, *pt, ctx.count_busy (), t[a].task_count));

        if (p == md.src)
        {
          src_i = pts.size ();
          src_ts1 = ts;
        }

        pts.push_back (prerequisite_target (pt, pi));
      }

      size_t src_tc1 (t[a].task_count.load (memory_order_consume));

      wg.wait ();

      size_t src_tc2 (t[a].task_count.load (memory_order_consume));

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
        // an obj?{} target directly.
        //
        pair<bool, target_state> mr (
          build2::match (
            a,
            *pt,
            pt->is_a<liba> () || pt->is_a<libs> () || pt->is_a<libux> ()
            ? unmatch::safe
            : unmatch::none));

        if (mr.first)
          pt = nullptr; // Ignore in execute.
        else if (i == src_i)
          src_ts2 = mr.second;
      }

      // Inject additional prerequisites. We only do it when performing update
      // since chances are we will have to update some of our prerequisites in
      // the process (auto-generated source code, header units).
      //
      if (a == perform_update_id)
      {
        const file& src (pts[src_i]->as<file> ());

        // Figure out if __symexport is used. While normally it is specified
        // on the project root (which we cached), it can be overridden with
        // a target-specific value for installed modules (which we sidebuild
        // as part of our project).
        //
        // @@ MODHDR MSVC: are we going to do the same for header units? I
        //    guess we will figure it out when MSVC supports header units.
        //    Also see hashing below.
        //
        if (ut == unit_type::module_intf) // Note: still unrefined.
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
          fsdir_rule::perform_update_direct (a, t);
        }

        // Note: the leading '@' is reserved for the module map prefix (see
        // extract_modules()) and no other line must start with it.
        //
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

        // Then the compiler environment checksum.
        //
        if (dd.expect (env_checksum) != nullptr)
          l4 ([&]{trace << "environment mismatch forcing update of " << t;});

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

          if (ut == unit_type::module_intf) // Note: still unrefined.
            cs.append (&md.symexport, sizeof (md.symexport));

          // If we track translate_include then we should probably also track
          // the cc.importable flag for each header we include, which would be
          // quite heavy-handed indeed. Or maybe we shouldn't bother with this
          // at all: after all include translation is an optimization so why
          // rebuild an otherwise up-to-date target?
          //
#if 0
          if (modules)
          {
            // While there is also the companion importable_headers map, it's
            // unlikely to change in a way that affects us without changes to
            // other things that we track (e.g., compiler version, etc).
            //
            if (const auto* v = cast_null<translatable_headers> (
                  t[x_translate_include]))
            {
              for (const auto& p: *v)
              {
                cs.append (p.first);
                cs.append (!p.second || *p.second);
              }
            }
          }
#endif
          if (md.pp != preprocessed::all)
          {
            append_options (cs, t, x_poptions);
            append_options (cs, t, c_poptions);

            // Hash *.export.poptions from prerequisite libraries.
            //
            append_library_options (cs, bs, a, t, li);
          }

          append_options (cs, t, c_coptions);
          append_options (cs, t, x_coptions);

          if (ot == otype::s)
          {
            // On Darwin, Win32 -fPIC is the default.
            //
            if (tclass == "linux" || tclass == "bsd")
              cs.append ("-fPIC");
          }

          append_options (cs, cmode);

          if (md.pp != preprocessed::all)
            append_sys_hdr_options (cs); // Extra system header dirs (last).

          if (dd.expect (cs.string ()) != nullptr)
            l4 ([&]{trace << "options mismatch forcing update of " << t;});
        }

        // Finally the source file.
        //
        {
          const path& p (src.path ());

          // @@ TMP: we seem to have a race condition here but can't quite put
          // our finger on it.
          //
          // NOTE: remember to get rid of src_ts*, etc., once done.
          //
#if 0
          assert (!p.empty ()); // Sanity check.
#else
          if (p.empty ())
          {
            target_state src_ts3 (src.matched_state (a, false));

            info << "unassigned path for target " << src <<
              info << "is empty_path: " << (&p == &empty_path) <<
              info << "target state 1: " << src_ts1 <<
              info << "target state 2: " << src_ts2 <<
              info << "target state 3: " << src_ts3 <<
              info << "target count 1: " << src_tc1 <<
              info << "target count 2: " << src_tc2 <<
              info << "please report at "
                 << "https://github.com/build2/build2/issues/89";

            assert (!p.empty ());
          }
#endif
          if (dd.expect (p) != nullptr)
            l4 ([&]{trace << "source file mismatch forcing update of " << t;});
        }

        // If any of the above checks resulted in a mismatch (different
        // compiler, options, or source file) or if the depdb is newer than
        // the target (interrupted update), then do unconditional update.
        //
        // Note that load_mtime() can only be used in the execute phase so we
        // have to check for a cached value manually.
        //
        bool u;
        timestamp mt;

        if (dd.writing ())
          u = true;
        else
        {
          if ((mt = t.mtime ()) == timestamp_unknown)
            t.mtime (mt = mtime (tp)); // Cache.

          u = dd.mtime > mt;
        }

        // If updating for any of the above reasons, treat it as if doesn't
        // exist.
        //
        if (u)
          mt = timestamp_nonexistent;

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

          u = update (trace, a, *pt, u ? timestamp_unknown : mt) || u;
        }

        // Check if the source is already preprocessed to a certain degree.
        // This determines which of the following steps we perform and on
        // what source (original or preprocessed).
        //
        // Note: must be set on the src target.
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

        unit tu;

        // If we have no #include directives (or header unit imports), then
        // skip header dependency extraction.
        //
        pair<file_cache::entry, bool> psrc (file_cache::entry (), false);
        if (md.pp < preprocessed::includes)
        {
          // Note: trace is used in a test.
          //
          l5 ([&]{trace << "extracting headers from " << src;});
          auto& is (tu.module_info.imports);
          psrc = extract_headers (a, bs, t, li, src, md, dd, u, mt, is);
          is.clear (); // No longer needed.
        }

        // Next we "obtain" the translation unit information. What exactly
        // "obtain" entails is tricky: If things changed, then we re-parse the
        // translation unit. Otherwise, we re-create this information from
        // depdb. We, however, have to do it here and now in case the database
        // is invalid and we still have to fallback to re-parse.
        //
        // Store the translation unit's checksum to detect ignorable changes
        // (whitespaces, comments, etc).
        //
        // Note that we skip all of this if we have deferred a failure from
        // the header extraction phase (none of the module information should
        // be relevant).
        //
        if (!md.deferred_failure)
        {
          optional<string> cs;
          if (string* l = dd.read ())
            cs = move (*l);
          else
            u = true; // Database is invalid, force re-parse.

          for (bool first (true);; first = false)
          {
            if (u)
            {
              // Flush depdb since it can be used (as a module map) by
              // parse_unit().
              //
              if (dd.writing ())
                dd.flush ();

              string ncs (
                parse_unit (a, t, li, src, psrc.first, md, dd.path, tu));

              if (!cs || *cs != ncs)
              {
                assert (first); // Unchanged TU has a different checksum?
                dd.write (ncs);
              }
              //
              // Don't clear the update flag if it was forced or the checksum
              // should not be relied upon.
              //
              else if (first && !ncs.empty ())
              {
                // Clear the update flag and set the touch flag. Unless there
                // is no (usable) object file, of course. See also the md.mt
                // logic below.
                //
                if (mt != timestamp_nonexistent)
                {
                  u = false;
                  md.touch = true;
                }
              }
            }

            if (modules)
            {
              if (u || !first)
              {
                string s (to_string (tu.type, tu.module_info));

                if (first)
                  dd.expect (s);
                else
                  dd.write (s);
              }
              else
              {
                if (string* l = dd.read ())
                {
                  tu.type = to_module_info (*l, tu.module_info);
                }
                else
                {
                  u = true; // Database is invalid, force re-parse.
                  continue;
                }
              }
            }

            break;
          }

          // Make sure the translation unit type matches the resulting target
          // type. Note that tu here is the unrefined type.
          //
          switch (tu.type)
          {
          case unit_type::non_modular:
          case unit_type::module_impl:
            {
              if (ut != unit_type::non_modular)
                fail << "translation unit " << src << " is not a module "
                     << "interface or partition" <<
                  info << "consider using " << x_src.name << "{} instead";
              break;
            }
          case unit_type::module_intf:
          case unit_type::module_intf_part:
          case unit_type::module_impl_part:
            {
              if (ut != unit_type::module_intf)
                fail << "translation unit " << src << " is a module "
                     << "interface or partition" <<
                  info << "consider using " << x_mod->name << "{} instead";
              break;
            }
          case unit_type::module_header:
            {
              assert (ut == unit_type::module_header);
              break;
            }
          }

          // Refine the non-modular/module-impl decision from match().
          //
          ut = md.type = tu.type;

          // Note: trace is used in a test.
          //
          l5 ([&]{trace << "extracting modules from " << src;});

          // Extract the module dependency information in addition to header
          // dependencies.
          //
          // NOTE: assumes that no further targets will be added into
          //       t.prerequisite_targets!
          //
          if (modules)
          {
            extract_modules (a, bs, t, li,
                             tts, src,
                             md, move (tu.module_info), dd, u);

            // Currently in VC module interface units must be compiled from
            // the original source (something to do with having to detect and
            // store header boundaries in the .ifc files).
            //
            // @@ MODHDR MSVC: should we do the same for header units? I guess
            //    we will figure it out when MSVC supports header units.
            //
            // @@ TMP: probably outdated. Probably the same for partitions.
            //
            // @@ See also similar check in extract_headers(), existing entry
            //    case.
            //
            if (ctype == compiler_type::msvc)
            {
              if (ut == unit_type::module_intf)
                psrc.second = false;
            }
          }
        }

        // If anything got updated, then we didn't rely on the cache. However,
        // the cached data could actually have been valid and the compiler run
        // in extract_headers() as well as the code above merely validated it.
        //
        // We do need to update the database timestamp, however. Failed that,
        // we will keep re-validating the cached data over and over again.
        //
        // @@ DRYRUN: note that for dry-run we would keep re-touching the
        // database on every run (because u is true). So for now we suppress
        // it (the file will be re-validated on the real run anyway). It feels
        // like support for reusing the (partially) preprocessed output (see
        // note below) should help solve this properly (i.e., we don't want
        // to keep re-validating the file on every subsequent dry-run as well
        // on the real run).
        //
        if (u && dd.reading () && !ctx.dry_run)
          dd.touch = true;

        dd.close ();
        md.dd = move (dd.path);

        // If the preprocessed output is suitable for compilation, then pass
        // it along.
        //
        if (psrc.second)
        {
          md.psrc = move (psrc.first);

          // Now is also the right time to unpin the cache entry (we don't do
          // it earlier because parse_unit() may need to read it).
          //
          md.psrc.unpin ();

          // Without modules keeping the (partially) preprocessed output
          // around doesn't buy us much: if the source/headers haven't changed
          // then neither will the object file. Modules make things more
          // interesting: now we may have to recompile an otherwise unchanged
          // translation unit because a named module BMI it depends on has
          // changed. In this case re-processing the translation unit would be
          // a waste and compiling the original source would break distributed
          // compilation.
          //
          // Note also that the long term trend will (hopefully) be for
          // modularized projects to get rid of #include's which means the
          // need for producing this partially preprocessed output will
          // (hopefully) gradually disappear. Or not, most C headers will stay
          // headers, and probably not importable.
          //
          // @@ TODO: no use keeping it if there are no named module imports
          //          (but see also file_cache::create() hint, and
          //          extract_headers() the cache case: there we just assume
          //          it exists if modules is true).
          //
          if (modules)
            md.psrc.temporary = false; // Keep.
        }

        // Above we may have ignored changes to the translation unit. The
        // problem is, unless we also update the target's timestamp, we will
        // keep re-checking this on subsequent runs and it is not cheap.
        // Updating the target's timestamp is not without problems either: it
        // will cause a re-link on a subsequent run. So, essentially, we
        // somehow need to remember two timestamps: one for checking
        // "preprocessor prerequisites" above and one for checking other
        // prerequisites (like modules) below. So what we are going to do is
        // "store" the first in the target file (so we do touch it) and the
        // second in depdb (which is never newer that the target).
        //
        // Perhaps when we start keeping the partially preprocessed output
        // this will fall away? Yes, please.
        //
        md.mt = u ? timestamp_nonexistent : dd.mtime;
      }

      switch (a)
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

    // Reverse-lookup target type(s) from extension.
    //
    small_vector<const target_type*, 2> compile_rule::
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
        optional<string> de (tt.default_extension (tk, s, nullptr, true));

        return de && *de == e;
      };

      small_vector<const target_type*, 2> r;

      for (const target_type* const* p (x_inc); *p != nullptr; ++p)
        if (test (**p))
          r.push_back (*p);

      return r;
    }

    void compile_rule::
    append_prefixes (prefix_map& m, const target& t, const variable& var) const
    {
      tracer trace (x, "compile_rule::append_prefixes");

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

          try
          {
            if (o.size () == 2)
            {
              if (++i == e)
                break; // Let the compiler complain.

              d = dir_path (*i);
            }
            else
              d = dir_path (*i, 2, string::npos);
          }
          catch (const invalid_path& e)
          {
            fail << "invalid directory '" << e.path << "'"
                 << " in option '" << o << "'"
                 << " in variable " << var
                 << " for target " << t;
          }

          l6 ([&]{trace << "-I " << d;});

          if (d.relative ())
            fail << "relative directory " << d
                 << " in option '" << o << "'"
                 << " in variable " << var
                 << " for target " << t;

          // If the directory is not normalized, we can complain or normalize
          // it. Let's go with normalizing to minimize questions/complaints.
          //
          if (!d.normalized (false)) // Allow non-canonical dir separators.
            d.normalize ();

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
                  trace << "ignoring mapping for prefix '" << p << "'\n"
                        << "  existing mapping to " << v.directory
                        << " priority " << v.priority << '\n'
                        << "  another mapping to  " << d
                        << " priority " << prio;
              }
              else
              {
                if (verb >= 4)
                  trace << "overriding mapping for prefix '" << p << "'\n"
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
              l6 ([&]{trace << "'" << p << "' -> " << d << " priority "
                            << prio;});
              m.emplace (move (p), prefix_value {move (d), prio});
            }
          };

#if 1
          // Enter all outer prefixes, including prefixless.
          //
          // The prefixless part is fuzzy but seems to be doing the right
          // thing ignoring/overriding-wise, at least in cases where one of
          // the competing -I paths is a subdirectory of another. But the
          // proper solution will be to keep all the prefixless entries (by
          // changing prefix_map to a multimap) since for them we have an
          // extra check (target must be explicitly spelled out in a
          // buildfile).
          //
          for (size_t prio (0);; ++prio)
          {
            bool e (p.empty ());
            enter ((e ? move (p) : p), (e ? move (d) : d), prio);
            if (e)
              break;
            p = p.directory ();
          }
#else
          size_t prio (0);
          for (bool e (false); !e; ++prio)
          {
            dir_path n (p.directory ());
            e = n.empty ();
            enter ((e ? move (p) : p), (e ? move (d) : d), prio);
            p = move (n);
          }
#endif
        }
      }
    }

    auto compile_rule::
    build_prefix_map (const scope& bs,
                      action a,
                      target& t,
                      linfo li) const -> prefix_map
    {
      prefix_map m;

      // First process our own.
      //
      append_prefixes (m, t, x_poptions);
      append_prefixes (m, t, c_poptions);

      // Then process the include directories from prerequisite libraries.
      //
      append_library_prefixes (m, bs, a, t, li);

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
    // (unless clang-cl; handled by our caller). Then we have the list of
    // headers, one per line, in this form (text can presumably be
    // translated):
    //
    // Note: including file: C:\Program Files (x86)\[...]\iostream
    //
    // Finally, if we hit a non-existent header, then we end with an error
    // line in this form:
    //
    // x.cpp(3): fatal error C1083: Cannot open include file: 'd/h.hpp':
    // No such file or directory
    //
    // @@ TODO: this is not the case for clang-cl: it issues completely
    //          different diagnostics and before any /showIncludes lines.
    //
    // Distinguishing between the include note and the include error is
    // easy: we can just check for C1083. Distinguising between the note and
    // other errors/warnings is harder: an error could very well end with
    // what looks like a path so we cannot look for the note but rather have
    // to look for an error. Here we assume that a line containing ' CNNNN:'
    // is an error. Should be robust enough in the face of language
    // translation, etc.
    //
    // It turns out C1083 is also used when we are unable to open the main
    // source file and the error line (which is printed after the first line
    // containing the file name) looks like this:
    //
    // c1xx: fatal error C1083: Cannot open source file: 's.cpp': No such
    // file or directory
    //
    // And it turns out C1083 is also used when we are unable to open a type
    // library specified with #import. In this case the error looks like this
    // (at least in VC 14, 15, and 16):
    //
    // ...\comdef.h: fatal error C1083: Cannot open type library file:
    // 'l.tlb': Error loading type library/DLL.
    //

    pair<size_t, size_t>
    msvc_sense_diag (const string&, char); // msvc.cxx

    static inline bool
    msvc_header_c1083 (const string& l, const pair<size_t, size_t>& pr)
    {
      return
        l.compare (pr.second, 5, "c1xx:")     != 0 && /* Not source file. */
        l.compare (pr.second, 9, "comdef.h:") != 0;   /* Not type library. */
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

      pair<size_t, size_t> pr (msvc_sense_diag (l, 'C'));
      size_t p (pr.first);

      if (p == string::npos)
      {
        // Include note.
        //
        // We assume the path is always at the end but need to handle both
        // absolute Windows and POSIX ones.
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
              path::traits_type::is_separator (l[p + 1]))
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
          fail << "unable to parse /showIncludes include note line \""
               << l << '"';

        return string (l, p);
      }
      else if (l.compare (p, 4, "1083") == 0 && msvc_header_c1083 (l, pr))
      {
        // Include error.
        //
        // The path is conveniently quoted with ''. Or so we thought: turns
        // out different translations (e.g., Chinese) can use different quote
        // characters and some translations (e.g., Russian) don't use quotes
        // at all. But the overall structure seems to be stable:
        //
        // ...C1083: <translated>: [']d/h.hpp[']: <translated>
        //
        // Where `'` is some sort of a quote character which could to be
        // multi-byte (e.g., in Chinese).
        //
        // Plus, in some internal (debug?) builds the second <translated> part
        // may have the "No such file or directory (c:\...\p0prepro.c:1722)"
        // form (so it may contain `:`).
#if 0
        string l;
        //l = "...: fatal error C1083: ...: 'libhello/version.hxx': ..."; //en
        //l = "...: fatal error C1083: ...: libhello/version.hxx: ...";   //ru
        //l = "...: fatal error C1083: ...: '\xb0libhello/version.hxx\xa1': ..."; //zh
        //l = "...: fatal error C1083: ...: 'libhello/version.hxx': No such file or directory (c:\\...\\p0prepro.c:1722)";
        p = l.find ("1083") + 1;
        text << l;
#endif

        // Find first leading ':' that's followed by a space (after "C1083:").
        //
        size_t p1 (p + 4); // 1083
        while ((p1 = l.find (':', p1 + 1)) != string::npos && l[p1 + 1] != ' ')
          ;

        // Find first trailing ':' that's followed by a space.
        //
        size_t p2 (l.size ());
        while ((p2 = l.rfind (':', p2 - 1)) != string::npos && l[p2 + 1] != ' ')
          ;

        if (p1 != string::npos &&
            p2 != string::npos &&
            (p2 - p1) > 3 )        // At least ": x:".

        {
          p1 += 2; // Skip leading ": ".

          // Now p1 is the first character of the potentially quoted path
          // while p2 -- one past the last character.
          //
          // We now skip any characters at the beginning and at the end that
          // could be quotes: single/double quotes plus, to handle the mutli-
          // byte case, non-printable ASCII characters (the latter is a bit
          // iffy: a multi-byte sequence could have one of the bytes
          // printable; in Chinese the sequences are \x27\xb0 and \xa1\x27
          // where \x27 is `'`).
          //
          auto quote = [] (char c)
          {
            return c == '\'' || c == '"' || c < 0x20 || c > 0x7e;
          };

          for (; p1 != p2 && quote (l[p1]);     ++p1) ;
          for (; p2 != p1 && quote (l[p2 - 1]); --p2) ;

          if (p1 != p2)
          {
            good_error = true;
            return string (l, p1 , p2 - p1);
          }
        }

        fail << "unable to parse /showIncludes include error line \""
             << l << '"' << endf;
      }
      else
      {
        // Some other error.
        //
        return string ();
      }
    }

    void
    msvc_sanitize_cl (cstrings&); // msvc.cxx

    // GCC module mapper handler.
    //
    // Note that the input stream is non-blocking while output is blocking
    // and this function should be prepared to handle closed input stream.
    // Any unhandled io_error is handled by the caller as a generic module
    // mapper io error. Returning false terminates the communication.
    //
    struct compile_rule::module_mapper_state //@@ gcc_module_mapper_state
    {
      size_t skip;              // Number of depdb entries to skip.
      size_t header_units = 0;  // Number of header units imported.
      module_imports& imports;  // Unused (potentially duplicate suppression).

      // Include translation (looked up lazily).
      //
      optional<const build2::cc::translatable_headers*> translatable_headers;

      small_vector<string, 2> batch; // Reuse buffers.

      module_mapper_state (size_t s, module_imports& i)
          : skip (s), imports (i) {}
    };

    bool compile_rule::
    gcc_module_mapper (module_mapper_state& st,
                       action a, const scope& bs, file& t, linfo li,
                       ifdstream& is,
                       ofdstream& os,
                       depdb& dd, bool& update, bool& bad_error,
                       optional<prefix_map>& pfx_map, srcout_map& so_map) const
    {
      tracer trace (x, "compile_rule::gcc_module_mapper");

      // While the dynamic mapper is only used during preprocessing, we still
      // need to handle batching (GCC sends batches of imports even in this
      // mode; actually, not for header unit imports and module imports we
      // don't see here). Note that we cannot sidestep batching by handing one
      // request at a time; while this might work to a certain extent (due to
      // pipe buffering), there is no guarantee (see libcody issue #20).

      // Read in the entire batch trying hard to reuse the buffers.
      //
      auto& batch (st.batch);
      size_t batch_n (0);

      for (;;)
      {
        if (batch.size () == batch_n)
          batch.push_back (string ());

        string& r (batch[batch_n]);

        if (eof (getline (is, r)))
          break;

        batch_n++;

        if (r.back () != ';')
          break;

        // Strip the trailing `;` word.
        //
        r.pop_back ();
        r.pop_back ();
      }

      if (batch_n == 0) // EOF
        return false;

      if (verb >= 3)
      {
        // Note that we show `;` in requests/responses so that the result
        // could be replayed.
        //
        // @@ Should we print the pid we are talking to? It gets hard to
        //    follow once things get nested. But if all our diag will include
        //    some kind of id (chain, thread?), then this will not be strictly
        //    necessary.
        //
        diag_record dr (text);
        for (size_t i (0); i != batch_n; ++i)
          dr << (i == 0 ? "  > " : " ;\n    ") << batch[i];
      }

      // Handle each request converting it into a response.
      //
      bool term (false);

      string tmp; // Reuse the buffer.
      for (size_t i (0); i != batch_n; ++i)
      {
        string& r (batch[i]);

        // @@ TODO: quoting and escaping.
        //
        size_t b (0), e (0), n; // Next word.

        auto next = [&r, &b, &e, &n] () -> size_t
        {
          return (n = next_word (r, b, e, ' ', '\t'));
        };

        next (); // Request name.

        auto name = [&r, b, n] (const char* c) -> bool
        {
          // We can reasonably assume a command will never be quoted.
          //
          return (r.compare (b, n, c) == 0 &&
                  (r[n] == ' '  || r[n] == '\t' || r[n] == '\0'));
        };

        // Handle the request by explicitly continuing to the next iteration
        // on success and optionally setting the reason on failure.
        //
        const char* w ("malformed request");

        if (name ("HELLO"))
        {
          // > HELLO <version> <compiler> <ident>
          // < HELLO <version> <mapper> [<flags>]
          //
          //@@ TODO: check protocol version.

          r = "HELLO 1 build2";
          continue;
        }
        else if (name ("MODULE-REPO"))
        {
          // > MODULE-REPO
          // < PATHNAME <repo-dir>

          // Return the current working directory to essentially disable this
          // functionality.
          //
          r = "PATHNAME .";
          continue;
        }
        // Turns out it's easiest to handle header IMPORT together with
        // INCLUDE since it can also trigger a re-search, etc. In a sense,
        // IMPORT is all of the INCLUDE logic (but skipping translation) plus
        // the BMI dependency synthesis. Note that we don't get named module
        // imports here.
        //
        else if (name ("MODULE-IMPORT") || name ("INCLUDE-TRANSLATE"))
        {
          // > MODULE-IMPORT <path> [<flags>]
          // < PATHNAME <bmi-path>
          //
          // > INCLUDE-TRANSLATE <path> [<flags>]
          // < BOOL [TRUE|FALSE]
          // < PATHNAME <bmi-path>

          bool imp (r[b] == 'M'); // import

          if (next ())
          {
            path f (r, b, n);
            bool exists (true);

            // The TU path we pass to the compiler is always absolute so any
            // ""-includes will also be absolute. As a result, the only way to
            // end up with a relative path is by using relative -I which
            // doesn't make much sense in our world (it will be relative to
            // CWD).
            //
            if (exists && f.relative ())
            {
              tmp.assign (r, b, n);
              r = "ERROR relative header path '"; r += tmp; r += '\'';
              continue;
            }

            // Note also that we may see multiple imports of the same header
            // if it's imported via multiple paths. There is nothing really we
            // can do about it since we have to have each path in the file
            // mapper (see below for details).
            //
            // At least in case of GCC, we don't see multiple imports for the
            // same path nor multiple inclusions, regardless of whether the
            // header uses #pragma once or include guards.

            // The skip_count logic: in a nutshell (and similar to the non-
            // mapper case), we may have "processed" some portion of the
            // headers based on the depdb cache and we need to avoid
            // re-processing them here. See the skip_count discussion for
            // details.
            //
            // Note also that we need to be careful not to decrementing the
            // count for re-searches and include translation.
            //
            bool skip (st.skip != 0);

            // The first part is the same for both include and import: resolve
            // the header path to target, update it, and trigger re-search if
            // necessary.
            //
            const file* ht (nullptr);
            auto& pts (t.prerequisite_targets[a]);

            // Enter, update, and see if we need to re-search this header.
            //
            bool updated (false), remapped;
            try
            {
              pair<const file*, bool> er (
                enter_header (a, bs, t, li,
                              move (f), false /* cache */, false /* norm */,
                              pfx_map, so_map));

              ht = er.first;
              remapped = er.second;

              if (remapped)
              {
                r = "ERROR remapping of headers not supported";
                continue;
              }

              // If we couldn't enter this header as a target or find a rule
              // to update it, then it most likely means a misspelled header
              // (rather than a broken generated header setup) and our
              // diagnostics won't really add anything to the compiler's. So
              // let's only print it at -V or higher.
              //
              if (ht == nullptr)
              {
                assert (!exists); // Sanity check.

                if (verb > 2)
                {
                  diag_record dr;
                  dr << error << "header '" << f << "' not found and no "
                     << "rule to generate it";

                  if (verb < 4)
                    dr << info << "re-run with --verbose=4 for more information";
                }

                throw failed ();
              }

              // Note that we explicitly update even for import (instead of,
              // say, letting the BMI rule do it implicitly) since we may need
              // to cause a re-search (see below).
              //
              if (!skip)
              {
                if (pts.empty () || pts.back () != ht)
                {
                  optional<bool> ir (inject_header (a, t,
                                                    *ht, timestamp_unknown,
                                                    verb > 2 /* fail */));
                  if (!ir)
                    throw failed ();

                  updated = *ir;
                }
                else
                  assert (exists);
              }
              else
                assert (exists && !remapped); // Maybe this should be an error.
            }
            catch (const failed&)
            {
              // If the header does not exist or could not be updated, do we
              // want our diagnostics, the compiler's, or both? We definitely
              // want the compiler's since it points to the exact location.
              // Ours could also be helpful. So while it will look a bit
              // messy, let's keep both (it would have been nicer to print
              // ours after the compiler's but that isn't easy).
              //
              r = "ERROR unable to update header '";
              r += (ht != nullptr ? ht->path () : f).string ();
              r += '\'';
              continue;
            }

            if (!imp) // Indirect prerequisite (see above).
              update = updated || update;

            // A mere update is not enough to cause a re-search. It either had
            // to also not exist or be remapped.
            //
            // @@ Currently impossible.
            //
            /*
            if ((updated && !exists) || remapped)
            {
              rs = "SEARCH";
              st.data = move (n); // Followup correlation.
              break;
            }
            */

            // Now handle INCLUDE and IMPORT differences.
            //
            const path& hp (ht->path ());
            const string& hs (hp.string ());

            // Reduce include translation to the import case.
            //
            if (!imp)
            {
              if (!st.translatable_headers)
                st.translatable_headers =
                  cast_null<translatable_headers> (t[x_translate_include]);

              if (*st.translatable_headers != nullptr)
              {
                auto& ths (**st.translatable_headers);

                // First look for the header path in the translatable headers
                // itself.
                //
                auto i (ths.find (hs)), ie (ths.end ());

                // Next look it up in the importable headers and then look up
                // the associated groups in the translatable headers.
                //
                if (i == ie)
                {
                  slock l (importable_headers->mutex);
                  auto& ihs (importable_headers->header_map);

                  auto j (ihs.find (hp)), je (ihs.end ());

                  if (j != je)
                  {
                    // The groups are ordered from the most to least specific.
                    //
                    for (const string& g: j->second)
                      if ((i = ths.find (g)) != ie)
                        break;
                  }

                  // Finally look for the `all` groups.
                  //
                  if (i == ie)
                  {
                    i = ths.find (header_group_all_importable);

                    if (i != ie)
                    {
                      // See if this header is marked as importable.
                      //
                      if (lookup l = (*ht)[c_importable])
                      {
                        if (!cast<bool> (l))
                          i = ie;
                      }
                      else if (j != je)
                      {
                        // See if this is one of ad hoc *-importable groups
                        // (currently only std-importable).
                        //
                        const auto& gs (j->second);
                        if (find (gs.begin (),
                                  gs.end (),
                                  header_group_std_importable) == gs.end ())
                          i = ie;
                      }
                      else
                        i = ie;
                    }

                    if (i == ie)
                      i = ths.find (header_group_all);
                  }
                }

                // Translate if we found an entry and it's not false.
                //
                imp = (i != ie && (!i->second || *i->second));
              }
            }

            if (imp)
            {
              try
              {
                // Synthesize the BMI dependency then update and add the BMI
                // target as a prerequisite.
                //
                const file& bt (make_header_sidebuild (a, bs, t, li, *ht));

                if (!skip)
                {
                  optional<bool> ir (inject_header (a, t,
                                                    bt, timestamp_unknown,
                                                    true /* fail */));
                  assert (ir); // Not from cache.
                  update = *ir || update;
                }

                const string& bp (bt.path ().string ());

                if (skip)
                  st.skip--;
                else
                {
                  // While the header path passed by the compiler is absolute
                  // (see the reason/check above), it is not necessarily
                  // normalized. And that's the path that the compiler will
                  // look for in the static mapping. So we have to write the
                  // original (which we may need to normalize when we read
                  // this mapping in extract_headers()).
                  //
                  tmp = "@ "; tmp.append (r, b, n); tmp += ' '; tmp += bp;
                  dd.expect (tmp);
                  st.header_units++;
                }

                r = "PATHNAME "; r += bp;
              }
              catch (const failed&)
              {
                r = "ERROR 'unable to update header unit for ";
                r += hs; r += '\'';
                continue;
              }
            }
            else
            {
              if (skip)
                st.skip--;
              else
                dd.expect (hs);

              // Confusingly, TRUE means include textually and FALSE means we
              // don't know.
              //
              r = "BOOL TRUE";
            }

            continue;
          }
        }
        else
          w = "unexpected request";

        // Truncate the response batch and terminate the communication (see
        // also libcody issue #22).
        //
        tmp.assign (r, b, n);
        r = "ERROR '"; r += w; r += ' '; r += tmp; r += '\'';
        batch_n = i + 1;
        term = true;
        break;
      }

      if (verb >= 3)
      {
        diag_record dr (text);
        for (size_t i (0); i != batch_n; ++i)
          dr << (i == 0 ? "  < " : " ;\n    ") << batch[i];
      }

      // Write the response batch.
      //
      for (size_t i (0);; )
      {
        string& r (batch[i]);

        if (r.compare (0, 6, "ERROR ") == 0)
          bad_error = true;

        os.write (r.c_str (), static_cast<streamsize> (r.size ()));

        if (++i == batch_n)
        {
          os.put ('\n');
          break;
        }
        else
          os.write (" ;\n", 3);
      }

      os.flush ();

      return !term;
    }

    // The original module mapper implementation (c++-modules-ex GCC branch)
    //
    // @@ TMP remove at some point
#if 0
    void compile_rule::
    gcc_module_mapper (module_mapper_state& st,
                       action a, const scope& bs, file& t, linfo li,
                       ifdstream& is,
                       ofdstream& os,
                       depdb& dd, bool& update, bool& bad_error,
                       optional<prefix_map>& pfx_map, srcout_map& so_map) const
    {
      tracer trace (x, "compile_rule::gcc_module_mapper");

      // Read in the request line.
      //
      // Because the dynamic mapper is only used during preprocessing, we
      // can assume there is no batching and expect to see one line at a
      // time.
      //
      string rq;
#if 1
      if (!eof (getline (is, rq)))
      {
        if (rq.empty ())
          rq = "<empty>"; // Not to confuse with EOF.
      }
#else
      for (char buf[4096]; !is.eof (); )
      {
        streamsize n (is.readsome (buf, sizeof (buf) - 1));
        buf[n] = '\0';

        if (char* p = strchr (buf, '\n'))
        {
          *p = '\0';

          if (++p != buf + n)
            fail << "batched module mapper request: '" << p << "'";

          rq += buf;
          break;
        }
        else
          rq += buf;
      }
#endif

      if (rq.empty ()) // EOF
        return;

      // @@ MODHDR: Should we print the pid we are talking to? It gets hard to
      //            follow once things get nested. But if all our diag will
      //            include some kind of id (chain, thread?), then this will
      //            not be strictly necessary.
      //
      if (verb >= 3)
        text << "  > " << rq;

      // Check for a command. If match, remove it and the following space from
      // the request string saving it in cmd (for diagnostics) unless the
      // second argument is false, and return true.
      //
      const char* cmd (nullptr);
      auto command = [&rq, &cmd] (const char* c, bool r = true)
      {
        size_t n (strlen (c));
        bool m (rq.compare (0, n, c) == 0 && rq[n] == ' ');

        if (m && r)
        {
          cmd = c;
          rq.erase (0, n + 1);
        }

        return m;
      };

      string rs;
      for (;;) // Breakout loop.
      {
        // Each command is reponsible for handling its auxiliary data while we
        // just clear it.
        //
        string data (move (st.data));

        if (command ("HELLO"))
        {
          // HELLO <ver> <kind> <ident>
          //
          //@@ MODHDR TODO: check protocol version.

          // We don't use "repository path" (whatever it is) so we pass '.'.
          //
          rs = "HELLO 0 build2 .";
        }
        //
        // Turns out it's easiest to handle IMPORT together with INCLUDE since
        // it can also trigger a re-search, etc. In a sense, IMPORT is all of
        // the INCLUDE logic (skipping translation) plus the BMI dependency
        // synthesis.
        //
        else if (command ("INCLUDE") || command ("IMPORT"))
        {
          // INCLUDE [<"']<name>[>"'] <path>
          // IMPORT [<"']<name>[>"'] <path>
          // IMPORT '<path>'
          //
          // <path> is the resolved path or empty if the header is not found.
          // It can be relative if it is derived from a relative path (either
          // via -I or includer). If <name> is single-quoted, then it cannot
          // be re-searched (e.g., implicitly included stdc-predef.h) and in
          // this case <path> is never empty.
          //
          // In case of re-search or include translation we may have to split
          // handling the same include or import across multiple commands.
          // Here are the scenarios in question:
          //
          // INCLUDE --> SEARCH -?-> INCLUDE
          // IMPORT  --> SEARCH -?-> IMPORT
          // INCLUDE --> IMPORT -?-> IMPORT
          //
          // The problem is we may not necessarily get the "followup" command
          // (the question marks above). We may not get the followup after
          // SEARCH because, for example, the newly found header has already
          // been included/imported using a different style/path. Similarly,
          // the IMPORT response may not be followed up with the IMPORT
          // command because this header has already been imported, for
          // example, using an import declaration. Throw into this #pragma
          // once, include guards, and how exactly the compiler deals with
          // them and things become truly unpredictable and hard to reason
          // about. As a result, for each command we have to keep the build
          // state consistent, specifically, without any "dangling" matched
          // targets (which would lead to skew dependency counts). Note: the
          // include translation is no longer a problem since we respond with
          // an immediate BMI.
          //
          // To keep things simple we are going to always add a target that we
          // matched to our prerequisite_targets. This includes the header
          // target when building the BMI: while not ideal, this should be
          // harmless provided we don't take its state/mtime into account.
          //
          // One thing we do want to handle specially is the "maybe-followup"
          // case discussed above. It is hard to distinguish from an unrelated
          // INCLUDE/IMPORT (we could have saved <name> and maybe correlated
          // based on that). But if we don't, then we will keep matching and
          // adding each target twice. What we can do, however, is check
          // whether this target is already in prerequisite_targets and skip
          // it if that's the case, which is a valid thing to do whether it is
          // a followup or an unrelated command. In fact, for a followup, we
          // only need to check the last element in prerequisite_targets.
          //
          // This approach strikes a reasonable balance between keeping things
          // simple and handling normal cases without too much overhead. Note
          // that we may still end up matching and adding the same targets
          // multiple times for pathological cases, like when the same header
          // is included using a different style/path, etc. We could, however,
          // take care of this by searching the entire prerequisite_targets,
          // which is always an option (and which would probably be required
          // if the compiler were to send the INCLUDE command before checking
          // for #pragma once or include guards, which GCC does not do).
          //
          // One thing that we cannot do without distinguishing followup and
          // unrelated commands is verify the remapped header found by the
          // compiler resolves to the expected target. So we will also do the
          // correlation via <name>.
          //
          bool imp (cmd[1] == 'M');

          path f;          // <path> or <name> if doesn't exist
          string n;        // [<"']<name>[>"']
          bool exists;     // <path> is not empty
          bool searchable; // <name> is not single-quoted
          {
            char q (rq[0]);                // Opening quote.
            q = (q ==  '<' ?  '>' :
                 q ==  '"' ?  '"' :
                 q == '\'' ? '\'' : '\0'); // Closing quote.

            size_t s (rq.size ()), qp; // Quote position.
            if (q == '\0' || (qp = rq.find (q, 1)) == string::npos)
              break; // Malformed command.

            n.assign (rq, 0, qp + 1);

            size_t p (qp + 1);
            if (imp && q == '\'' && p == s) // IMPORT '<path>'
            {
              exists = true;
              // Leave f empty and fall through.
            }
            else
            {
              if (p != s && rq[p++] != ' ') // Skip following space, if any.
                break;

              exists = (p != s);

              if (exists)
              {
                rq.erase (0, p);
                f = path (move (rq));
                assert (!f.empty ());
              }
              //else // Leave f empty and fall through.
            }

            if (f.empty ())
            {
              rq.erase (0, 1);   // Opening quote.
              rq.erase (qp - 1); // Closing quote and trailing space, if any.
              f = path (move (rq));
            }

            // Complete relative paths not to confuse with non-existent.
            //
            if (exists && !f.absolute ())
              f.complete ();

            searchable = (q != '\'');
          }

          // The skip_count logic: in a nutshell (and similar to the non-
          // mapper case), we may have "processed" some portion of the headers
          // based on the depdb cache and we need to avoid re-processing them
          // here. See the skip_count discussion for details.
          //
          // Note also that we need to be careful not to decrementing the
          // count for re-searches and include translation.
          //
          bool skip (st.skip != 0);

          // The first part is the same for both INCLUDE and IMPORT: resolve
          // the header path to target, update it, and trigger re-search if
          // necessary.
          //
          const file* ht (nullptr);
          auto& pts (t.prerequisite_targets[a]);

          // If this is a followup command (or indistinguishable from one),
          // then as a sanity check verify the header found by the compiler
          // resolves to the expected target.
          //
          if (data == n)
          {
            assert (!skip); // We shouldn't be re-searching while skipping.

            if (exists)
            {
              pair<const file*, bool> r (
                enter_header (a, bs, t, li,
                              move (f), false /* cache */, false /* norm */,
                              pfx_map, so_map));

              if (!r.second) // Shouldn't be remapped.
                ht = r.first;
            }

            if (ht != pts.back ())
            {
              ht = static_cast<const file*> (pts.back ().target);
              rs = "ERROR expected header '" + ht->path ().string () +
                "' to be found instead";
              bad_error = true; // We expect an error from the compiler.
              break;
            }

            // Fall through.
          }
          else
          {
            // Enter, update, and see if we need to re-search this header.
            //
            bool updated (false), remapped;
            try
            {
              pair<const file*, bool> er (
                enter_header (a, bs, t, li,
                              move (f), false /* cache */, false /* norm */,
                              pfx_map, so_map));

              ht = er.first;
              remapped = er.second;

              if (remapped && !searchable)
              {
                rs = "ERROR remapping non-re-searchable header " + n;
                bad_error = true;
                break;
              }

              // If we couldn't enter this header as a target or find a rule
              // to update it, then it most likely means a misspelled header
              // (rather than a broken generated header setup) and our
              // diagnostics won't really add anything to the compiler's. So
              // let's only print it at -V or higher.
              //
              if (ht == nullptr)
              {
                assert (!exists); // Sanity check.

                if (verb > 2)
                {
                  diag_record dr;
                  dr << error << "header '" << f << "' not found";

                  if (verb < 4)
                    dr << info << "re-run with --verbose=4 for more information";
                }

                throw failed ();
              }

              // Note that we explicitly update even for IMPORT (instead of,
              // say, letting the BMI rule do it implicitly) since we may need
              // to cause a re-search (see below).
              //
              if (!skip)
              {
                if (pts.empty () || pts.back () != ht)
                {
                  optional<bool> ir (inject_header (a, t,
                                                    *ht, timestamp_unknown,
                                                    verb > 2 /* fail */));
                  if (!ir)
                    throw failed ();

                  updated = *ir;
                }
                else
                  assert (exists);
              }
              else
                assert (exists && !remapped); // Maybe this should be an error.
            }
            catch (const failed&)
            {
              // If the header does not exist or could not be updated, do we
              // want our diagnostics, the compiler's, or both? We definitely
              // want the compiler's since it points to the exact location.
              // Ours could also be helpful. So while it will look a bit
              // messy, let's keep both (it would have been nicer to print
              // ours after the compiler's but that isn't easy).
              //
              rs = !exists
                ? string ("INCLUDE")
                : ("ERROR unable to update header '" +
                   (ht != nullptr ? ht->path () : f).string () + "'");

              bad_error = true;
              break;
            }

            if (!imp) // Indirect prerequisite (see above).
              update = updated || update;

            // A mere update is not enough to cause a re-search. It either had
            // to also not exist or be remapped.
            //
            if ((updated && !exists) || remapped)
            {
              rs = "SEARCH";
              st.data = move (n); // Followup correlation.
              break;
            }

            // Fall through.
          }

          // Now handle INCLUDE and IMPORT differences.
          //
          const string& hp (ht->path ().string ());

          // Reduce include translation to the import case.
          //
          if (!imp && xlate_hdr != nullptr)
          {
            auto i (lower_bound (
                      xlate_hdr->begin (), xlate_hdr->end (),
                      hp,
                      [] (const string& x, const string& y)
                      {
                        return path::traits_type::compare (x, y) < 0;
                      }));

            imp = (i != xlate_hdr->end () && *i == hp);
          }

          if (imp)
          {
            try
            {
              // Synthesize the BMI dependency then update and add the BMI
              // target as a prerequisite.
              //
              const file& bt (make_header_sidebuild (a, bs, t, li, *ht));

              if (!skip)
              {
                optional<bool> ir (inject_header (a, t,
                                                  bt, timestamp_unknown,
                                                  true /* fail */));
                assert (ir); // Not from cache.
                update = *ir || update;
              }

              const string& bp (bt.path ().string ());

              if (!skip)
              {
                // @@ MODHDR: we write normalized path while the compiler will
                //            look for the original. In particular, this means
                //            that paths with `..` won't work. Maybe write
                //            original for mapping and normalized for our use?
                //
                st.headers++;
                dd.expect ("@ '" + hp + "' " + bp);
              }
              else
                st.skip--;

              rs = "IMPORT " + bp;
            }
            catch (const failed&)
            {
              rs = "ERROR unable to update header unit '" + hp + "'";
              bad_error = true;
              break;
            }
          }
          else
          {
            if (!skip)
              dd.expect (hp);
            else
              st.skip--;

            rs = "INCLUDE";
          }
        }

        break;
      }

      if (rs.empty ())
      {
        rs = "ERROR unexpected command '";

        if (cmd != nullptr)
        {
          rs += cmd; // Add the command back.
          rs += ' ';
        }

        rs += rq;
        rs += "'";

        bad_error = true;
      }

      if (verb >= 3)
        text << "  < " << rs;

      os << rs << endl;
    }
#endif

    // Enter as a target a header file. Depending on the cache flag, the file
    // is assumed to either have come from the depdb cache or from the
    // compiler run.
    //
    // Return the header target and an indication of whether it was remapped
    // or NULL if the header does not exist and cannot be generated. In the
    // latter case the passed header path is guaranteed to be still valid but
    // might have been adjusted (e.g., normalized, etc).
    //
    // Note: this used to be a lambda inside extract_headers() so refer to the
    // body of that function for the overall picture.
    //
    pair<const file*, bool> compile_rule::
    enter_header (action a, const scope& bs, file& t, linfo li,
                  path&& f, bool cache, bool norm,
                  optional<prefix_map>& pfx_map, srcout_map& so_map) const
    {
      tracer trace (x, "compile_rule::enter_header");

      // Find or maybe insert the target. The directory is only moved from if
      // insert is true. Note that it must be normalized.
      //
      auto find = [&trace, &t, this] (dir_path&& d,
                                      path&& f,
                                      bool insert) -> const file*
      {
        // Split the file into its name part and extension. Here we can assume
        // the name part is a valid filesystem name.
        //
        // Note that if the file has no extension, we record an empty
        // extension rather than NULL (which would signify that the default
        // extension should be added).
        //
        string e (f.extension ());
        string n (move (f).string ());

        if (!e.empty ())
          n.resize (n.size () - e.size () - 1); // One for the dot.

        // See if this directory is part of any project and if so determine
        // the target type.
        //
        // While at it also determine if this target is from the src or out
        // tree of said project.
        //
        dir_path out;

        // It's possible the extension-to-target type mapping is ambiguous
        // (usually because both C and X-language headers use the same .h
        // extension). In this case we will first try to find one that matches
        // an explicit target (similar logic to when insert is false).
        //
        small_vector<const target_type*, 2> tts;

        // Note that the path can be in out or src directory and the latter
        // can be associated with multiple scopes. So strictly speaking we
        // need to pick one that is "associated" with us. But that is still a
        // TODO (see scope_map::find() for details) and so for now we just
        // pick the first one (it's highly unlikely the source file extension
        // mapping will differ based on the configuration).
        //
        {
          const scope& bs (**t.ctx.scopes.find (d).first);
          if (const scope* rs = bs.root_scope ())
          {
            tts = map_extension (bs, n, e);

            if (!bs.out_eq_src () && d.sub (bs.src_path ()))
              out = out_src (d, *rs);
          }
        }

        // If it is outside any project, or the project doesn't have such an
        // extension, assume it is a plain old C header.
        //
        if (tts.empty ())
        {
          // If the project doesn't "know" this extension then we can't
          // possibly find an explicit target of this type.
          //
          if (!insert)
            return nullptr;

          tts.push_back (&h::static_type);
        }

        // Find or insert target.
        //
        // Note that in case of the target type ambiguity we first try to find
        // an explicit target that resolves this ambiguity.
        //
        const target* r (nullptr);

        if (!insert || tts.size () > 1)
        {
          // Note that we skip any target type-specific searches (like for an
          // existing file) and go straight for the target object since we
          // need to find the target explicitly spelled out.
          //
          // Also, it doesn't feel like we should be able to resolve an
          // absolute path with a spelled-out extension to multiple targets.
          //
          for (const target_type* tt: tts)
            if ((r = t.ctx.targets.find (*tt, d, out, n, e, trace)) != nullptr)
              break;

          // Note: we can't do this because of the in-source builds where
          // there won't be explicit targets for non-generated headers.
          //
          // This should be harmless, however, since in our world generated
          // headers are normally spelled-out as explicit targets. And if not,
          // we will still get an error, just a bit less specific.
          //
#if 0
          if (r == nullptr && insert)
          {
            f = d / n;
            if (!e.empty ())
            {
              f += '.';
              f += e;
            }

            diag_record dr (fail);
            dr << "mapping of header " << f << " to target type is ambiguous";
            for (const target_type* tt: tts)
              dr << info << "could be " << tt->name << "{}";
            dr << info << "spell-out its target to resolve this ambiguity";
          }
#endif
        }

        // @@ OPT: move d, out, n
        //
        if (r == nullptr && insert)
          r = &search (t, *tts[0], d, out, n, &e, nullptr);

        return static_cast<const file*> (r);
      };

      // If it's not absolute then it either does not (yet) exist or is a
      // relative ""-include (see init_args() for details). Reduce the second
      // case to absolute.
      //
      // Note: we now always use absolute path to the translation unit so this
      // no longer applies. But let's keep it for posterity.
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

      const file* pt (nullptr);
      bool remapped (false);

      // If still relative then it does not exist.
      //
      if (f.relative ())
      {
        // This is probably as often an error as an auto-generated file, so
        // trace at level 4.
        //
        l4 ([&]{trace << "non-existent header '" << f << "'";});

        f.normalize ();

        // The relative path might still contain '..' (e.g., ../foo.hxx;
        // presumably ""-include'ed). We don't attempt to support auto-
        // generated headers with such inclusion styles.
        //
        if (f.normalized ())
        {
          if (!pfx_map)
            pfx_map = build_prefix_map (bs, a, t, li);

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
              // Note: value in pfx_map is not necessarily canonical.
              //
              dir_path pd (i->second.directory);
              pd.canonicalize ();

              l4 ([&]{trace << "prefix '" << d << "' mapped to " << pd;});

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
              else
                l4 ([&]{trace << "no explicit target in " << pd;});
            }
            else
              l4 ([&]{trace << "no prefix map entry for '" << d << "'";});
          }
          else
            l4 ([&]{trace << "prefix map is empty";});
        }
      }
      else
      {
        // Normalize the path unless it comes from the depdb, in which case
        // we've already done that (normally). This is also where we handle
        // src-out remap (again, not needed if cached).
        //
        if (!cache || norm)
          normalize_header (f);

        if (!cache)
        {
          if (!so_map.empty ())
          {
            // Find the most qualified prefix of which we are a sub-path.
            //
            auto i (so_map.find_sup (f));
            if (i != so_map.end ())
            {
              // Ok, there is an out tree for this headers. Remap to a path
              // from the out tree and see if there is a target for it. Note
              // that the value in so_map is not necessarily canonical.
              //
              dir_path d (i->second);
              d /= f.leaf (i->first).directory ();
              d.canonicalize ();

              pt = find (move (d), f.leaf (), false); // d is not moved from.

              if (pt != nullptr)
              {
                path p (d / f.leaf ());
                l4 ([&]{trace << "remapping " << f << " to " << p;});
                f = move (p);
                remapped = true;
              }
            }
          }
        }

        if (pt == nullptr)
        {
          l6 ([&]{trace << "entering " << f;});
          pt = find (f.directory (), f.leaf (), true);
        }
      }

      return make_pair (pt, remapped);
    }

    // Update and add to the list of prerequisite targets a header or header
    // unit target.
    //
    // Return the indication of whether it has changed or, if the passed
    // timestamp is not timestamp_unknown, is older than the target. If the
    // header does not exists nor can be generated (no rule), then issue
    // diagnostics and fail if the fail argument is true and return nullopt
    // otherwise.
    //
    // Note: this used to be a lambda inside extract_headers() so refer to the
    // body of that function for the overall picture.
    //
    optional<bool> compile_rule::
    inject_header (action a, file& t,
                   const file& pt, timestamp mt, bool f /* fail */) const
    {
      tracer trace (x, "compile_rule::inject_header");

      // Even if failing we still use try_match() in order to issue consistent
      // (with extract_headers() below) diagnostics (rather than the generic
      // "not rule to update ...").
      //
      if (!try_match (a, pt).first)
      {
        if (!f)
          return nullopt;

        diag_record dr;
        dr << fail << "header " << pt << " not found and no rule to "
           << "generate it";

        if (verb < 4)
          dr << info << "re-run with --verbose=4 for more information";
      }

      bool r (update (trace, a, pt, mt));

      // Add to our prerequisite target list.
      //
      t.prerequisite_targets[a].push_back (&pt);

      return r;
    }

    // Extract and inject header dependencies. Return the preprocessed source
    // file as well as an indication if it is usable for compilation (see
    // below for details).
    //
    // This is also the place where we handle header units which are a lot
    // more like auto-generated headers than modules. In particular, if a
    // header unit BMI is out-of-date, then we have to re-preprocess this
    // translation unit.
    //
    pair<file_cache::entry, bool> compile_rule::
    extract_headers (action a,
                     const scope& bs,
                     file& t,
                     linfo li,
                     const file& src,
                     match_data& md,
                     depdb& dd,
                     bool& update,
                     timestamp mt,
                     module_imports& imports) const
    {
      tracer trace (x, "compile_rule::extract_headers");

      context& ctx (t.ctx);

      otype ot (li.type);

      bool reprocess (cast_false<bool> (t[c_reprocess]));

      file_cache::entry psrc;
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

      // Preprocesor mode that preserves as much information as possible while
      // still performing inclusions. Also serves as a flag indicating whether
      // this compiler uses the separate preprocess and compile setup.
      //
      const char* pp (nullptr);

      switch (ctype)
      {
      case compiler_type::gcc:
        {
          // -fdirectives-only is available since GCC 4.3.0.
          //
          if (cmaj > 4 || (cmaj == 4 && cmin >= 3))
            pp = "-fdirectives-only";

          break;
        }
      case compiler_type::clang:
        {
          // -frewrite-includes is available since Clang 3.2.0.
          //
          if (cmaj > 3 || (cmaj == 3 && cmin >= 2))
            pp = "-frewrite-includes";

          break;
        }
      case compiler_type::msvc:
        {
          // Asking MSVC to preserve comments doesn't really buy us anything
          // but does cause some extra buggy behavior.
          //
          //pp = "/C";
          break;
        }
      case compiler_type::icc:
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
      bool args_gen;     // Current state of args.
      size_t args_i (0); // Start of the -M/-MD "tail".

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
      // Finally, if we are using the module mapper, then all this mess falls
      // away: we only run the compiler once, we let the diagnostics through,
      // we get a compiler error (with location information) if a header is
      // not found, and there is no problem with outdated generated headers
      // since we update/remap them before the compiler has a chance to read
      // them. Overall, this "dependency mapper" approach is how it should
      // have been done from the beginning.

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
      srcout_map so_map; // path_map<dir_path>

      // Dynamic module mapper.
      //
      bool mod_mapper (false);

      // The gen argument to init_args() is in/out. The caller signals whether
      // to force the generated header support and on return it signals
      // whether this support is enabled. The first call to init_args is
      // expected to have gen false.
      //
      // Return NULL if the dependency information goes to stdout and a
      // pointer to the temporary file path otherwise.
      //
      auto init_args = [a, &t, ot, li, reprocess,
                        &src, &md, &psrc, &sense_diag, &mod_mapper, &bs,
                        pp, &env, &args, &args_gen, &args_i, &out, &drm,
                        &so_map, this]
        (bool& gen) -> const path*
      {
        context& ctx (t.ctx);

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

          // If we are re-processing the translation unit, then allow the
          // translation unit to detect header/module dependency extraction.
          // This can be used to work around separate preprocessing bugs in
          // the compiler.
          //
          if (reprocess)
            args.push_back ("-D__build2_preprocess");

          append_options (args, t, x_poptions);
          append_options (args, t, c_poptions);

          // Add *.export.poptions from prerequisite libraries.
          //
          append_library_options (args, bs, a, t, li);

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
                if (!dir_path::traits_type::is_separator (ds.back ()))
                  ds += dir_path::traits_type::directory_separator;

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
                    const scope& bs (ctx.scopes.find_out (d));
                    if (bs.root_scope () != nullptr)
                    {
                      if (!bs.out_eq_src ())
                      {
                        const dir_path& bp (bs.out_path ());

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
          // @@ MODHDR Clang: should be solved with the dynamic module mapper
          //    if/when Clang supports it?
          //

          // Don't treat warnings as errors.
          //
          const char* werror (nullptr);
          switch (cclass)
          {
          case compiler_class::gcc:  werror = "-Werror"; break;
          case compiler_class::msvc: werror = "/WX";     break;
          }

          bool clang (ctype == compiler_type::clang);

          append_options (args, t, c_coptions, werror);
          append_options (args, t, x_coptions, werror);

          switch (cclass)
          {
          case compiler_class::msvc:
            {
              // /F*: style option availability (see perform_update()).
              //
              bool fc (cmaj >= 18 && cvariant != "clang");

              args.push_back ("/nologo");

              append_options (args, cmode);
              append_sys_hdr_options (args); // Extra system header dirs (last).

              // See perform_update() for details on overriding the default
              // exceptions and runtime.
              //
              if (x_lang == lang::cxx && !find_option_prefix ("/EH", args))
                args.push_back ("/EHsc");

              if (!find_option_prefixes ({"/MD", "/MT"}, args))
                args.push_back ("/MD");

              args.push_back ("/P");            // Preprocess to file.
              args.push_back ("/showIncludes"); // Goes to stdout (with diag).
              if (pp != nullptr)
                args.push_back (pp);            // /C (preserve comments).
              args.push_back ("/WX");           // Warning as error (see above).

              msvc_sanitize_cl (args);

              psrc = ctx.fcache.create (t.path () + x_pext, !modules);

              if (fc)
              {
                args.push_back ("/Fi:");
                args.push_back (psrc.path ().string ().c_str ());
              }
              else
              {
                out = "/Fi" + psrc.path ().string ();
                args.push_back (out.c_str ());
              }

              append_lang_options (args, md); // Compile as.
              gen = args_gen = true;
              break;
            }
          case compiler_class::gcc:
            {
              // See perform_update() for details on the choice of options.
              //
              if (ot == otype::s)
              {
                if (tclass == "linux" || tclass == "bsd")
                  args.push_back ("-fPIC");
              }

              if (ctype == compiler_type::clang && tsys == "win32-msvc")
              {
                initializer_list<const char*> os {"-nostdlib", "-nostartfiles"};
                if (!find_options (os, cmode) && !find_options (os, args))
                {
                  args.push_back ("-D_MT");
                  args.push_back ("-D_DLL");
                }
              }

              if (ctype == compiler_type::clang && cvariant == "emscripten")
              {
                if (x_lang == lang::cxx)
                {
                  if (!find_option_prefix ("DISABLE_EXCEPTION_CATCHING=", args))
                  {
                    args.push_back ("-s");
                    args.push_back ("DISABLE_EXCEPTION_CATCHING=0");
                  }
                }
              }

              append_options (args, cmode,
                              cmode.size () - (modules && clang ? 1 : 0));
              append_sys_hdr_options (args); // Extra system header dirs (last).

              // Setup the dynamic module mapper if needed.
              //
              // Note that it's plausible in the future we will use it even if
              // modules are disabled, for example, to implement better -MG.
              // In which case it will have probably be better called a
              // "dependency mapper".
              //
              if (modules)
              {
                if (ctype == compiler_type::gcc)
                {
                  args.push_back ("-fmodule-mapper=<>");
                  mod_mapper = true;
                }
              }

              // Depending on the compiler, decide whether (and how) we can
              // produce preprocessed output as a side effect of dependency
              // extraction.
              //
              // Note: -MM -MG skips missing <>-included.

              // Clang's -M does not imply -w (disable warnings). We also
              // don't need them in the -MD case (see above) so disable for
              // both.
              //
              if (clang)
                args.push_back ("-w");

              append_lang_options (args, md);

              if (pp != nullptr)
              {
                // With the GCC module mapper the dependency information is
                // written directly to depdb by the mapper.
                //
                if (ctype == compiler_type::gcc && mod_mapper)
                {
                  // Note that in this mode we don't have -MG re-runs. In a
                  // sense we are in the -MG mode (or, more precisely, the "no
                  // -MG required" mode) right away.
                  //
                  args.push_back ("-E");
                  args.push_back (pp);
                  gen = args_gen = true;
                  r = &drm.path; // Bogus/hack to force desired process start.
                }
                else
                {
                  // Previously we used '*' as a target name but it gets
                  // expanded to the current directory file names by GCC (4.9)
                  // that comes with MSYS2 (2.4). Yes, this is the (bizarre)
                  // behavior of GCC being executed in the shell with -MQ '*'
                  // option and not just -MQ *.
                  //
                  args.push_back ("-MQ"); // Quoted target name.
                  args.push_back ("^");   // Old versions can't do empty.

                  // Note that the options are carefully laid out to be easy
                  // to override (see below).
                  //
                  args_i = args.size ();

                  args.push_back ("-MD");
                  args.push_back ("-E");
                  args.push_back (pp);

                  // Dependency output.
                  //
                  // GCC until version 8 was not capable of writing the
                  // dependency information to stdout. We also either need to
                  // sense the diagnostics on the -E runs (which we currently
                  // can only do if we don't need to read stdout) or we could
                  // be communicating with the module mapper via stdin/stdout.
                  //
                  if (ctype == compiler_type::gcc)
                  {
                    // Use the .t extension (for "temporary"; .d is taken).
                    //
                    r = &(drm = auto_rmfile (t.path () + ".t")).path;
                  }

                  args.push_back ("-MF");
                  args.push_back (r != nullptr ? r->string ().c_str () : "-");

                  sense_diag = (ctype == compiler_type::gcc);
                  gen = args_gen = false;
                }

                // Preprocessor output.
                //
                psrc = ctx.fcache.create (t.path () + x_pext, !modules);
                args.push_back ("-o");
                args.push_back (psrc.path ().string ().c_str ());
              }
              else
              {
                args.push_back ("-MQ");
                args.push_back ("^");
                args.push_back ("-M");
                args.push_back ("-MG"); // Treat missing headers as generated.
                gen = args_gen = true;
              }

              break;
            }
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
          assert (gen != args_gen && args_i != 0);

          size_t i (args_i);

          if (gen)
          {
            // Overwrite.
            //
            args[i++] = "-M";
            args[i++] = "-MG";
            args[i++] = src.path ().string ().c_str ();
            args[i]   = nullptr;

            if (ctype == compiler_type::gcc)
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

            if (ctype == compiler_type::gcc)
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

      // Enter as a target, update, and add to the list of prerequisite
      // targets a header file. Depending on the cache flag, the file is
      // assumed to either have come from the depdb cache or from the compiler
      // run. Return true if the extraction process should be restarted and
      // false otherwise. Return nullopt if the header is not found and cannot
      // be generated, the diagnostics has been issued, but the failure has
      // been deferred to the compiler run in order to get better diagnostics.
      //
      auto add = [a, &bs, &t, li,
                  &pfx_map, &so_map,
                  &dd, &skip_count,
                  this] (path hp, bool cache, timestamp mt) -> optional<bool>
      {
        context& ctx (t.ctx);

        // We can only defer the failure if we will be running the compiler.
        //
        // We also used to only do it in the "keep going" mode but that proved
        // to be inconvenient: some users like to re-run a failed build with
        // -s not to get "swamped" with errors.
        //
        bool df (!ctx.match_only && !ctx.dry_run_option);

        const file* ht (enter_header (a, bs, t, li,
                                      move (hp), cache, false /* norm */,
                                      pfx_map, so_map).first);
        if (ht == nullptr)
        {
          diag_record dr;
          dr << error << "header '" << hp << "' not found and no rule to "
             << "generate it";

          if (df)
            dr << info << "failure deferred to compiler diagnostics";

          if (verb < 4)
            dr << info << "re-run with --verbose=4 for more information";

          if (df) return nullopt; else dr << endf;
        }

        // If we are reading the cache, then it is possible the file has since
        // been removed (think of a header in /usr/local/include that has been
        // uninstalled and now we need to use one from /usr/include). This
        // will lead to the match failure which we translate to a restart.
        //
        if (optional<bool> u = inject_header (a, t, *ht, mt, false /* fail */))
        {
          // Verify/add it to the dependency database.
          //
          if (!cache)
            dd.expect (ht->path ());

          skip_count++;
          return *u;
        }
        else if (!cache)
        {
          diag_record dr;
          dr << error << "header " << *ht << " not found and no rule to "
             << "generate it";

          if (df)
            dr << info << "failure deferred to compiler diagnostics";

          if (verb < 4)
            dr << info << "re-run with --verbose=4 for more information";

          if (df) return nullopt; else dr << endf;
        }

        dd.write (); // Invalidate this line.
        return true;
      };

      // As above but for a header unit. Note that currently it is only used
      // for the cached case (the other case is handled by the mapper). We
      // also assume that the path may not be normalized (see below).
      //
      auto add_unit = [a, &bs, &t, li,
                       &pfx_map, &so_map,
                       &dd, &skip_count, &md,
                       this] (path hp, path bp, timestamp mt) -> optional<bool>
      {
        context& ctx (t.ctx);
        bool df (!ctx.match_only && !ctx.dry_run_option);

        const file* ht (
          enter_header (a, bs, t, li,
                        move (hp), true /* cache */, true /* norm */,
                        pfx_map, so_map).first);

        if (ht == nullptr)
        {
          diag_record dr;
          dr << error << "header '" << hp << "' not found and no rule to "
             << "generate it";

          if (df)
            dr << info << "failure deferred to compiler diagnostics";

          if (verb < 4)
            dr << info << "re-run with --verbose=4 for more information";

          if (df) return nullopt; else dr << endf;
        }

        // Again, looks like we have to update the header explicitly since
        // we want to restart rather than fail if it cannot be updated.
        //
        if (inject_header (a, t, *ht, mt, false /* fail */))
        {
          const file& bt (make_header_sidebuild (a, bs, t, li, *ht));

          // It doesn't look like we need the cache semantics here since given
          // the header, we should be able to build its BMI. In other words, a
          // restart is not going to change anything.
          //
          optional<bool> u (inject_header (a, t, bt, mt, true /* fail */));
          assert (u); // Not from cache.

          if (bt.path () == bp)
          {
            md.header_units++;
            skip_count++;
            return *u;
          }
        }

        dd.write (); // Invalidate this line.
        return true;
      };

      // See init_args() above for details on generated header support.
      //
      bool gen (false);
      optional<bool>   force_gen;
      optional<size_t> force_gen_skip; // Skip count at last force_gen run.

      const path* drmp (nullptr); // Points to drm.path () if active.

      // If nothing so far has invalidated the dependency database, then try
      // the cached data before running the compiler.
      //
      bool cache (!update);

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
              // See apply() for details on the extra MSVC check.
              //
              return modules && (ctype != compiler_type::msvc ||
                                 md.type != unit_type::module_intf)
                ? make_pair (ctx.fcache.create_existing (t.path () + x_pext),
                             true)
                : make_pair (file_cache::entry (), false);
            }

            // This can be a header or a header unit (mapping).
            //
            // If this header (unit) came from the depdb, make sure it is no
            // older than the target (if it has changed since the target was
            // updated, then the cached data is stale).
            //
            optional<bool> r;
            if ((*l)[0] == '@')
            {
              // @@ What if the header path contains spaces? How is GCC
              //    handling this?

              size_t p (l->find (' ', 2));

              if (p != string::npos)
              {
                // Note that the header path is absolute and commonly but not
                // necessarily normalized.
                //
                path h (*l, 2, p - 2);
                path b (move (l->erase (0, p + 1)));

                r = add_unit (move (h), move (b), mt);
              }
              else
                r = true; // Corrupt database?
            }
            else
              r = add (path (move (*l)), true /* cache */, mt);

            if (r)
            {
              restart = *r;

              if (restart)
              {
                update = true;
                l6 ([&]{trace << "restarting (cache)";});
                break;
              }
            }
            else
            {
              // Trigger recompilation and mark as expected to fail.
              //
              update = true;
              md.deferred_failure = true;

              // Bail out early if we have deferred a failure.
              //
              return make_pair (file_cache::entry (), false);
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

            // If we are producing the preprocessed output, get its write
            // handle.
            //
            file_cache::write psrcw (psrc
                                     ? psrc.init_new ()
                                     : file_cache::write ());

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

              // In some cases we may need to ignore the error return status.
              // The good_error flag keeps track of that. Similarly, sometimes
              // we expect the error return status based on the output that we
              // see. The bad_error flag is for that.
              //
              bool good_error (false), bad_error (false);

              // If we have no generated header support, then suppress all
              // diagnostics (if things go badly we will restart with this
              // support).
              //
              if (drmp == nullptr) // Dependency info goes to stdout.
              {
                assert (!sense_diag); // Note: could support with fdselect().

                // For VC with /P the dependency info and diagnostics all go
                // to stderr so redirect it to stdout.
                //
                pr = process (
                  cpath,
                  args.data (),
                  0,
                  -1,
                  cclass == compiler_class::msvc ? 1 : gen ? 2 : -2,
                  nullptr, // CWD
                  env.empty () ? nullptr : env.data ());
              }
              else // Dependency info goes to a temporary file.
              {
                pr = process (cpath,
                              args.data (),
                              mod_mapper ? -1 : 0,
                              mod_mapper ? -1 : 2, // Send stdout to stderr.
                              gen ? 2 : sense_diag ? -1 : -2,
                              nullptr, // CWD
                              env.empty () ? nullptr : env.data ());

                // Monitor for module mapper requests and/or diagnostics. If
                // diagnostics is detected, mark the preprocessed output as
                // unusable for compilation.
                //
                if (mod_mapper || sense_diag)
                {
                  module_mapper_state mm_state (skip_count, imports);

                  const char* w (nullptr);
                  try
                  {
                    // For now we don't need to do both so let's use a simpler
                    // blocking implementation. Note that the module mapper
                    // also needs to be adjusted when switching to the
                    // non-blocking version.
                    //
#if 1
                    assert (mod_mapper != sense_diag);

                    if (mod_mapper)
                    {
                      w = "module mapper request";

                      // Note: the order is important (see the non-blocking
                      // verison for details).
                      //
                      ifdstream is (move (pr.in_ofd),
                                    fdstream_mode::skip,
                                    ifdstream::badbit);
                      ofdstream os (move (pr.out_fd));

                      do
                      {
                        if (!gcc_module_mapper (mm_state,
                                                a, bs, t, li,
                                                is, os,
                                                dd, update, bad_error,
                                                pfx_map, so_map))
                          break;

                      } while (!is.eof ());

                      os.close ();
                      is.close ();
                    }

                    if (sense_diag)
                    {
                      w = "diagnostics";
                      ifdstream is (move (pr.in_efd), fdstream_mode::skip);
                      puse = puse && (is.peek () == ifdstream::traits_type::eof ());
                      is.close ();
                    }
#else
                    fdselect_set fds;
                    auto add = [&fds] (const auto_fd& afd) -> fdselect_state*
                    {
                      int fd (afd.get ());
                      fdmode (fd, fdstream_mode::non_blocking);
                      fds.push_back (fd);
                      return &fds.back ();
                    };

                    // Note that while we read both streams until eof in
                    // normal circumstances, we cannot use fdstream_mode::skip
                    // for the exception case on both of them: we may end up
                    // being blocked trying to read one stream while the
                    // process may be blocked writing to the other. So in case
                    // of an exception we only skip the diagnostics and close
                    // the mapper stream hard. The latter should happen first
                    // so the order of the following variable is important.
                    //
                    ifdstream es;
                    ofdstream os;
                    ifdstream is;

                    fdselect_state* ds (nullptr);
                    if (sense_diag)
                    {
                      w = "diagnostics";
                      ds = add (pr.in_efd);
                      es.open (move (pr.in_efd), fdstream_mode::skip);
                    }

                    fdselect_state* ms (nullptr);
                    if (mod_mapper)
                    {
                      w = "module mapper request";
                      ms = add (pr.in_ofd);
                      is.open (move (pr.in_ofd));
                      os.open (move (pr.out_fd)); // Note: blocking.
                    }

                    // Set each state pointer to NULL when the respective
                    // stream reaches eof.
                    //
                    while (ds != nullptr || ms != nullptr)
                    {
                      w = "output";
                      ifdselect (fds);

                      // First read out the diagnostics in case the mapper
                      // interaction produces more. To make sure we don't get
                      // blocked by full stderr, the mapper should only handle
                      // one request at a time.
                      //
                      if (ds != nullptr && ds->ready)
                      {
                        w = "diagnostics";

                        for (char buf[4096];;)
                        {
                          streamsize c (sizeof (buf));
                          streamsize n (es.readsome (buf, c));

                          if (puse && n > 0)
                            puse = false;

                          if (n < c)
                            break;
                        }

                        if (es.eof ())
                        {
                          es.close ();
                          ds->fd = nullfd;
                          ds = nullptr;
                        }
                      }

                      if (ms != nullptr && ms->ready)
                      {
                        w = "module mapper request";

                        gcc_module_mapper (mm_state,
                                           a, bs, t, li,
                                           is, os,
                                           dd, update, bad_error,
                                           pfx_map, so_map);
                        if (is.eof ())
                        {
                          os.close ();
                          is.close ();
                          ms->fd = nullfd;
                          ms = nullptr;
                        }
                      }
                    }
#endif
                  }
                  catch (const io_error& e)
                  {
                    if (pr.wait ())
                      fail << "io error handling " << x_lang << " compiler "
                           << w << ": " << e;

                    // Fall through.
                  }

                  if (mod_mapper)
                    md.header_units += mm_state.header_units;
                }

                // The idea is to reduce this to the stdout case.
                //
                pr.wait ();

                // With -MG we want to read dependency info even if there is
                // an error (in case an outdated header file caused it). But
                // with the GCC module mapper an error is non-negotiable, so
                // to speak, and so we want to skip all of that. In fact, we
                // now write directly to depdb without generating and then
                // parsing an intermadiate dependency makefile.
                //
                pr.in_ofd = (ctype == compiler_type::gcc && mod_mapper)
                  ? auto_fd (nullfd)
                  : fdopen (*drmp, fdopen_mode::in);
              }

              if (pr.in_ofd != nullfd)
              {
                // We may not read all the output (e.g., due to a restart).
                // Before we used to just close the file descriptor to signal
                // to the other end that we are not interested in the rest.
                // This works fine with GCC but Clang (3.7.0) finds this
                // impolite and complains, loudly (broken pipe). So now we are
                // going to skip until the end.
                //
                ifdstream is (move (pr.in_ofd),
                              fdstream_mode::text | fdstream_mode::skip,
                              ifdstream::badbit);

                size_t skip (skip_count);
                string l; // Reuse.
                for (bool first (true), second (false); !restart; )
                {
                  if (eof (getline (is, l)))
                    break;

                  l6 ([&]{trace << "header dependency line '" << l << "'";});

                  // Parse different dependency output formats.
                  //
                  switch (cclass)
                  {
                  case compiler_class::msvc:
                    {
                      // The first line should be the file we are compiling,
                      // unless this is clang-cl. If it is not, then something
                      // went wrong even before we could compile anything
                      // (e.g., file does not exist). In this case the first
                      // line (and everything after it) is presumably
                      // diagnostics.
                      //
                      // It can, however, be a command line warning, for
                      // example:
                      //
                      // cl : Command line warning D9025 : overriding '/W3' with '/W4'
                      //
                      // So we try to detect and skip them assuming they will
                      // also show up during the compilation proper.
                      //
                      if (first)
                      {
                        if (cvariant != "clang")
                        {
                          if (l != src.path ().leaf ().string ())
                          {
                            // D8XXX are errors while D9XXX are warnings.
                            //
                            size_t p (msvc_sense_diag (l, 'D').first);
                            if (p != string::npos && l[p] == '9')
                              continue;

                            text << l;
                            bad_error = true;
                            break;
                          }
                          // Fall through.
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
                        // @@ TMP: but this does seem to happen in some rare,
                        //    hard to reproduce situations.
#if 0
                        assert (!good_error);
#else
                        if (good_error)
                        {
                          info   << "previously existing header '" << f << "'"
                                 << " appears to have disappeared during build" <<
                            info << "line: " << l <<
                            info << "skip: " << skip <<
                            info << "please report at "
                                 << "https://github.com/build2/build2/issues/80";

                          assert (!good_error);
                        }
#endif
                        skip--;
                      }
                      else
                      {
                        if (optional<bool> r = add (path (move (f)),
                                                    false /* cache */,
                                                    pmt))
                        {
                          restart = *r;

                          // If the header does not exist (good_error), then
                          // restart must be true. Except that it is possible
                          // that someone running in parallel has already
                          // updated it. In this case we must force a restart
                          // since we haven't yet seen what's after this
                          // at-that-time-non-existent header.
                          //
                          // We also need to force the target update (normally
                          // done by add()).
                          //
                          if (good_error)
                            restart = true;
                          //
                          // And if we have updated the header (restart is
                          // true), then we may end up in this situation: an
                          // old header got included which caused the
                          // preprocessor to fail down the line. So if we are
                          // restarting, set the good error flag in case the
                          // process fails because of something like this (and
                          // if it is for a valid reason, then we will pick it
                          // up on the next round).
                          //
                          else if (restart)
                            good_error = true;

                          if (restart)
                          {
                            update = true;
                            l6 ([&]{trace << "restarting";});
                          }
                        }
                        else
                        {
                          // Trigger recompilation and mark as expected to
                          // fail.
                          //
                          update = true;
                          md.deferred_failure = true;
                        }
                      }

                      break;
                    }
                  case compiler_class::gcc:
                    {
                      // Make dependency declaration.
                      //
                      size_t pos (0);

                      if (first)
                      {
                        // Empty/invalid output should mean the wait() call
                        // below will return false.
                        //
                        if (l.empty ()  ||
                            l[0] != '^' || l[1] != ':' || l[2] != ' ')
                        {
                          // @@ Hm, we don't seem to redirect stderr to stdout
                          //    for this class of compilers so I wonder why
                          //    we are doing this?
                          //
                          if (!l.empty ())
                            text << l;

                          bad_error = true;
                          break;
                        }

                        first = false;
                        second = true;

                        // While normally we would have the source file on the
                        // first line, if too long, it will be moved to the
                        // next line and all we will have on this line is:
                        // "^: \".
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

                        if (optional<bool> r = add (path (move (f)),
                                                    false /* cache */,
                                                    pmt))
                        {
                          restart = *r;

                          if (restart)
                          {
                            // The same "preprocessor may fail down the line"
                            // logic as above.
                            //
                            good_error = true;

                            update = true;
                            l6 ([&]{trace << "restarting";});
                            break;
                          }
                        }
                        else
                        {
                          // Trigger recompilation, mark as expected to fail,
                          // and bail out.
                          //
                          update = true;
                          md.deferred_failure = true;
                          break;
                        }
                      }

                      break;
                    }
                  }

                  if (bad_error || md.deferred_failure)
                    break;
                }

                // Bail out early if we have deferred a failure.
                //
                if (md.deferred_failure)
                {
                  is.close ();
                  return make_pair (file_cache::entry (), false);
                }

                // In case of VC, we are parsing stderr and if things go
                // south, we need to copy the diagnostics for the user to see.
                //
                if (bad_error && cclass == compiler_class::msvc)
                {
                  // We used to just dump the whole rdbuf but it turns out VC
                  // may continue writing include notes interleaved with the
                  // diagnostics. So we have to filter them out.
                  //
                  for (; !eof (getline (is, l)); )
                  {
                    pair<size_t, size_t> p (msvc_sense_diag (l, 'C'));
                    if (p.first != string::npos             &&
                        l.compare (p.first, 4, "1083") != 0 &&
                        msvc_header_c1083 (l, p))
                    {
                      diag_stream_lock () << l << endl;
                    }
                  }
                }

                is.close ();

                // This is tricky: it is possible that in parallel someone has
                // generated all our missing headers and we wouldn't restart
                // normally.
                //
                // In this case we also need to force the target update (which
                // is normally done by add()).
                //
                if (force_gen && *force_gen)
                {
                  restart = update = true;
                  force_gen = false;
                }
              }

              if (pr.wait ())
              {
                if (!bad_error) // Ignore expected successes (we are done).
                {
                  if (!restart && psrc)
                    psrcw.close ();

                  continue;
                }

                fail << "expected error exit status from " << x_lang
                     << " compiler";
              }
              else if (pr.exit->normal ())
              {
                if (good_error) // Ignore expected errors (restart).
                  continue;
              }

              // Fall through.
            }
            catch (const io_error& e)
            {
              if (pr.wait ())
                fail << "unable to read " << x_lang << " compiler header "
                     << "dependency output: " << e;

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
                // In some pathological situations we may end up switching
                // back and forth indefinitely without making any headway. So
                // we use skip_count to track our progress.
                //
                // Examples that have been encountered so far:
                //
                // - Running out of disk space.
                //
                // - Using __COUNTER__ in #if which is incompatible with the
                //   GCC's -fdirectives-only mode.
                //
                // - A Clang bug: https://bugs.llvm.org/show_bug.cgi?id=35580
                //
                // So let's show the yo-yo'ing command lines and ask the user
                // to investigate.
                //
                // Note: we could restart one more time but this time without
                // suppressing diagnostics. This could be useful since, say,
                // running out of disk space may not reproduce on its own (for
                // example, because we have removed all the partially
                // preprocessed source files).
                //
                if (force_gen_skip && *force_gen_skip == skip_count)
                {
                  diag_record dr (fail);

                  dr << "inconsistent " << x_lang << " compiler behavior" <<
                    info << "run the following two commands to investigate";

                  dr << info;
                  print_process (dr, args.data ()); // No pipes.

                  init_args ((gen = true));
                  dr << info << "";
                  print_process (dr, args.data ()); // No pipes.
                }

                restart = true;
                force_gen = true;
                force_gen_skip = skip_count;
                l6 ([&]{trace << "restarting with forced generated headers";});
              }
              continue;
            }
            else
              run_finish (args, pr); // Throws.
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

      // Add the terminating blank line (we are updating depdb).
      //
      dd.expect ("");

      puse = puse && !reprocess && psrc;
      return make_pair (move (psrc), puse);
    }

    // Return the translation unit information (last argument) and its
    // checksum (result). If the checksum is empty, then it should not be
    // used.
    //
    string compile_rule::
    parse_unit (action a,
                file& t,
                linfo li,
                const file& src,
                file_cache::entry& psrc,
                const match_data& md,
                const path& dd,
                unit& tu) const
    {
      tracer trace (x, "compile_rule::parse_unit");

      otype ot (li.type);

      // If things go wrong give the user a bit extra context. Let's call it
      // "scanning" instead of "parsing" since this has become an established
      // term.
      //
      auto df = make_diag_frame (
        [&src](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while scanning " << src;
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
      small_vector<string, 2> header_args; // Header unit options storage.

      const path* sp; // Source path.

      // @@ MODHDR: If we are reprocessing, then will need module mapper for
      //            include translation. Hairy... Can't we add support for
      //            include translation in file mapper?
      //
      bool reprocess (cast_false<bool> (t[c_reprocess]));

      bool ps; // True if extracting from psrc.
      if (md.pp < preprocessed::modules)
      {
        // If we were instructed to reprocess the source during compilation,
        // then also reprocess it here. While the preprocessed output may be
        // usable for our needs, to be safe we assume it is not (and later we
        // may extend cc.reprocess to allow specifying where reprocessing is
        // needed).
        //
        ps = psrc && !reprocess;
        sp = &(ps ? psrc.path () : src.path ());

        // VC's preprocessed output, if present, is fully preprocessed.
        //
        if (cclass != compiler_class::msvc || !ps)
        {
          // This should match with how we setup preprocessing and is pretty
          // similar to init_args() from extract_headers().
          //
          args.push_back (cpath.recall_string ());

          if (reprocess)
            args.push_back ("-D__build2_preprocess");

          append_options (args, t, x_poptions);
          append_options (args, t, c_poptions);

          append_library_options (args, t.base_scope (), a, t, li);

          if (md.symexport)
            append_symexport_options (args, t);

          // Make sure we don't fail because of warnings.
          //
          // @@ Can be both -WX and /WX.
          //
          const char* werror (nullptr);
          switch (cclass)
          {
          case compiler_class::gcc:  werror = "-Werror"; break;
          case compiler_class::msvc: werror = "/WX";     break;
          }

          bool clang (ctype == compiler_type::clang);

          append_options (args, t, c_coptions, werror);
          append_options (args, t, x_coptions, werror);

          append_header_options (env, args, header_args, a, t, md, dd);

          switch (cclass)
          {
          case compiler_class::msvc:
            {
              args.push_back ("/nologo");

              append_options (args, cmode);
              append_sys_hdr_options (args);

              if (x_lang == lang::cxx && !find_option_prefix ("/EH", args))
                args.push_back ("/EHsc");

              if (!find_option_prefixes ({"/MD", "/MT"}, args))
                args.push_back ("/MD");

              args.push_back ("/E");
              // args.push_back ("/C"); // See above.

              msvc_sanitize_cl (args);

              append_lang_options (args, md); // Compile as.

              break;
            }
          case compiler_class::gcc:
            {
              if (ot == otype::s)
              {
                if (tclass == "linux" || tclass == "bsd")
                  args.push_back ("-fPIC");
              }

              if (ctype == compiler_type::clang && tsys == "win32-msvc")
              {
                initializer_list<const char*> os {"-nostdlib", "-nostartfiles"};
                if (!find_options (os, cmode) && !find_options (os, args))
                {
                  args.push_back ("-D_MT");
                  args.push_back ("-D_DLL");
                }
              }

              if (ctype == compiler_type::clang && cvariant == "emscripten")
              {
                if (x_lang == lang::cxx)
                {
                  if (!find_option_prefix ("DISABLE_EXCEPTION_CATCHING=", args))
                  {
                    args.push_back ("-s");
                    args.push_back ("DISABLE_EXCEPTION_CATCHING=0");
                  }
                }
              }

              append_options (args, cmode,
                              cmode.size () - (modules && clang ? 1 : 0));
              append_sys_hdr_options (args);

              args.push_back ("-E");
              append_lang_options (args, md);

              // Options that trigger preprocessing of partially preprocessed
              // output are a bit of a compiler-specific voodoo.
              //
              if (ps)
              {
                if (ctype == compiler_type::gcc)
                {
                  // Note that only these two *plus* -x do the trick.
                  //
                  args.push_back ("-fpreprocessed");
                  args.push_back ("-fdirectives-only");
                }
              }

              break;
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
        // If we are compiling the preprocessed output, get its read handle.
        //
        file_cache::read psrcr (ps ? psrc.open () : file_cache::read ());

        // Temporarily disable the removal of the preprocessed file in case of
        // an error. We re-enable it below.
        //
        bool ptmp (ps && psrc.temporary);
        if (ptmp)
          psrc.temporary = false;

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
          p.parse (is, path_name (*sp), tu);

          is.close ();

          if (pr.wait ())
          {
            if (ptmp)
              psrc.temporary = true; // Re-enable.

            unit_type& ut (tu.type);
            module_info& mi (tu.module_info);

            if (!modules)
            {
              if (ut != unit_type::non_modular || !mi.imports.empty ())
                fail << "modules support required by " << src;
            }
            else
            {
              // Sanity checks.
              //
              // If we are compiling a module interface or partition, make
              // sure the translation unit has the necessary declarations.
              //
              if (ut != unit_type::module_intf      &&
                  ut != unit_type::module_intf_part &&
                  ut != unit_type::module_impl_part &&
                  src.is_a (*x_mod))
                fail << src << " is not a module interface or partition unit";

              // A header unit should look like a non-modular translation unit.
              //
              if (md.type == unit_type::module_header)
              {
                if (ut != unit_type::non_modular)
                  fail << "module declaration in header unit " << src;

                ut = md.type;
                mi.name = src.path ().string ();
              }

              // Prior to 15.5 (19.12) VC was not using the 'export module M;'
              // syntax so we use the preprequisite type to distinguish
              // between interface and implementation units.
              //
              // @@ TMP: probably outdated.
              //
              if (ctype == compiler_type::msvc && cmaj == 19 && cmin <= 11)
              {
                if (ut == unit_type::module_impl && src.is_a (*x_mod))
                  ut = unit_type::module_intf;
              }
            }

            // If we were forced to reprocess, assume the checksum is not
            // accurate (parts of the translation unit could have been
            // #ifdef'ed out; see __build2_preprocess).
            //
            return reprocess ? string () : move (p.checksum);
          }

          // Fall through.
        }
        catch (const io_error& e)
        {
          if (pr.wait ())
            fail << "unable to read " << x_lang << " preprocessor output: "
                 << e;

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
          run_finish (args, pr); // Throws.
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
    void compile_rule::
    extract_modules (action a,
                     const scope& bs,
                     file& t,
                     linfo li,
                     const compile_target_types& tts,
                     const file& src,
                     match_data& md,
                     module_info&& mi,
                     depdb& dd,
                     bool& update) const
    {
      tracer trace (x, "compile_rule::extract_modules");

      // If things go wrong, give the user a bit extra context.
      //
      auto df = make_diag_frame (
        [&src](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while extracting module dependencies from " << src;
        });

      unit_type ut (md.type);
      module_imports& is (mi.imports);

      // Search and match all the modules we depend on. If this is a module
      // implementation unit, then treat the module itself as if it was
      // imported (we insert it first since for some compilers we have to
      // differentiate between this special module and real imports). Note
      // that module partitions do not have this implied import semantics.
      // Note also: move.
      //
      if (ut == unit_type::module_impl)
        is.insert (
          is.begin (),
          module_import {import_type::module_intf, move (mi.name), false, 0});

      // The change to the set of imports would have required a change to
      // source code (or options). Changes to the bmi{}s themselves will be
      // detected via the normal prerequisite machinery. However, the same set
      // of imports could be resolved to a different set of bmi{}s (in a sense
      // similar to changing the source file). To detect this we calculate and
      // store a hash of all (not just direct) bmi{}'s paths.
      //
      sha256 cs;

      if (!is.empty ())
        md.modules = search_modules (a, bs, t, li, tts.bmi, src, is, cs);

      if (dd.expect (cs.string ()) != nullptr)
        update = true;

      // Save the module map for compilers that use it.
      //
      switch (ctype)
      {
      case compiler_type::gcc:
        {
          // We don't need to redo this if the above hash hasn't changed and
          // the database is still valid.
          //
          if (dd.writing () || !dd.skip ())
          {
            // Note that for header unit, name will be an absolute and
            // normalized path since that's the TU path we pass to the
            // compiler.
            //
            auto write = [&dd] (const string& name, const path& file)
            {
              dd.write ("@ ", false);
              dd.write (name, false);
              dd.write (' ', false);
              dd.write (file);
            };

            // The output mapping is provided in the same way as input.
            //
            if (ut == unit_type::module_intf      ||
                ut == unit_type::module_intf_part ||
                ut == unit_type::module_impl_part ||
                ut == unit_type::module_header)
              write (mi.name, t.path ());

            if (size_t start = md.modules.start)
            {
              // Note that we map both direct and indirect imports to override
              // any module paths that might be stored in the BMIs (or
              // resolved relative to "repository path", whatever that is).
              //
              const auto& pts (t.prerequisite_targets[a]);
              for (size_t i (start); i != pts.size (); ++i)
              {
                if (const target* m = pts[i])
                {
                  // Save a variable lookup by getting the module name from
                  // the import list (see search_modules()).
                  //
                  // Note: all real modules (not header units).
                  //
                  write (is[i - start].name, m->as<file> ().path ());
                }
              }
            }
          }
          break;
        }
      default:
        break;
      }

      // Set the cc.module_name rule-specific variable if this is an interface
      // or partition unit. Note that it may seem like a good idea to set it
      // on the bmi{} group to avoid duplication. We, however, cannot do it
      // MT-safely since we don't match the group.
      //
      // @@ MODHDR TODO: do we need this for header units? Currently we don't
      //    see header units here.
      //
      if (ut == unit_type::module_intf      ||
          ut == unit_type::module_intf_part ||
          ut == unit_type::module_impl_part
          /*ut == unit_type::module_header*/)
      {
        if (value& v = t.state[a].assign (c_module_name))
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
    module_positions compile_rule::
    search_modules (action a,
                    const scope& bs,
                    file& t,
                    linfo li,
                    const target_type& btt,
                    const file& src,
                    module_imports& imports,
                    sha256& cs) const
    {
      tracer trace (x, "compile_rule::search_modules");

      // NOTE: currently we don't see header unit imports (they are handled by
      //       extract_headers() and are not in imports).

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
      // name (`core`). So what we are going to do is require that, within a
      // pool (library, executable), the interface file names contain enough
      // of the module name tail to unambiguously resolve all the module
      // imports. On our side we are going to implement a "fuzzy" module name
      // to file name match. This should be reliable enough since we will
      // always verify our guesses once we match the target and extract the
      // actual module name. Plus, the user will always have the option of
      // resolving any impasses by specifying the module name explicitly.
      //
      // So, the fuzzy match: the idea is that each match gets a score, the
      // number of characters in the module name that got matched. A match
      // with the highest score is used. And we use the (length + 1) for a
      // match against an actual module name.
      //
      // Actually, the scoring system is a bit more elaborate than that.
      // Consider module name core.window and two files, window.mxx and
      // abstract-window.mxx: which one is likely to define this module?
      // Clearly the first, but in the above-described scheme they will get
      // the same score. More generally, consider these "obvious" (to the
      // human, that is) situations:
      //
      //   window.mxx          vs  abstract-window.mxx
      //   details/window.mxx  vs  abstract-window.mxx
      //   gtk-window.mxx      vs  gtk-abstract-window.mxx
      //
      // To handle such cases we are going to combine the above primary score
      // with the following secondary scores (in that order):
      //
      // A) Strength of separation between matched and unmatched parts:
      //
      //    '\0' > directory separator > other separator > unseparated
      //
      //    Here '\0' signifies nothing to separate (unmatched part is empty).
      //
      // B) Shortness of the unmatched part.
      //
      // Finally, for the fuzzy match we require a complete match of the last
      // module (or partition) component. Failed that, we will match `format`
      // to `print` because the last character (`t`) is the same.
      //
      // For std.* modules we only accept non-fuzzy matches (think std.core vs
      // some core.mxx). And if such a module is unresolved, then we assume it
      // is pre-built and will be found by some other means (e.g., VC's
      // IFCPATH).
      //
      // Note also that we handle module partitions the same as submodules. In
      // other words, for matching, `.` and `:` are treated the same.
      //
      auto match_max = [] (const string& m) -> size_t
      {
        // The primary and sub-scores are packed in the following decimal
        // representation:
        //
        // PPPPABBBB
        //
        // Where PPPP is the primary score, A is the A) score, and BBBB is
        // the B) scope described above. Zero signifies no match.
        //
        // We use decimal instead of binary packing to make it easier for the
        // human to separate fields in the trace messages, during debugging,
        // etc.
        //
        return m.size () * 100000 + 99999; // Maximum match score.
      };

      auto match = [] (const string& f, const string& m) -> size_t
      {
        auto char_sep = [] (char c) -> char
        {
          // Return the character (translating directory seperators to '/') if
          // it is a separator and '\0' otherwise (so can be used as bool).
          //
          return (c == '_' || c == '-' || c == '.'    ? c   :
                  path::traits_type::is_separator (c) ? '/' : '\0');
        };

        auto case_sep = [] (char c1, char c2)
        {
          return (alpha (c1) &&
                  alpha (c2) &&
                  (ucase (c1) == c1) != (ucase (c2) == c2));
        };

        auto mod_sep = [] (char c) {return c == '.' || c == ':';};

        size_t fn (f.size ()), fi (fn);
        size_t mn (m.size ()), mi (mn);

        // True if the previous character was counted as a real (that is,
        // non-case changing) separator.
        //
        bool fsep (false);
        bool msep (false);

        // We require complete match of at least last module component.
        //
        bool match (false);

        // Scan backwards for as long as we match. Keep track of the previous
        // character for case change detection.
        //
        for (char fc, mc, fp ('\0'), mp ('\0');
             fi != 0 && mi != 0;
             fp = fc, mp = mc, --fi, --mi)
        {
          fc = f[fi - 1];
          mc = m[mi - 1];

          if (icasecmp (fc, mc) == 0)
          {
            fsep = msep = false;
            continue;
          }

          // We consider all separators equal and character case change being
          // a separators. Some examples of the latter:
          //
          // foo.bar
          // foo:bar
          //  fooBAR
          //  FOObar
          //
          bool fs (char_sep (fc));
          bool ms (mod_sep (mc) || mc == '_');

          if (fs && ms)
          {
            fsep = msep = true;
            match = match || mod_sep (mc);
            continue;
          }

          // Only if one is a real separator do we consider case change.
          //
          if (fs || ms)
          {
            bool fa (false), ma (false);
            if ((fs || (fa = case_sep (fp, fc))) &&
                (ms || (ma = case_sep (mp, mc))))
            {
              // Stay on this character if imaginary punctuation (note: cannot
              // be both true).
              //
              if (fa) {++fi; msep = true;}
              if (ma) {++mi; fsep = true;}

              match = match || mod_sep (mc);
              continue;
            }
          }

          break; // No match.
        }

        // Deal with edge cases: complete module match and complete file
        // match.
        //
        match = match || mi == 0 || (fi == 0 && mod_sep (m[mi - 1]));

        if (!match)
          return 0;

        // "Uncount" real separators.
        //
        if (fsep) fi++;
        if (msep) mi++;

        // Use the number of characters matched in the module name and not
        // in the file (this may not be the same because of the imaginary
        // separators).
        //
        size_t ps (mn - mi);

        // The strength of separation sub-score.
        //
        // Check for case change between the last character that matched and
        // the first character that did not.
        //
        size_t as (0);
        if      (fi == 0)                                 as = 9;
        else if (char c = char_sep (f[fi - 1]))           as = c == '/' ? 8 : 7;
        else if (fi != fn && case_sep (f[fi], f[fi - 1])) as = 7;

        // The length of the unmatched part sub-score.
        //
        size_t bs (9999 - fi);

        return ps * 100000 + as * 10000 + bs;
      };

      auto& pts (t.prerequisite_targets[a]);
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
      // recursively. The good news is we are actually in a pretty good shape
      // to handle this: after match all our prerequisite BMIs will have their
      // prerequisite BMIs known, recursively. The only bit that is missing is
      // the re-export flag of some sorts. As well as deciding where to handle
      // it: here or in append_module_options(). After some meditation it
      // became clear handling it here will be simpler: we need to weed out
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
      //    @@ I think now we could use prerequisite_targets::data for this?
      //
      // 2. Once we have matched all the bmi{}s we are importing directly
      //    (with all the re-exported by us at the back), we will go over them
      //    and copy all of their re-exported bmi{}s (using the position we
      //    saved on step #1). The end result will be a recursively-explored
      //    list of imported bmi{}s that append_module_options() can simply
      //    convert to the list of options.
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
      // see the library metadata protocol for details).
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

      auto check_fuzzy = [&trace, &imports, &pts, &match, &match_max, start, n]
        (const target* pt, const string& name)
      {
        for (size_t i (0); i != n; ++i)
        {
          module_import& m (imports[i]);

          if (std_module (m.name)) // No fuzzy std.* matches.
            continue;

          if (m.score > match_max (m.name)) // Resolved to module name.
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
      auto check_exact = [&trace, &imports, &pts, &match_max, start, n, &done]
        (const string& name) -> const target**
      {
        const target** r (nullptr);
        done = true;

        for (size_t i (0); i != n; ++i)
        {
          module_import& m (imports[i]);

          size_t ms (match_max (m.name));

          if (m.score > ms) // Resolved to module name (no effect on done).
            continue;

          if (r == nullptr)
          {
            size_t s (name == m.name ? ms + 1 : 0);

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

      // Find the module in prerequisite targets of a library (recursively)
      // seeing through libu*{}. Note: sets the `done` flag. See similar
      // logic in pkgconfig_save().
      //
      auto find = [a, &bs, this,
                   &check_exact, &done] (const file& l,
                                         const auto& find) -> void
      {
        for (const target* pt: l.prerequisite_targets[a])
        {
          if (pt == nullptr)
            continue;

          // Note that here we (try) to use whatever flavor of bmi*{} is
          // available.
          //
          // @@ MOD: BMI compatibility check.
          //
          if (pt->is_a<bmix> ())
          {
            const string& n (cast<string> (pt->state[a].vars[c_module_name]));

            if (const target** p = check_exact (n))
              *p = pt;
          }
          else if (pt->is_a (*x_mod))
          {
            // This is an installed library with a list of module sources (the
            // source are specified as prerequisites but the fallback file
            // rule puts them into prerequisite_targets for us).
            //
            // The module names should be specified but if not assume
            // something else is going on and ignore.
            //
            // Note also that besides modules, prerequisite_targets may
            // contain libraries which are interface dependencies of this
            // library and which may be called to resolve its module
            // dependencies.
            //
            const string* n (cast_null<string> (pt->vars[c_module_name]));

            if (n == nullptr)
              continue;

            if (const target** p = check_exact (*n))
              *p = &this->make_module_sidebuild (a, bs, l, *pt, *n); // GCC 4.9
          }
          // Note that in prerequisite targets we will have the libux{}
          // members, not the group.
          //
          else if (const libux* pl = pt->is_a<libux> ())
            find (*pl, find);
          else
            continue;

          if (done)
            break;
        }
      };

      for (prerequisite_member p: group_prerequisite_members (a, t))
      {
        if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
          continue;

        const target* pt (p.load ()); // Should be cached for libraries.

        if (pt != nullptr)
        {
          const file* lt (nullptr);

          if (const libx* l = pt->is_a<libx> ())
            lt = link_member (*l, a, li);
          else if (pt->is_a<liba> () || pt->is_a<libs> () || pt->is_a<libux> ())
            lt = &pt->as<file> ();

          // If this is a library, check its bmi{}s and mxx{}s.
          //
          if (lt != nullptr)
          {
            find (*lt, find);

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
        // While we want to use group_prerequisite_members() below, we cannot
        // call resolve_group() since we will be doing it "speculatively" for
        // modules that we may use but also for modules that may use us. This
        // quickly leads to deadlocks. So instead we are going to perform an
        // ad hoc group resolution.
        //
        const target* pg;
        if (p.is_a<bmi> ())
        {
          pg = pt != nullptr ? pt : &p.search (t);
          pt = &search (t, btt, p.key ()); // Same logic as in picking obj*{}.
        }
        else if (p.is_a (btt))
        {
          pg = &search (t, bmi::static_type, p.key ());
          if (pt == nullptr) pt = &p.search (t);
        }
        else
          continue;

        // Find the mxx{} prerequisite and extract its "file name" for the
        // fuzzy match unless the user specified the module name explicitly.
        //
        for (prerequisite_member p:
               prerequisite_members (a, t, group_prerequisites (*pt, pg)))
        {
          if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
            continue;

          if (p.is_a (*x_mod))
          {
            // Check for an explicit module name. Only look for an existing
            // target (which means the name can only be specified on the
            // target itself, not target type/pattern-spec).
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
            fail (relative (src))
              << "unable to resolve module " << imports[i].name <<
              info << "verify module interface is listed as a prerequisite, "
              << "otherwise" <<
              info << "consider adjusting module interface file names or" <<
              info << "consider specifying module name with " << x
              << ".module_name";
          }
        }
      }

      // Match in parallel and wait for completion.
      //
      match_members (a, t, pts, start);

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

        if (m.score <= match_max (in))
        {
          const string& mn (cast<string> (bt->state[a].vars[c_module_name]));

          if (in != mn)
          {
            // Note: matched, so the group should be resolved.
            //
            for (prerequisite_member p: group_prerequisite_members (a, *bt))
            {
              if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
                continue;

              if (p.is_a (*x_mod)) // Got to be there.
              {
                fail (relative (src))
                  << "failed to correctly guess module name from " << p <<
                  info << "guessed: " << in <<
                  info << "actual:  " << mn <<
                  info << "consider adjusting module interface file names or" <<
                  info << "consider specifying module name with " << x
                  << ".module_name";
              }
            }
          }
        }

        // Hash (we know it's a file).
        //
        cs.append (static_cast<const file&> (*bt).path ().string ());

        // Copy over bmi{}s from our prerequisites weeding out duplicates.
        //
        if (size_t j = bt->data<match_data> ().modules.start)
        {
          // Hard to say whether we should reserve or not. We will probably
          // get quite a bit of duplications.
          //
          auto& bpts (bt->prerequisite_targets[a]);
          for (size_t m (bpts.size ()); j != m; ++j)
          {
            const target* et (bpts[j]);

            if (et == nullptr)
              continue; // Unresolved (std.*).

            const string& mn (cast<string> (et->state[a].vars[c_module_name]));

            if (find_if (imports.begin (), imports.end (),
                         [&mn] (const module_import& i)
                         {
                           return i.name == mn;
                         }) == imports.end ())
            {
              pts.push_back (et);
              cs.append (static_cast<const file&> (*et).path ().string ());

              // Add to the list of imports for further duplicate suppression.
              // We could have stored reference to the name (e.g., in score)
              // but it's probably not worth it if we have a small string
              // optimization.
              //
              import_type t (mn.find (':') != string::npos
                             ? import_type::module_part
                             : import_type::module_intf);
              imports.push_back (module_import {t, mn, true, 0});
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

    // Find or create a modules sidebuild subproject returning its root
    // directory.
    //
    pair<dir_path, const scope&> compile_rule::
    find_modules_sidebuild (const scope& rs) const
    {
      context& ctx (rs.ctx);

      // First figure out where we are going to build. We want to avoid
      // multiple sidebuilds so the outermost scope that has loaded the
      // cc.config module and that is within our amalgmantion seems like a
      // good place.
      //
      const scope* as (&rs);
      {
        const scope* ws (as->weak_scope ());
        if (as != ws)
        {
          const scope* s (as);
          do
          {
            s = s->parent_scope ()->root_scope ();

            // Use cc.core.vars as a proxy for {c,cxx}.config (a bit smelly).
            //
            // This is also the module that registers the scope operation
            // callback that cleans up the subproject.
            //
            if (cast_false<bool> ((*s)["cc.core.vars.loaded"]))
              as = s;

          } while (s != ws);
        }
      }

      // We build modules in a subproject (since there might be no full
      // language support loaded in the amalgamation, only *.config). So the
      // first step is to check if the project has already been created and/or
      // loaded and if not, then to go ahead and do so.
      //
      dir_path pd (as->out_path () /
                   as->root_extra->build_dir /
                   module_build_modules_dir /=
                   x);

      const scope* ps (&ctx.scopes.find_out (pd));

      if (ps->out_path () != pd)
      {
        // Switch the phase to load then create and load the subproject.
        //
        phase_switch phs (ctx, run_phase::load);

        // Re-test again now that we are in exclusive phase (another thread
        // could have already created and loaded the subproject).
        //
        ps = &ctx.scopes.find_out (pd);

        if (ps->out_path () != pd)
        {
          // The project might already be created in which case we just need
          // to load it.
          //
          optional<bool> altn (false); // Standard naming scheme.
          if (!is_src_root (pd, altn))
          {
            // Copy our standard and force modules.
            //
            string extra;

            if (const string* std = cast_null<string> (rs[x_std]))
              extra += string (x) + ".std = " + *std + '\n';

            extra += string (x) + ".features.modules = true";

            create_project (
              pd,
              as->out_path ().relative (pd),  /* amalgamation */
              {},                             /* boot_modules */
              extra,                          /* root_pre */
              {string (x) + '.'},             /* root_modules */
              "",                             /* root_post */
              nullopt,                        /* config_module */
              nullopt,                        /* config_file */
              false,                          /* buildfile */
              "the cc module",
              2);                             /* verbosity */
          }

          ps = &load_project (ctx, pd, pd, false /* forwarded */);
        }
      }

      // Some sanity checks.
      //
#ifndef NDEBUG
      assert (ps->root ());
      const module* m (ps->find_module<module> (x));
      assert (m != nullptr && m->modules);
#endif

      return pair<dir_path, const scope&> (move (pd), *as);
    }

    // Synthesize a dependency for building a module binary interface on
    // the side.
    //
    const file& compile_rule::
    make_module_sidebuild (action a,
                           const scope& bs,
                           const file& lt,
                           const target& mt,
                           const string& mn) const
    {
      tracer trace (x, "compile_rule::make_module_sidebuild");

      // Note: see also make_header_sidebuild() below.

      dir_path pd (find_modules_sidebuild (*bs.root_scope ()).first);

      // We need to come up with a file/target name that will be unique enough
      // not to conflict with other modules. If we assume that within an
      // amalgamation there is only one "version" of each module, then the
      // module name itself seems like a good fit. We just replace '.' with
      // '-' and ':' with '+'.
      //
      string mf;
      transform (mn.begin (), mn.end (),
                 back_inserter (mf),
                 [] (char c) {return c == '.' ? '-' : c == ':' ? '+' : c;});

      // It seems natural to build a BMI type that corresponds to the library
      // type. After all, this is where the object file part of the BMI is
      // going to come from (unless it's a module interface-only library).
      //
      const target_type& tt (compile_types (link_type (lt).type).bmi);

      // Store the BMI target in the subproject root. If the target already
      // exists then we assume all this is already done (otherwise why would
      // someone have created such a target).
      //
      if (const file* bt = bs.ctx.targets.find<file> (
            tt,
            pd,
            dir_path (), // Always in the out tree.
            mf,
            nullopt,     // Use default extension.
            trace))
        return *bt;

      prerequisites ps;
      ps.push_back (prerequisite (mt));

      // We've added the mxx{} but it may import other modules from this
      // library. Or from (direct) dependencies of this library. We add them
      // all as prerequisites so that the standard module search logic can
      // sort things out. This is pretty similar to what we do in link when
      // synthesizing dependencies for bmi{}'s.
      //
      // Note: lt is matched and so the group is resolved.
      //
      ps.push_back (prerequisite (lt));
      for (prerequisite_member p: group_prerequisite_members (a, lt))
      {
        if (include (a, lt, p) != include_type::normal) // Excluded/ad hoc.
          continue;

        if (p.is_a<libx> () ||
            p.is_a<liba> () || p.is_a<libs> () || p.is_a<libux> ())
        {
          ps.push_back (p.as_prerequisite ());
        }
      }

      auto p (bs.ctx.targets.insert_locked (
                tt,
                move (pd),
                dir_path (), // Always in the out tree.
                move (mf),
                nullopt,     // Use default extension.
                target_decl::implied,
                trace));
      file& bt (static_cast<file&> (p.first));

      // Note that this is racy and someone might have created this target
      // while we were preparing the prerequisite list.
      //
      if (p.second)
      {
        bt.prerequisites (move (ps));

        // Unless this is a binless library, we don't need the object file
        // (see config_data::b_binless for details).
        //
        bt.vars.assign (b_binless) = (lt.mtime () == timestamp_unreal);
      }

      return bt;
    }

    // Synthesize a dependency for building a header unit binary interface on
    // the side.
    //
    const file& compile_rule::
    make_header_sidebuild (action a,
                           const scope& bs,
                           const file& t,
                           linfo li,
                           const file& ht) const
    {
      tracer trace (x, "compile_rule::make_header_sidebuild");

      // Note: similar to make_module_sidebuild() above.

      auto sb (find_modules_sidebuild (*bs.root_scope ()));
      dir_path pd (move (sb.first));
      const scope& as (sb.second);

      // Determine if this header belongs to one of the libraries we depend
      // on.
      //
      // Note that because libraries are not in prerequisite_targets, we have
      // to go through prerequisites, similar to append_library_options().
      //
      const target* lt (nullptr); // Can be lib{}.
      {
        // Note that any such library would necessarily be an interface
        // dependency so we never need to go into implementations.
        //
        auto imp = [] (const target&, bool) { return false; };

        // The same logic as in append_libraries().
        //
        struct data
        {
          action         a;
          const file&    ht;
          const target*& lt;
        } d {a, ht, lt};

        auto lib = [&d] (
          const target* const* lc,
          const small_vector<reference_wrapper<const string>, 2>&,
          lflags,
          bool)
        {
          // It's unfortunate we have no way to bail out.
          //
          if (d.lt != nullptr)
            return;

          const target* l (lc != nullptr ? *lc : nullptr); // Can be lib{}.

          if (l == nullptr)
            return;

          // Feels like we should only consider non-utility libraries with
          // utilities being treated as "direct" use.
          //
          if (l->is_a<libux> ())
            return;

          // Since the library is searched and matched, all the headers should
          // be in prerequisite_targets.
          //
          const auto& pts (l->prerequisite_targets[d.a]);
          if (find (pts.begin (), pts.end (), &d.ht) != pts.end ())
            d.lt = l;
        };

        for (prerequisite_member p: group_prerequisite_members (a, t))
        {
          if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
            continue;

          // Should be already searched and matched for libraries.
          //
          if (const target* pt = p.load ())
          {
            if (const libx* l = pt->is_a<libx> ())
              pt = link_member (*l, a, li);

            bool la;
            const file* f;
            if ((la = (f = pt->is_a<liba> ()))  ||
                (la = (f = pt->is_a<libux> ())) ||
                (     (f = pt->is_a<libs> ())))
            {
              // Note that we are requesting process_libraries() to not pick
              // the liba/libs{} member of the installed libraries and return
              // the lib{} group itself instead. This is because, for the
              // installed case, the library prerequisites (both headers and
              // interface dependency libraries) are matched by file_rule
              // which won't pick the liba/libs{} member (naturally) but will
              // just match the lib{} group.
              //
              process_libraries (
                a, bs, nullopt, sys_lib_dirs,
                *f, la, 0, // lflags unused.
                imp, lib, nullptr, true /* self */);
            }
          }
        }
      }

      // What should we use as a file/target name? On one hand we want it
      // unique enough so that <stdio.h> and <custom/stdio.h> don't end up
      // with the same BMI. On the other, we need the same headers resolving
      // to the same target, regardless of how they were imported. So it feels
      // like the name should be the absolute and normalized (actualized on
      // case-insensitive filesystems) header path. We could try to come up
      // with something by sanitizing certain characters, etc. But then the
      // names will be very long and ugly, they will run into path length
      // limits, etc. So instead we will use the file name plus an abbreviated
      // hash of the whole path, something like stdio-211321fe6de7.
      //
      string mf;
      {
        // @@ MODHDR: Can we assume the path is actualized since the header
        //            target came from enter_header()? No, not anymore: it
        //            is now normally just normalized.
        //
        const path& hp (ht.path ());
        mf = hp.leaf ().make_base ().string ();
        mf += '-';
        mf += sha256 (hp.string ()).abbreviated_string (12);
      }

      // If the header comes from the library, use its hbmi?{} type to
      // maximize reuse.
      //
      const target_type& tt (
        compile_types (
          lt != nullptr && !lt->is_a<lib> ()
          ? link_type (*lt).type
          : li.type).hbmi);

      if (const file* bt = bs.ctx.targets.find<file> (
            tt,
            pd,
            dir_path (), // Always in the out tree.
            mf,
            nullopt,     // Use default extension.
            trace))
        return *bt;

      prerequisites ps;
      ps.push_back (prerequisite (ht));

      // Similar story as for modules: the header may need poptions from its
      // library (e.g., -I to find other headers that it includes).
      //
      if (lt != nullptr)
        ps.push_back (prerequisite (*lt));
      else
      {
        // If the header does not belong to a library then this is a "direct"
        // use, for example, by an exe{} target. In this case we need to add
        // all the prerequisite libraries as well as scope p/coptions (in a
        // sense, we are trying to approximate how all the sources that would
        // typically include such a header are build).
        //
        // Note that this is also the case when we build the library's own
        // sources (in a way it would have been cleaner to always build
        // library's headers with only its "interface" options/prerequisites
        // but that won't be easy to achieve).
        //
        // Note also that at first it might seem like a good idea to
        // incorporate this information into the hash we use to form the BMI
        // name. But that would reduce sharing of the BMI. For example, that
        // would mean we will build the library header twice, once with the
        // implementation options/prerequisites and once -- with interface.
        // On the other hand, importable headers are expected to be "modular"
        // and should probably not depend on any of the implementation
        // options/prerequisites (though one could conceivably build a
        // "richer" BMI if it is also to be used to build the library
        // implementation -- interesting idea).
        //
        for (prerequisite_member p: group_prerequisite_members (a, t))
        {
          if (include (a, t, p) != include_type::normal) // Excluded/ad hoc.
            continue;

          if (p.is_a<libx> () ||
              p.is_a<liba> () || p.is_a<libs> () || p.is_a<libux> ())
          {
            ps.push_back (p.as_prerequisite ());
          }
        }
      }

      auto p (bs.ctx.targets.insert_locked (
                tt,
                move (pd),
                dir_path (), // Always in the out tree.
                move (mf),
                nullopt,     // Use default extension.
                target_decl::implied,
                trace));
      file& bt (static_cast<file&> (p.first));

      // Note that this is racy and someone might have created this target
      // while we were preparing the prerequisite list.
      //
      if (p.second)
      {
        bt.prerequisites (move (ps));

        // Add the p/coptions from our scope in case of a "direct" use. Take
        // into account hbmi{} target-type/pattern values to allow specifying
        // hbmi-specific options.
        //
        if (lt == nullptr)
        {
          auto set = [&bs, &as, &tt, &bt] (const variable& var)
          {
            // Avoid duplicating the options if they are from the same
            // amalgamation as the sidebuild.
            //
            lookup l (bs.lookup (var, tt, bt.name, hbmi::static_type, bt.name));
            if (l.defined () && !l.belongs (as))
              bt.assign (var) = *l;
          };

          set (c_poptions);
          set (x_poptions);
          set (c_coptions);
          set (x_coptions);
        }
      }

      return bt;
    }

    // Filter cl.exe noise (msvc.cxx).
    //
    void
    msvc_filter_cl (ifdstream&, const path& src);

    // Append header unit-related options.
    //
    // Note that this function is called for both full preprocessing and
    // compilation proper and in the latter case it is followed by a call
    // to append_module_options().
    //
    void compile_rule::
    append_header_options (environment&,
                           cstrings& args,
                           small_vector<string, 2>& stor,
                           action,
                           const file&,
                           const match_data& md,
                           const path& dd) const
    {
      switch (ctype)
      {
      case compiler_type::gcc:
        {
          if (md.header_units != 0)
          {
            string s (relative (dd).string ());
            s.insert (0, "-fmodule-mapper=");
            s += "?@"; // Significant line prefix.
            stor.push_back (move (s));
          }

          break;
        }
      case compiler_type::clang:
      case compiler_type::msvc:
      case compiler_type::icc:
        break;
      }

      // Shallow-copy storage to args. Why not do it as we go along pushing
      // into storage? Because of potential reallocations.
      //
      for (const string& a: stor)
        args.push_back (a.c_str ());
    }

    // Append module-related options.
    //
    // Note that this function is only called for the compilation proper and
    // after a call to append_header_options() (so watch out for duplicate
    // options).
    //
    void compile_rule::
    append_module_options (environment& env,
                           cstrings& args,
                           small_vector<string, 2>& stor,
                           action a,
                           const file& t,
                           const match_data& md,
                           const path& dd) const
    {
      unit_type ut (md.type);
      const module_positions& ms (md.modules);

      dir_path stdifc; // See the VC case below.

      switch (ctype)
      {
      case compiler_type::gcc:
        {
          // Use the module map stored in depdb.
          //
          // Note that it is also used to specify the output BMI file.
          //
          if (md.header_units == 0 && // In append_header_options()?
              (ms.start != 0                     ||
               ut == unit_type::module_intf      ||
               ut == unit_type::module_intf_part ||
               ut == unit_type::module_impl_part ||
               ut == unit_type::module_header))
          {
            string s (relative (dd).string ());
            s.insert (0, "-fmodule-mapper=");
            s += "?@"; // Cookie (aka line prefix).
            stor.push_back (move (s));
          }

          break;
        }
      case compiler_type::clang:
        {
          if (ms.start == 0)
            return;

          // Clang embeds module file references so we only need to specify
          // our direct imports.
          //
          // If/when we get the ability to specify the mapping in a file, we
          // will pass the whole list.
          //
#if 0
          // In Clang the module implementation's unit .pcm is special and
          // must be "loaded".
          //
          if (ut == unit_type::module_impl)
          {
            const file& f (pts[ms.start]->as<file> ());
            string s (relative (f.path ()).string ());
            s.insert (0, "-fmodule-file=");
            stor.push_back (move (s));
          }

          // Use the module map stored in depdb for others.
          //
          string s (relative (dd).string ());
          s.insert (0, "-fmodule-file-map=@=");
          stor.push_back (move (s));
#else
          auto& pts (t.prerequisite_targets[a]);
          for (size_t i (ms.start),
                 n (ms.copied != 0 ? ms.copied : pts.size ());
               i != n;
               ++i)
          {
            const target* pt (pts[i]);

            if (pt == nullptr)
              continue;

            // Here we use whatever bmi type has been added. And we know all
            // of these are bmi's.
            //
            const file& f (pt->as<file> ());
            string s (relative (f.path ()).string ());

            // In Clang the module implementation's unit .pcm is special and
            // must be "loaded".
            //
            if (ut == unit_type::module_impl && i == ms.start)
              s.insert (0, "-fmodule-file=");
            else
            {
              s.insert (0, 1, '=');
              s.insert (0, cast<string> (f.state[a].vars[c_module_name]));
              s.insert (0, "-fmodule-file=");
            }

            stor.push_back (move (s));
          }
#endif
          break;
        }
      case compiler_type::msvc:
        {
          if (ms.start == 0)
            return;

          auto& pts (t.prerequisite_targets[a]);
          for (size_t i (ms.start), n (pts.size ());
               i != n;
               ++i)
          {
            const target* pt (pts[i]);

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
            if (std_module (cast<string> (f.state[a].vars[c_module_name])))
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
      case compiler_type::icc:
        break;
      }

      // Shallow-copy storage to args. Why not do it as we go along pushing
      // into storage? Because of potential reallocations.
      //
      for (const string& a: stor)
        args.push_back (a.c_str ());

      if (getenv ("IFCPATH"))
      {
        // VC's IFCPATH takes precedence over /module:stdIfcDir so unset it if
        // we are using our own std modules. Note: IFCPATH saved in guess.cxx.
        //
        if (!stdifc.empty ())
          env.push_back ("IFCPATH");
      }
      else if (stdifc.empty ())
      {
        // Add the VC's default directory (should be only one).
        //
        if (sys_mod_dirs != nullptr && !sys_mod_dirs->empty ())
        {
          args.push_back ("/module:stdIfcDir");
          args.push_back (sys_mod_dirs->front ().string ().c_str ());
        }
      }
    }

    target_state compile_rule::
    perform_update (action a, const target& xt) const
    {
      const file& t (xt.as<file> ());
      const path& tp (t.path ());

      match_data md (move (t.data<match_data> ()));
      unit_type ut (md.type);

      context& ctx (t.ctx);

      // While all our prerequisites are already up-to-date, we still have to
      // execute them to keep the dependency counts straight. Actually, no, we
      // may also have to update the modules.
      //
      // Note that this also takes care of forcing update on any ad hoc
      // prerequisite change.
      //
      auto pr (
        execute_prerequisites<file> (
          md.src.type (),
          a, t,
          md.mt,
          [s = md.modules.start] (const target&, size_t i)
          {
            return s != 0 && i >= s; // Only compare timestamps for modules.
          },
          md.modules.copied)); // See search_modules() for details.

      const file& s (pr.second);
      const path* sp (&s.path ());

      // Force recompilation in case of a deferred failure even if nothing
      // changed.
      //
      if (pr.first && !md.deferred_failure)
      {
        if (md.touch)
        {
          touch (ctx, tp, false, 2);
          t.mtime (system_clock::now ());
          ctx.skip_count.fetch_add (1, memory_order_relaxed);
        }
        // Note: else mtime should be cached.

        return *pr.first;
      }

      // Make sure depdb is no older than any of our prerequisites (see md.mt
      // logic description above for details). Also save the sequence start
      // time if doing mtime checks (see the depdb::check_mtime() call below).
      //
      timestamp start (depdb::mtime_check ()
                       ? system_clock::now ()
                       : timestamp_unknown);

      touch (ctx, md.dd, false, verb_never);

      const scope& bs (t.base_scope ());

      otype ot (compile_type (t, ut));
      linfo li (link_info (bs, ot));
      compile_target_types tts (compile_types (ot));

      environment env;
      cstrings args {cpath.recall_string ()};

      // If we are building a module interface or partition, then the target
      // is bmi*{} and it may have an ad hoc obj*{} member. For header units
      // there is no obj*{} (see the corresponding add_adhoc_member() call in
      // apply()).
      //
      path relm;
      path relo;
      switch (ut)
      {
      case unit_type::module_header:
        break;
      case unit_type::module_intf:
      case unit_type::module_intf_part:
      case unit_type::module_impl_part:
        {
          if (const file* o = find_adhoc_member<file> (t, tts.obj))
            relo = relative (o->path ());

          break;
        }
      default:
        relo = relative (tp);
      }

      // Build the command line.
      //
      if (md.pp != preprocessed::all)
      {
        // Note that these come in the reverse order of coptions since the
        // header search paths are examined in the order specified (in
        // contrast to the "last value wins" semantics that we assume for
        // coptions).
        //
        append_options (args, t, x_poptions);
        append_options (args, t, c_poptions);

        // Add *.export.poptions from prerequisite libraries.
        //
        append_library_options (args, bs, a, t, li);

        if (md.symexport)
          append_symexport_options (args, t);
      }

      append_options (args, t, c_coptions);
      append_options (args, t, x_coptions);

      string out, out1;                    // Output options storage.
      small_vector<string, 2> header_args; // Header unit options storage.
      small_vector<string, 2> module_args; // Module options storage.

      size_t out_i (0);  // Index of the -o option.
      size_t lang_n (0); // Number of lang options.

      switch (cclass)
      {
      case compiler_class::msvc:
        {
          // The /F*: option variants with separate names only became
          // available in VS2013/12.0. Why do we bother? Because the command
          // line suddenly becomes readable.
          //
          // Also, clang-cl does not yet support them, at least not in 8 or 9.
          //
          bool fc (cmaj >= 18 && cvariant != "clang");

          args.push_back ("/nologo");

          append_options (args, cmode);

          if (md.pp != preprocessed::all)
            append_sys_hdr_options (args); // Extra system header dirs (last).

          // While we want to keep the low-level build as "pure" as possible,
          // the two misguided defaults, C++ exceptions and runtime, just have
          // to be fixed. Otherwise the default build is pretty much unusable.
          // But we also make sure that the user can easily disable our
          // defaults: if we see any relevant options explicitly specified, we
          // take our hands off.
          //
          // For C looks like no /EH* (exceptions supported but no C++ objects
          // destroyed) is a reasonable default.
          //
          if (x_lang == lang::cxx && !find_option_prefix ("/EH", args))
            args.push_back ("/EHsc");

          // The runtime is a bit more interesting. At first it may seem like
          // a good idea to be a bit clever and use the static runtime if we
          // are building obja{}. And for obje{} we could decide which runtime
          // to use based on the library link order: if it is static-only,
          // then we could assume the static runtime. But it is indeed too
          // clever: when building liba{} we have no idea who is going to use
          // it. It could be an exe{} that links both static and shared
          // libraries (and is therefore built with the shared runtime). And
          // to safely use the static runtime, everything must be built with
          // /MT and there should be no DLLs in the picture. So we are going
          // to play it safe and always default to the shared runtime.
          //
          // In a similar vein, it would seem reasonable to use the debug
          // runtime if we are compiling with debug. But, again, there will be
          // fireworks if we have some projects built with debug and some
          // without and then we try to link them together (which is not an
          // unreasonable thing to do). So by default we will always use the
          // release runtime.
          //
          if (!find_option_prefixes ({"/MD", "/MT"}, args))
            args.push_back ("/MD");

          msvc_sanitize_cl (args);

          append_header_options (env, args, header_args, a, t, md, md.dd);
          append_module_options (env, args, module_args, a, t, md, md.dd);

          // The presence of /Zi or /ZI causes the compiler to write debug
          // info to the .pdb file. By default it is a shared file called
          // vcNN.pdb (where NN is the VC version) created (wait for it) in
          // the current working directory (and not the directory of the .obj
          // file). Also, because it is shared, there is a special Windows
          // service that serializes access. We, of course, want none of that
          // so we will create a .pdb per object file.
          //
          // Note that this also changes the name of the .idb file (used for
          // minimal rebuild and incremental compilation): cl.exe take the /Fd
          // value and replaces the .pdb extension with .idb.
          //
          // Note also that what we are doing here appears to be incompatible
          // with PCH (/Y* options) and /Gm (minimal rebuild).
          //
          // @@ MOD: TODO deal with absent relo.
          //
          if (find_options ({"/Zi", "/ZI"}, args))
          {
            if (fc)
              args.push_back ("/Fd:");
            else
              out1 = "/Fd";

            out1 += relo.string ();
            out1 += ".pdb";

            args.push_back (out1.c_str ());
          }

          if (fc)
          {
            args.push_back ("/Fo:");
            args.push_back (relo.string ().c_str ());
          }
          else
          {
            out = "/Fo" + relo.string ();
            args.push_back (out.c_str ());
          }

          // @@ MODHDR MSVC
          // @@ MODPART MSVC
          //
          if (ut == unit_type::module_intf)
          {
            relm = relative (tp);

            args.push_back ("/module:interface");
            args.push_back ("/module:output");
            args.push_back (relm.string ().c_str ());
          }

          // Note: no way to indicate that the source if already preprocessed.

          args.push_back ("/c");                   // Compile only.
          append_lang_options (args, md);          // Compile as.
          args.push_back (sp->string ().c_str ()); // Note: relied on being last.

          break;
        }
      case compiler_class::gcc:
        {
          if (ot == otype::s)
          {
            // On Darwin, Win32 -fPIC is the default.
            //
            if (tclass == "linux" || tclass == "bsd")
              args.push_back ("-fPIC");
          }

          if (tsys == "win32-msvc")
          {
            switch (ctype)
            {
            case compiler_type::clang:
              {
                // Default to the /EHsc exceptions support for C++, similar to
                // the the MSVC case above.
                //
                // Note that both vanilla clang++ and clang-cl drivers add
                // -fexceptions and -fcxx-exceptions by default. However,
                // clang-cl also adds -fexternc-nounwind, which implements the
                // 'c' part in /EHsc. Note that adding this option is not a
                // mere optimization, as we have discovered through some
                // painful experience; see Clang bug #45021.
                //
                // Let's also omit this option if -f[no]-exceptions is
                // specified explicitly.
                //
                if (x_lang == lang::cxx)
                {
                  if (!find_options ({"-fexceptions", "-fno-exceptions"}, args))
                  {
                    args.push_back ("-Xclang");
                    args.push_back ("-fexternc-nounwind");
                  }
                }

                // Default to the multi-threaded DLL runtime (/MD), similar to
                // the MSVC case above.
                //
                // Clang's MSVC.cpp will not link the default runtime if
                // either -nostdlib or -nostartfiles is specified. Let's do
                // the same.
                //
                initializer_list<const char*> os {"-nostdlib", "-nostartfiles"};
                if (!find_options (os, cmode) && !find_options (os, args))
                {
                  args.push_back ("-D_MT");
                  args.push_back ("-D_DLL");

                  // All these -Xclang --dependent-lib=... add quite a bit of
                  // noise to the command line. The alternative is to use the
                  // /DEFAULTLIB option during linking. The drawback of that
                  // approach is that now we can theoretically build the
                  // object file for one runtime but try to link it with
                  // something else.
                  //
                  // For example, an installed static library was built for a
                  // non-debug runtime while a project that links it uses
                  // debug. With the --dependent-lib approach we will try to
                  // link multiple runtimes while with /DEFAULTLIB we may end
                  // up with unresolved symbols (but things might also work
                  // out fine, unless the runtimes have incompatible ABIs).
                  //
                  // Let's start with /DEFAULTLIB and see how it goes (see the
                  // link rule).
                  //
#if 0
                  args.push_back ("-Xclang");
                  args.push_back ("--dependent-lib=msvcrt");

                  // This provides POSIX compatibility (map open() to _open(),
                  // etc).
                  //
                  args.push_back ("-Xclang");
                  args.push_back ("--dependent-lib=oldnames");
#endif
                }

                break;
              }
            case compiler_type::gcc:
            case compiler_type::msvc:
            case compiler_type::icc:
              assert (false);
            }
          }

          // For now Emscripten defaults to partial C++ exceptions support
          // (you can throw but not catch). We enable full support unless it
          // was explicitly disabled by the user.
          //
          if (ctype == compiler_type::clang && cvariant == "emscripten")
          {
            if (x_lang == lang::cxx)
            {
              if (!find_option_prefix ("DISABLE_EXCEPTION_CATCHING=", args))
              {
                args.push_back ("-s");
                args.push_back ("DISABLE_EXCEPTION_CATCHING=0");
              }
            }
          }

          append_options (args, cmode);

          if (md.pp != preprocessed::all)
            append_sys_hdr_options (args); // Extra system header dirs (last).

          append_header_options (env, args, header_args, a, t, md, md.dd);
          append_module_options (env, args, module_args, a, t, md, md.dd);

          // Note: the order of the following options is relied upon below.
          //
          out_i = args.size (); // Index of the -o option.

          if (ut == unit_type::module_intf      ||
              ut == unit_type::module_intf_part ||
              ut == unit_type::module_impl_part ||
              ut == unit_type::module_header)
          {
            switch (ctype)
            {
            case compiler_type::gcc:
              {
                // Output module file is specified in the mapping file, the
                // same as input.
                //
                if (ut == unit_type::module_header) // No obj, -c implied.
                  break;

                if (!relo.empty ())
                {
                  args.push_back ("-o");
                  args.push_back (relo.string ().c_str ());
                }
                else if (ut != unit_type::module_header)
                {
                  // Should this be specified in append_lang_options() like
                  // -fmodule-header (which, BTW, implies -fmodule-only)?
                  // While it's plausible that -fmodule-header has some
                  // semantic differences that should be in effect during
                  // preprocessing, -fmodule-only seems to only mean "don't
                  // write the object file" so for now we specify it only
                  // here.
                  //
                  args.push_back ("-fmodule-only");
                }

                args.push_back ("-c");
                break;
              }
            case compiler_type::clang:
              {
                // @@ MOD TODO: deal with absent relo.

                relm = relative (tp);

                args.push_back ("-o");
                args.push_back (relm.string ().c_str ());
                args.push_back ("--precompile");

                // Without this option Clang's .pcm will reference source
                // files.  In our case this file may be transient (.ii). Plus,
                // it won't play nice with distributed compilation.
                //
                args.push_back ("-Xclang");
                args.push_back ("-fmodules-embed-all-files");

                break;
              }
            case compiler_type::msvc:
            case compiler_type::icc:
              assert (false);
            }
          }
          else
          {
            args.push_back ("-o");
            args.push_back (relo.string ().c_str ());
            args.push_back ("-c");
          }

          lang_n = append_lang_options (args, md);

          if (md.pp == preprocessed::all)
          {
            // Note that the mode we select must still handle comments and
            // line continuations. So some more compiler-specific voodoo.
            //
            switch (ctype)
            {
            case compiler_type::gcc:
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
            case compiler_type::clang:
              {
                // Clang handles comments and line continuations in the
                // preprocessed source (it does not have -fpreprocessed).
                //
                break;
              }
            case compiler_type::icc:
              break; // Compile as normal source for now.
            case compiler_type::msvc:
              assert (false);
            }
          }

          args.push_back (sp->string ().c_str ());

          break;
        }
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
      bool psrc (md.psrc);
      bool ptmp (psrc && md.psrc.temporary);
      if (psrc)
      {
        args.pop_back (); // nullptr
        args.pop_back (); // sp

        sp = &md.psrc.path ();

        // This should match with how we setup preprocessing.
        //
        switch (ctype)
        {
        case compiler_type::gcc:
          {
            // The -fpreprocessed is implied by .i/.ii. But not when compiling
            // a header unit (there is no .hi/.hii).
            //
            if (ut == unit_type::module_header)
              args.push_back ("-fpreprocessed");
            else
              // Pop -x since it takes precedence over the extension.
              //
              // @@ I wonder why bother and not just add -fpreprocessed? Are
              //    we trying to save an option or does something break?
              //
              for (; lang_n != 0; --lang_n)
                args.pop_back ();

            args.push_back ("-fdirectives-only");
            break;
          }
        case compiler_type::clang:
          {
            // Note that without -x Clang will treat .i/.ii as fully
            // preprocessed.
            //
            break;
          }
        case compiler_type::msvc:
          {
            // Nothing to do (/TP or /TC already there).
            //
            break;
          }
        case compiler_type::icc:
          assert (false);
        }

        args.push_back (sp->string ().c_str ());
        args.push_back (nullptr);

        // Let's keep the preprocessed file in case of an error but only at
        // verbosity level 3 and up (when one actually sees it mentioned on
        // the command line). We also have to re-enable on success (see
        // below).
        //
        if (ptmp && verb >= 3)
          md.psrc.temporary = false;
      }

      if (verb >= 3)
        print_process (args);

      // @@ DRYRUN: Currently we discard the (partially) preprocessed file on
      // dry-run which is a waste. Even if we keep the file around (like we do
      // for the error case; see above), we currently have no support for
      // re-using the previously preprocessed output. However, everything
      // points towards us needing this in the near future since with modules
      // we may be out of date but not needing to re-preprocess the
      // translation unit (i.e., one of the imported module's BMIs has
      // changed).
      //
      if (!ctx.dry_run)
      {
        try
        {
          // If we are compiling the preprocessed output, get its read handle.
          //
          file_cache::read psrcr (psrc ? md.psrc.open () : file_cache::read ());

          // VC cl.exe sends diagnostics to stdout. It also prints the file
          // name being compiled as the first line. So for cl.exe we redirect
          // stdout to a pipe, filter that noise out, and send the rest to
          // stderr.
          //
          // For other compilers redirect stdout to stderr, in case any of
          // them tries to pull off something similar. For sane compilers this
          // should be harmless.
          //
          bool filter (ctype == compiler_type::msvc);

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

              // If anything remains in the stream, send it all to stderr.
              // Note that the eof check is important: if the stream is at
              // eof, this and all subsequent writes to the diagnostics stream
              // will fail (and you won't see a thing).
              //
              if (is.peek () != ifdstream::traits_type::eof ())
                diag_stream_lock () << is.rdbuf ();

              is.close ();
            }
            catch (const io_error&) {} // Assume exits with error.
          }

          run_finish (args, pr);
        }
        catch (const process_error& e)
        {
          error << "unable to execute " << args[0] << ": " << e;

          if (e.child)
            exit (1);

          throw failed ();
        }

        if (md.deferred_failure)
          fail << "expected error exit status from " << x_lang << " compiler";
      }

      // Remove preprocessed file (see above).
      //
      if (ptmp && verb >= 3)
        md.psrc.temporary = true;

      // Clang's module compilation requires two separate compiler
      // invocations.
      //
      // @@ MODPART: Clang (all of this is probably outdated).
      //
      if (ctype == compiler_type::clang && ut == unit_type::module_intf)
      {
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

        if (!ctx.dry_run)
        {
          // Remove the target file if this fails. If we don't do that, we
          // will end up with a broken build that is up-to-date.
          //
          auto_rmfile rm (relm);

          try
          {
            process pr (cpath,
                        args.data (),
                        0, 2, 2,
                        nullptr, // CWD
                        env.empty () ? nullptr : env.data ());

            run_finish (args, pr);
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
      }

      timestamp now (system_clock::now ());

      if (!ctx.dry_run)
        depdb::check_mtime (start, md.dd, tp, now);

      // Should we go to the filesystem and get the new mtime? We know the
      // file has been modified, so instead just use the current clock time.
      // It has the advantage of having the subseconds precision. Plus, in
      // case of dry-run, the file won't be modified.
      //
      t.mtime (now);
      return target_state::changed;
    }

    target_state compile_rule::
    perform_clean (action a, const target& xt) const
    {
      const file& t (xt.as<file> ());

      // Compressed preprocessed file extension.
      //
      auto cpext = [this, &t, s = string ()] () mutable -> const char*
      {
        return (s = t.ctx.fcache.compressed_extension (x_pext)).c_str ();
      };

      clean_extras extras;

      switch (ctype)
      {
      case compiler_type::gcc:   extras = {".d", x_pext, cpext (), ".t"};          break;
      case compiler_type::clang: extras = {".d", x_pext, cpext ()};                break;
      case compiler_type::msvc:  extras = {".d", x_pext, cpext (), ".idb", ".pdb"};break;
      case compiler_type::icc:   extras = {".d"};                                  break;
      }

      return perform_clean_extra (a, t, extras);
    }
  }
}
