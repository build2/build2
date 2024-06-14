// file      : libbuild2/cc/compile-rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/compile-rule.hxx>

#include <cerrno>
#include <cstdlib>  // exit()
#include <cstring>  // strlen(), strchr(), strncmp()

#include <libbutl/path-pattern.hxx>

#include <libbuild2/file.hxx>
#include <libbuild2/depdb.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>  // mtime()
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/make-parser.hxx>

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
      throw invalid_argument ("invalid preprocessed value '" + s + '\'');
    }

    // Return true if the compiler supports -isystem (GCC class) or
    // /external:I (MSVC class).
    //
    static inline bool
    isystem (const data& d)
    {
      switch (d.cclass)
      {
      case compiler_class::gcc:
        {
          return true;
        }
      case compiler_class::msvc:
        {
          if (d.cvariant.empty ())
          {
            // While /external:I is available since 15.6, it required
            // /experimental:external (and was rather buggy) until 16.10.
            //
            return d.cmaj > 19 || (d.cmaj == 19 && d.cmin >= 29);
          }
          else if (d.cvariant != "clang")
          {
            // clang-cl added support for /external:I (by translating it to
            // -isystem) in version 13.
            //
            return d.cvmaj >= 13;
          }
          else
            return false;
        }
      }

      return false;
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

    // Note that we don't really need this for clean (where we only need
    // unrefined unit type) so we could make this update-only. But let's keep
    // it simple for now. Note that now we do need the source prerequisite
    // type in clean to deal with Objective-X.
    //
    struct compile_rule::match_data
    {
      match_data (const compile_rule& r,
                  unit_type t,
                  const prerequisite_member& s)
          : type (t), src (s), rule (r) {}

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

      const compile_rule& rule;

      target_state
      operator() (action a, const target& t)
      {
        return rule.perform_update (a, t, *this);
      }
    };

    compile_rule::
    compile_rule (data&& d, const scope& rs)
        : common (move (d)),
          rule_id (string (x) += ".compile 6")
    {
      // Locate the header cache (see enter_header() for details).
      //
      {
        string mn (string (x) + ".config");

        header_cache_ = rs.find_module<config_module> (mn); // Must be there.

        const scope* ws (rs.weak_scope ());
        if (ws != &rs)
        {
          const scope* s (&rs);
          do
          {
            s = s->parent_scope ()->root_scope ();

            if (const auto* m = s->find_module<config_module> (mn))
              header_cache_ = m;

          } while (s != ws);
        }
      }
    }

    template <typename T>
    void compile_rule::
    append_sys_hdr_options (T& args) const
    {
      assert (sys_hdr_dirs_mode + sys_hdr_dirs_extra <= sys_hdr_dirs.size ());

      // Note that the mode options are added as part of cmode.
      //
      auto b (sys_hdr_dirs.begin () + sys_hdr_dirs_mode);
      auto x (b + sys_hdr_dirs_extra);

      // Add extras.
      //
      // Note: starting from 16.10, MSVC gained /external:I option though it
      // doesn't seem to affect the order, only "system-ness".
      //
      append_option_values (
        args,
        cclass == compiler_class::gcc  ? "-isystem" :
        cclass == compiler_class::msvc ? (isystem (*this)
                                          ? "/external:I"
                                          : "/I") : "-I",
        b, x,
        [] (const dir_path& d) {return d.string ().c_str ();});

      // For MSVC if we have no INCLUDE environment variable set, then we
      // add all of them. But we want extras to come first. Note also that
      // clang-cl takes care of this itself.
      //
      // Note also that we don't use /external:I to have consistent semantics
      // with when INCLUDE is set (there is separate /external:env for that).
      //
      if (ctype == compiler_type::msvc && cvariant != "clang")
      {
        if (!getenv ("INCLUDE"))
        {
          append_option_values (
            args, "/I",
            x, sys_hdr_dirs.end (),
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

          // Note: /interface and /internalPartition are in addition to /TP.
          //
          switch (md.type)
          {
          case unit_type::non_modular:
          case unit_type::module_impl:
            {
              break;
            }
          case unit_type::module_intf:
          case unit_type::module_intf_part:
            {
              o2 = "/interface";
              break;
            }
          case unit_type::module_impl_part:
            {
              o2 = "/internalPartition";
              break;
            }
          case unit_type::module_header:
            {
              //@@ MODHDR TODO: /exportHeader
              assert (false);
              break;
            }
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

              if (x_assembler_cpp (md.src))
                o2 = "assembler-with-cpp";
              else
              {
                bool obj (x_objective (md.src));

                switch (x_lang)
                {
                case lang::c:   o2 = obj ? "objective-c"   : "c";   break;
                case lang::cxx: o2 = obj ? "objective-c++" : "c++"; break;
                }
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
    match (action a, target& t) const
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
        if (ut == unit_type::module_header ? p.is_a (**x_hdrs) || p.is_a<h> () :
            ut == unit_type::module_intf   ? p.is_a (*x_mod)                   :
            p.is_a (x_src)                        ||
            (x_asp != nullptr && p.is_a (*x_asp)) ||
            (x_obj != nullptr && p.is_a (*x_obj)))
        {
          // Save in the target's auxiliary storage.
          //
          t.data (a, match_data (*this, ut, p));
          return true;
        }
      }

      l4 ([&]{trace << "no " << x_lang << " source file for target " << t;});
      return false;
    }

    // Append or hash library options from a pair of *.export.* variables
    // (first is x.* then cc.*) recursively, prerequisite libraries first.
    // If common is true, then only append common options from the lib{}
    // groups.
    //
    template <typename T>
    void compile_rule::
    append_library_options (appended_libraries& ls, T& args,
                            const scope& bs,
                            const scope* is, // Internal scope.
                            action a, const file& l, bool la,
                            linfo li, bool common,
                            library_cache* lib_cache) const
    {
      struct data
      {
        appended_libraries& ls;
        T& args;
        const scope* is;
      } d {ls, args, is};

      // See through utility libraries.
      //
      auto imp = [] (const target& l, bool la) {return la && l.is_a<libux> ();};

      auto opt = [&d, this] (const target& l, // Note: could be lib{}
                             const string& t, bool com, bool exp)
      {
        // Note that in our model *.export.poptions are always "interface",
        // even if set on liba{}/libs{}, unlike loptions.
        //
        if (!exp) // Ignore libux.
          return true;

        // Suppress duplicates.
        //
        // Compilation is the simple case: we can add the options on the first
        // occurrence of the library and ignore (and prune) all subsequent
        // occurrences. See GitHub issue #114 for details.
        //
        if (find (d.ls.begin (), d.ls.end (), &l) != d.ls.end ())
          return false;

        // Note: go straight for the public variable pool.
        //
        const variable& var (
          com
          ? c_export_poptions
          : (t == x
             ? x_export_poptions
             : l.ctx.var_pool[t + ".export.poptions"]));

        if (const strings* ops = cast_null<strings> (l[var]))
        {
          // If enabled, remap -I to -isystem or /external:I for paths that
          // are outside of the internal scope provided the library is not
          // whitelisted.
          //
          auto whitelist = [&l] (const strings& pats)
          {
            return find_if (pats.begin (), pats.end (),
                            [&l] (const string& pat)
                            {
                              return path_match (l.name, pat);
                            }) != pats.end ();
          };

          const scope* is (d.is);

          if (is != nullptr && c_ilibs != nullptr && whitelist (*c_ilibs))
            is = nullptr;

          if (is != nullptr && x_ilibs != nullptr && whitelist (*x_ilibs))
            is = nullptr;

          for (auto i (ops->begin ()), e (ops->end ()); i != e; ++i)
          {
            const string& o (*i);

            if (is != nullptr)
            {
              // See if this is -I<dir> or -I <dir> (or /I... for MSVC).
              //
              // While strictly speaking we can only attempt to recognize
              // options until we hit something unknown (after that, we don't
              // know what's an option and what's a value), it doesn't seem
              // likely to cause issues here, where we only expect to see -I,
              // -D, and -U.
              //
              bool msvc (cclass == compiler_class::msvc);

              if ((o[0] == '-' || (msvc && o[0] == '/')) && o[1] == 'I')
              {
                bool sep (o.size () == 2); // -I<dir> vs -I <dir>

                const char* v (nullptr);
                size_t vn (0);
                if (sep)
                {
                  if (i + 1 == e)
                    ; // Append as is and let the compiler complain.
                  else
                  {
                    ++i;
                    v = i->c_str ();
                    vn = i->size ();
                  }
                }
                else
                {
                  v = o.c_str () + 2;
                  vn = o.size () - 2;
                }

                if (v != nullptr)
                {
                  // See if we need to translate the option for this path. We
                  // only do this for absolute paths and try to optimize for
                  // the already normalized ones.
                  //
                  if (path_traits::absolute (v))
                  {
                    const char* p (nullptr);
                    size_t pn (0);

                    dir_path nd;
                    if (path_traits::normalized (v, vn, true /* separators */))
                    {
                      p = v;
                      pn = vn;
                    }
                    else
                    try
                    {
                      nd = dir_path (v, vn);
                      nd.normalize ();
                      p = nd.string ().c_str ();
                      pn = nd.string ().size ();
                    }
                    catch (const invalid_path&)
                    {
                      // Ignore this path.
                    }

                    if (p != nullptr)
                    {
                      auto sub = [p, pn] (const dir_path& d)
                      {
                        return path_traits::sub (
                          p, pn,
                          d.string ().c_str (), d.string ().size ());
                      };

                      // Translate if it's neither in src nor in out of the
                      // internal scope.
                      //
                      if (!sub (is->src_path ()) &&
                          (is->out_eq_src () || !sub (is->out_path ())))
                      {
                        // Note: must use original value (path is temporary).
                        //
                        append_option (d.args,
                                       msvc ? "/external:I" : "-isystem");
                        append_option (d.args, v);
                        continue;
                      }
                    }
                  }

                  // If not translated, preserve the original form.
                  //
                  append_option (d.args, o.c_str ());
                  if (sep) append_option (d.args, v);

                  continue;
                }
              }
            }

            append_option (d.args, o.c_str ());
          }
        }

        // From the process_libraries() semantics we know that the final call
        // is always for the common options.
        //
        if (com)
          d.ls.push_back (&l);

        return true;
      };

      process_libraries (a, bs, li, sys_lib_dirs,
                         l, la, 0, // lflags unused.
                         imp, nullptr, opt,
                         false /* self */,
                         common /* proc_opt_group */,
                         lib_cache);
    }

    void compile_rule::
    append_library_options (appended_libraries& ls, strings& args,
                            const scope& bs,
                            action a, const file& l, bool la,
                            linfo li,
                            bool common,
                            bool original) const
    {
      const scope* is (!original && isystem (*this)
                       ? effective_iscope (bs)
                       : nullptr);
      append_library_options (ls, args, bs, is, a, l, la, li, common, nullptr);
    }

    template <typename T>
    void compile_rule::
    append_library_options (T& args,
                            const scope& bs,
                            action a, const target& t, linfo li) const
    {
      auto iscope = [this, &bs, is = optional<const scope*> ()] () mutable
      {
        if (!is)
          is = isystem (*this) ? effective_iscope (bs) : nullptr;

        return *is;
      };

      appended_libraries ls;
      library_cache lc;

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
            append_library_options (ls,
                                    args,
                                    bs, iscope (),
                                    a, *f, la,
                                    li,
                                    false /* common */,
                                    &lc);
          }
        }
      }
    }

    // Append library prefixes based on the *.export.poptions variables
    // recursively, prerequisite libraries first.
    //
    void compile_rule::
    append_library_prefixes (appended_libraries& ls, prefix_map& pm,
                             const scope& bs,
                             action a, const target& t, linfo li) const
    {
      struct data
      {
        appended_libraries& ls;
        prefix_map&         pm;
      } d {ls, pm};

      auto imp = [] (const target& l, bool la) {return la && l.is_a<libux> ();};

      auto opt = [&d, this] (const target& lt,
                             const string& t, bool com, bool exp)
      {
        if (!exp)
          return true;

        const file& l (lt.as<file> ());

        // Suppress duplicates like in append_library_options().
        //
        if (find (d.ls.begin (), d.ls.end (), &l) != d.ls.end ())
          return false;

        // If this target does not belong to any project (e.g, an "imported as
        // installed" library), then it can't possibly generate any headers
        // for us.
        //
        if (const scope* rs = l.base_scope ().root_scope ())
        {
          // Note: go straight for the public variable pool.
          //
          const variable& var (
            com
            ? c_export_poptions
            : (t == x
               ? x_export_poptions
               : l.ctx.var_pool[t + ".export.poptions"]));

          append_prefixes (d.pm, *rs, l, var);
        }

        if (com)
          d.ls.push_back (&l);

        return true;
      };

      // The same logic as in append_library_options().
      //
      const function<bool (const target&, bool)> impf (imp);
      const function<bool (const target&, const string&, bool, bool)> optf (opt);

      library_cache lib_cache;
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
                             impf, nullptr, optf,
                             false /* self */,
                             false /* proc_opt_group */,
                             &lib_cache);
        }
      }
    }

    recipe compile_rule::
    apply (action a, target& xt) const
    {
      tracer trace (x, "compile_rule::apply");

      file& t (xt.as<file> ()); // Either obj*{} or bmi*{}.

      match_data& md (t.data<match_data> (a));

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
        if ((ut == unit_type::module_intf      ||
             ut == unit_type::module_intf_part ||
             ut == unit_type::module_impl_part) && cast_true<bool> (t[b_binless]))
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
            // Well, that was the case until we've added support for immediate
            // importation of libraries, which happens during the load phase
            // and natually leaves the library unmatched. While we could have
            // returned from search_library() an indication of whether the
            // library has been matched, this doesn't seem worth the trouble.
            //
            if (p.proj ())
            {
              pt = search_library (a,
                                   sys_lib_dirs,
                                   usr_lib_dirs,
                                   p.prerequisite);

#if 0
              if (pt != nullptr && !modules)
                continue;
#endif
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

          if (pt == dir ||
              (a.operation () == clean_id && !pt->dir.sub (rs.out_path ())))
            continue;
        }

        match_async (a, *pt, ctx.count_busy (), t[a].task_count);

        if (p == md.src)
          src_i = pts.size ();

        pts.push_back (prerequisite_target (pt, pi));
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
        // an obj?{} target directly.
        //
        // @@ If for some reason unmatch fails, this messes up the for_install
        //    logic because we will update this library during match. Perhaps
        //    we should postpone updating them until execute if we failed to
        //    unmatch. See how we do this in ad hoc rule.
        //
        pair<bool, target_state> mr (
          match_complete (
            a,
            *pt,
            pt->is_a<liba> () || pt->is_a<libs> () || pt->is_a<libux> ()
            ? unmatch::safe
            : unmatch::none));

        if (mr.first)
          pt = nullptr; // Ignore in execute.
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

        // NOTE: see similar code in adhoc_buildscript_rule::apply().

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
          fsdir_rule::perform_update_direct (a, *dir);
        }

        // Note: the leading '@' is reserved for the module map prefix (see
        // extract_modules()) and no other line must start with it.
        //
        // NOTE: see also the predefs rule if changing anything here.
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
          assert (!p.empty ()); // Sanity check.

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
          extract_headers (a, bs, t, li, src, md, dd, u, mt, is, psrc);
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
                  // Appended to by to_module_info() below.
                  //
                  tu.module_info.imports.clear ();

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
        if (u && dd.reading () && !ctx.dry_run_option)
          dd.touch = timestamp_unknown;

        dd.close (false /* mtime_check */);
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
      case perform_update_id: return move (md);
      case perform_clean_id:
        {
          return [this, srct = &md.src.type ()] (action a, const target& t)
          {
            return perform_clean (a, t, *srct);
          };
        }
      default: return noop_recipe; // Configure update.
      }
    }

    void compile_rule::
    append_prefixes (prefix_map& m,
                     const scope& rs, const target& t,
                     const variable& var) const
    {
      tracer trace (x, "compile_rule::append_prefixes");

      if (auto l = t[var])
      {
        const auto& v (cast<strings> (l));

        for (auto i (v.begin ()), e (v.end ()); i != e; ++i)
        {
          const string& o (*i);

          // -I can either be in the "-Ifoo" or "-I foo" form. For MSVC it
          // can also be /I.
          //
          // Note that we naturally assume that -isystem, /external:I, etc.,
          // are not relevant here.
          //
          bool msvc (cclass == compiler_class::msvc);

          if (!((o[0] == '-' || (msvc && o[0] == '/')) && o[1] == 'I'))
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
          if (d.sub (rs.out_path ()))
            append_prefix (trace, m, t, move (d));
        }
      }
    }

    auto compile_rule::
    build_prefix_map (const scope& bs,
                      action a,
                      const target& t,
                      linfo li) const -> prefix_map
    {
      prefix_map pm;

      // First process our own.
      //
      const scope& rs (*bs.root_scope ());
      append_prefixes (pm, rs, t, x_poptions);
      append_prefixes (pm, rs, t, c_poptions);

      // Then process the include directories from prerequisite libraries.
      //
      appended_libraries ls;
      append_library_prefixes (ls, pm, bs, a, t, li);

      return pm;
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
    struct compile_rule::gcc_module_mapper_state
    {
      size_t skip;              // Number of depdb entries to skip.
      size_t header_units = 0;  // Number of header units imported.
      module_imports& imports;  // Unused (potentially duplicate suppression).

      // Include translation (looked up lazily).
      //
      optional<const build2::cc::translatable_headers*> translatable_headers;

      small_vector<string, 2> batch; // Reuse buffers.
      size_t                  batch_n = 0;

      gcc_module_mapper_state (size_t s, module_imports& i)
          : skip (s), imports (i) {}
    };

    // The module mapper is called on one line of input at a time. It should
    // return nullopt if another line is expected (batch), false if the mapper
    // interaction should be terminated, and true if it should be continued.
    //
    optional<bool> compile_rule::
    gcc_module_mapper (gcc_module_mapper_state& st,
                       action a, const scope& bs, file& t, linfo li,
                       const string& l,
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
      small_vector<string, 2>& batch (st.batch);
      size_t& batch_n (st.batch_n);

      // Add the next line.
      //
      {
        if (batch.size () == batch_n)
          batch.push_back (l);
        else
          batch[batch_n] = l;

        batch_n++;
      }

      // Check if more is expected in this batch.
      //
      {
        string& r (batch[batch_n - 1]);

        if (r.back () == ';')
        {
          // Strip the trailing `;` word.
          //
          r.pop_back ();
          r.pop_back ();

          return nullopt;
        }
      }

      if (verb >= 3)
      {
        // It doesn't feel like buffering this would be useful.
        //
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
        size_t rn (r.size ());

        // The protocol uses a peculiar quoting/escaping scheme that can be
        // summarized as follows (see the libcody documentation for details):
        //
        // - Words are seperated with spaces and/or tabs.
        //
        // - Words need not be quoted if they only containing characters from
        //   the [-+_/%.A-Za-z0-9] set.
        //
        // - Otherwise words need to be single-quoted.
        //
        // - Inside single-quoted words, the \n \t \' and \\ escape sequences
        //   are recognized.
        //
        // Note that we currently don't treat abutted quotes (as in a' 'b) as
        // a single word (it doesn't seem plausible that we will ever receive
        // something like this).
        //
        size_t b (0), e (0), n; bool q; // Next word.

        auto next = [&r, rn, &b, &e, &n, &q] () -> size_t
        {
          if (b != e)
            b = e;

          // Skip leading whitespaces.
          //
          for (; b != rn && (r[b] == ' ' || r[b] == '\t'); ++b) ;

          if (b != rn)
          {
            q = (r[b] == '\'');

            // Find first trailing whitespace or closing quote.
            //
            for (e = b + 1; e != rn; ++e)
            {
              // Note that we deal with invalid quoting/escaping in unquote().
              //
              switch (r[e])
              {
              case ' ':
              case '\t':
                if (q)
                  continue;
                else
                  break;
              case '\'':
                if (q)
                {
                  ++e; // Include closing quote (hopefully).
                  break;
                }
                else
                {
                  assert (false); // Abutted quote.
                  break;
                }
              case '\\':
                if (++e != rn) // Skip next character (hopefully).
                  continue;
                else
                  break;
              default:
                continue;
              }

              break;
            }

            n = e - b;
          }
          else
          {
            q = false;
            e = rn;
            n = 0;
          }

          return n;
        };

        // Unquote into tmp the current word returning false if malformed.
        //
        auto unquote = [&r, &b, &n, &q, &tmp] (bool clear = true) -> bool
        {
          if (q && n > 1)
          {
            size_t e (b + n - 1);

            if (r[b] == '\'' && r[e] == '\'')
            {
              if (clear)
                tmp.clear ();

              size_t i (b + 1);
              for (; i != e; ++i)
              {
                char c (r[i]);
                if (c == '\\')
                {
                  if (++i == e)
                  {
                    i = 0;
                    break;
                  }

                  c = r[i];
                  if      (c == 'n') c = '\n';
                  else if (c == 't') c = '\t';
                }
                tmp += c;
              }

              if (i == e)
                return true;
            }
          }

          return false;
        };

#if 0
#define UNQUOTE(x, y)                     \
        r = x; rn = r.size (); b = e = 0; \
        assert (next () && unquote () && tmp == y)

        UNQUOTE ("'foo bar'", "foo bar");
        UNQUOTE (" 'foo bar' ", "foo bar");
        UNQUOTE ("'foo\\\\bar'", "foo\\bar");
        UNQUOTE ("'\\'foo bar'", "'foo bar");
        UNQUOTE ("'foo bar\\''", "foo bar'");
        UNQUOTE ("'\\'foo\\\\bar\\''", "'foo\\bar'");

        fail << "all good";
#endif

        // Escape if necessary the specified string and append to r.
        //
        auto escape = [&r] (const string& s)
        {
          size_t b (0), e, n (s.size ());
          while (b != n && (e = s.find_first_of ("\\'\n\t", b)) != string::npos)
          {
            r.append (s, b, e - b); // Preceding chunk.

            char c (s[e]);
            r += '\\';
            r += (c == '\n' ? 'n' : c == '\t' ? 't' : c);
            b = e + 1;
          }

          if (b != n)
            r.append (s, b, e); // Final chunk.
        };

        // Quote and escape if necessary the specified string and append to r.
        //
        auto quote = [&r, &escape] (const string& s)
        {
          if (find_if (s.begin (), s.end (),
                       [] (char c)
                       {
                         return !((c >= 'a' && c <= 'z') ||
                                  (c >= '0' && c <= '9') ||
                                  (c >= 'A' && c <= 'Z') ||
                                  c == '-' || c == '_' || c == '/' ||
                                  c == '.' || c == '+' || c == '%');
                       }) == s.end ())
          {
            r += s;
          }
          else
          {
            r += '\'';
            escape (s);
            r += '\'';
          }
        };

#if 0
#define QUOTE(x, y)            \
        r.clear (); quote (x); \
        assert (r == y)

        QUOTE ("foo/Bar-7.h", "foo/Bar-7.h");

        QUOTE ("foo bar", "'foo bar'");
        QUOTE ("foo\\bar", "'foo\\\\bar'");
        QUOTE ("'foo bar", "'\\'foo bar'");
        QUOTE ("foo bar'", "'foo bar\\''");
        QUOTE ("'foo\\bar'", "'\\'foo\\\\bar\\''");

        fail << "all good";
#endif

        next (); // Request name.

        auto name = [&r, b, n, q] (const char* c) -> bool
        {
          // We can reasonably assume a command will never be quoted.
          //
          return (!q                       &&
                  r.compare (b, n, c) == 0 &&
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
            path f;
            if (!q)
              f = path (r, b, n);
            else if (unquote ())
              f = path (tmp);
            else
            {
              r = "ERROR 'malformed quoting/escaping in request'";
              continue;
            }

            bool exists (true);

            // The TU path we pass to the compiler is always absolute so any
            // ""-includes will also be absolute. As a result, the only way to
            // end up with a relative path is by using relative -I which
            // doesn't make much sense in our world (it will be relative to
            // CWD).
            //
            if (exists && f.relative ())
            {
              r = "ERROR 'relative header path ";
              escape (f.string ());
              r += '\'';
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
                enter_header (
                  a, bs, t, li,
                  move (f), false /* cache */, false /* normalized */,
                  pfx_map, so_map));

              ht = er.first;
              remapped = er.second;

              if (remapped)
              {
                r = "ERROR 'remapping of headers not supported'";
                continue;
              }

              // If we couldn't enter this header as a target or find a rule
              // to update it, then it most likely means a misspelled header
              // (rather than a broken generated header setup) and our
              // diagnostics won't really add anything to the compiler's. So
              // let's only print it at -V or higher.
              //
              if (ht == nullptr) // f is still valid.
              {
                assert (!exists); // Sanity check.

                if (verb > 2)
                {
                  diag_record dr;
                  dr << error << "header " << f << " not found and no "
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
              // Note: if ht is NULL, f is still valid.
              //
              r = "ERROR 'unable to update header ";
              escape ((ht != nullptr ? ht->path () : f).string ());
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
                  // @@ This still breaks if the header path contains spaces.
                  //    GCC bug 110153.
                  //
                  tmp = "@ ";
                  if (!q) tmp.append (r, b, n);
                  else    unquote (false /* clear */); // Can't fail.
                  tmp += ' ';
                  tmp += bp;

                  dd.expect (tmp);
                  st.header_units++;
                }

                r = "PATHNAME ";
                quote (bp);
              }
              catch (const failed&)
              {
                r = "ERROR 'unable to update header unit for ";
                escape (hs);
                r += '\'';
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
        tmp.assign (r, b, n); // Request name (unquoted).
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
      // @@ It's theoretically possible that we get blocked writing the
      //    response while the compiler gets blocked writing the diagnostics.
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

      batch_n = 0; // Start a new batch.

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
                enter_header (
                  a, bs, t, li,
                  move (f), false /* cache */, false /* normalized */,
                  pfx_map, so_map));

              if (!r.second) // Shouldn't be remapped.
                ht = r.first;
            }

            if (ht != pts.back ())
            {
              ht = &pts.back ().target->as<file> ();
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
                enter_header (
                  a, bs, t, li,
                  move (f), false /* cache */, false /* normalized */,
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
              if (ht == nullptr) // f is still valid.
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
              // Note: if ht is NULL, f is still valid.
              //
              rs = !exists
                ? string ("INCLUDE")
                : ("ERROR unable to update header '" +
                   (ht != nullptr ? ht->path () : f).string () + '\'');

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
              rs = "ERROR unable to update header unit '" + hp + '\'';
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

    //atomic_count cache_hit {0};
    //atomic_count cache_mis {0};
    //atomic_count cache_cls {0};

    // The fp path is only moved from on success.
    //
    // Note: this used to be a lambda inside extract_headers() so refer to the
    // body of that function for the overall picture.
    //
    pair<const file*, bool> compile_rule::
    enter_header (action a, const scope& bs, file& t, linfo li,
                  path&& fp, bool cache, bool norm,
                  optional<prefix_map>& pfx_map,
                  const srcout_map& so_map) const
    {
      tracer trace (x, "compile_rule::enter_header");

      // It's reasonable to expect the same header to be included by multiple
      // translation units, which means we will be re-doing this work over and
      // over again. And it's not exactly cheap, taking up to 50% of an
      // up-to-date check time on some projects. So we are going to cache the
      // header path to target mapping.
      //
      // While we pass quite a bit of specific "context" (target, base scope)
      // to enter_file(), here is the analysis why the result will not depend
      // on this context for the non-absent header (fp is absolute):
      //
      // 1. Let's start with the base scope (bs). Firstly, the base scope
      //    passed to map_extension() is the scope of the header (i.e., it is
      //    the scope of fp.directory()). Other than that, the target base
      //    scope is only passed to build_prefix_map() which is only called
      //    for the absent header (linfo is also only used here).
      //
      // 2. Next is the target (t). It is passed to build_prefix_map() but
      //    that doesn't matter for the same reason as in (1). Other than
      //    that, it is only passed to build2::search() which in turn passes
      //    it to target type-specific prerequisite search callback (see
      //    target_type::search) if one is not NULL. The target type in
      //    question here is one of the headers and we know all of them use
      //    the standard file_search() which ignores the passed target.
      //
      // 3. Finally, so_map could be used for an absolute fp. While we could
      //    simply not cache the result if it was used (second half of the
      //    result pair is true), there doesn't seem to be any harm in caching
      //    the remapped path->target mapping. In fact, if to think about it,
      //    there is no harm in caching the generated file mapping since it
      //    will be immediately generated and any subsequent inclusions we
      //    will "see" with an absolute path, which we can resolve from the
      //    cache.
      //
      // To put it another way, all we need to do is make sure that if we were
      // to not return an existing cache entry, the call to enter_file() would
      // have returned exactly the same path/target.
      //
      // @@ Could it be that the header is re-mapped in one config but not the
      //    other (e.g., when we do both in src and in out builds and we pick
      //    the generated header in src)? If so, that would lead to a
      //    divergence. I.e., we would cache the no-remap case first and then
      //    return it even though the re-map is necessary? Why can't we just
      //    check for re-mapping ourselves? A: the remapping logic in
      //    enter_file() is not exactly trivial.
      //
      //    But on the other hand, I think we can assume that different
      //    configurations will end up with different caches. In other words,
      //    we can assume that for the same "cc amalgamation" we use only a
      //    single "version" of a header. Seems reasonable.
      //
      // Note also that while it would have been nice to have a unified cc
      // cache, the map_extension() call is passed x_incs which is module-
      // specific. In other words, we may end up mapping the same header to
      // two different targets depending on whether it is included from, say,
      // C or C++ translation unit. We could have used a unified cache for
      // headers that were mapped using the fallback target type, which would
      // cover the installed headers. Maybe, one day (it's also possible that
      // separate caches reduce contention).
      //
      // Another related question is where we want to keep the cache: project,
      // strong amalgamation, or weak amalgamation (like module sidebuilds).
      // Some experimentation showed that weak has the best performance (which
      // suggest that a unified cache will probably be a win).
      //
      // Note also that we don't need to clear this cache since we never clear
      // the targets set. In other words, the only time targets are
      // invalidated is when we destroy the build context, which also destroys
      // the cache.
      //
      const config_module& hc (*header_cache_);

      // First check the cache.
      //
      config_module::header_key hk;

      bool e (fp.absolute ());
      if (e)
      {
        if (!norm)
        {
          normalize_external (fp, "header");
          norm = true;
        }

        hk.file = move (fp);
        hk.hash = hash<path> () (hk.file);

        slock l (hc.header_map_mutex);
        auto i (hc.header_map.find (hk));
        if (i != hc.header_map.end ())
        {
          //cache_hit.fetch_add (1, memory_order_relaxed);
          return make_pair (i->second, false);
        }

        fp = move (hk.file);

        //cache_mis.fetch_add (1, memory_order_relaxed);
      }

      struct data
      {
        linfo li;
        optional<prefix_map>& pfx_map;
      } d {li, pfx_map};

      // If it is outside any project, or the project doesn't have such an
      // extension, assume it is a plain old C header.
      //
      auto r (enter_file (
                trace, "header",
                a, bs, t,
                fp, cache, norm,
                [this] (const scope& bs, const string& n, const string& e)
                {
                  return map_extension (bs, n, e, x_incs);
                },
                h::static_type,
                [this, &d] (action a, const scope& bs, const target& t)
                  -> const prefix_map&
                {
                  if (!d.pfx_map)
                    d.pfx_map = build_prefix_map (bs, a, t, d.li);

                  return *d.pfx_map;
                },
                so_map));

      // Cache.
      //
      if (r.first != nullptr)
      {
        hk.file = move (fp);

        // Calculate the hash if we haven't yet and re-calculate it if the
        // path has changed (header has been remapped).
        //
        if (!e || r.second)
          hk.hash = hash<path> () (hk.file);

        const file* f;
        {
          ulock l (hc.header_map_mutex);
          auto p (hc.header_map.emplace (move (hk), r.first));
          f = p.second ? nullptr : p.first->second;
        }

        if (f != nullptr)
        {
          //cache_cls.fetch_add (1, memory_order_relaxed);

#if 0
          assert (r.first == f);
#else
          if (r.first != f)
          {
            info   << "inconsistent header cache content" <<
              info << "encountered: " << *f <<
              info << "expected: " << *r.first <<
              info << "please report at "
                   << "https://github.com/build2/build2/issues/390";

            assert (r.first == f);
          }
#endif
        }
      }

      return r;
    }

    // Note: this used to be a lambda inside extract_headers() so refer to the
    // body of that function for the overall picture.
    //
    optional<bool> compile_rule::
    inject_header (action a, file& t,
                   const file& pt, timestamp mt, bool fail) const
    {
      tracer trace (x, "compile_rule::inject_header");

      return inject_file (trace, "header", a, t, pt, mt, fail);
    }

    // Extract and inject header dependencies. Return (in result) the
    // preprocessed source file as well as an indication if it is usable for
    // compilation (see below for details). Note that result is expected to
    // be initialized to {entry (), false}. Not using return type due to
    // GCC bug #107555.
    //
    // This is also the place where we handle header units which are a lot
    // more like auto-generated headers than modules. In particular, if a
    // header unit BMI is out-of-date, then we have to re-preprocess this
    // translation unit.
    //
    void compile_rule::
    extract_headers (action a,
                     const scope& bs,
                     file& t,
                     linfo li,
                     const file& src,
                     match_data& md,
                     depdb& dd,
                     bool& update,
                     timestamp mt,
                     module_imports& imports,
                     pair<file_cache::entry, bool>& result) const
    {
      tracer trace (x, "compile_rule::extract_headers");

      context& ctx (t.ctx);

      otype ot (li.type);

      bool reprocess (cast_false<bool> (t[c_reprocess]));

      file_cache::entry psrc;
      bool puse (true);

      // Preprocessed file extension.
      //
      const char* pext (x_assembler_cpp (src) ? ".Si"      :
                        x_objective (src)     ? x_obj_pext :
                        x_pext);

      // Preprocesor mode that preserves as much information as possible while
      // still performing inclusions. Also serves as a flag indicating whether
      // this (non-MSVC) compiler uses the separate preprocess and compile
      // setup.
      //
      const char* pp (nullptr);

      switch (ctype)
      {
      case compiler_type::gcc:
        {
          // -fdirectives-only is available since GCC 4.3.0.
          //
          if (cmaj > 4 || (cmaj == 4 && cmin >= 3))
          {
            // Note that for assembler-with-cpp GCC currently forces full
            // preprocessing in (what appears to be) an attempt to paper over
            // a deeper issue (see GCC bug 109534). If/when that bug gets
            // fixed, we can enable this on our side. Note that Clang's
            // -frewrite-includes also has issues (see below).
            //
            if (!x_assembler_cpp (src))
              pp = "-fdirectives-only";
          }

          break;
        }
      case compiler_type::clang:
        {
          // -frewrite-includes is available since Clang 3.2.0.
          //
          if (cmaj > 3 || (cmaj == 3 && cmin >= 2))
          {
            // While Clang's -frewrite-includes appears to work, there are
            // some issues with correctly tracking location information
            // (manifests itself as wrong line numbers in debug info, for
            // example). The result also appears to reference the .Si file
            // instead of the original source file for some reason.
            //
            if (!x_assembler_cpp (src))
              pp = "-frewrite-includes";
          }

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
      // what we are going to do to work around this: we will sense if any
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
      // BTW, another example of a plausible preprocessor warnings are C4819
      // and C4828 (character unrepresentable in source charset).
      //
      // Finally, if we are using the module mapper, then all this mess falls
      // away: we only run the compiler once, we let the diagnostics through,
      // we get a compiler error (with location information) if a header is
      // not found, and there is no problem with outdated generated headers
      // since we update/remap them before the compiler has a chance to read
      // them. Overall, this "dependency mapper" approach is how it should
      // have been done from the beginning. Note: that's the ideal world,
      // the reality is that the required mapper extensions are not (yet)
      // in libcody/GCC.

      // Note: diagnostics sensing is currently only supported if dependency
      // info is written to a file (see above).
      //
      bool sense_diag (false);

      // And here is another problem: if we have an already generated header
      // in src and the one in out does not yet exist, then the compiler will
      // pick the one in src and we won't even notice. Note that this is not
      // only an issue with mixing in and out of source builds (which does feel
      // wrong but is oh so convenient): this is also a problem with
      // pre-generated headers, a technique we use to make installing the
      // generator by end-users optional by shipping pre-generated headers.
      //
      // This is a nasty problem that doesn't seem to have a perfect solution
      // (except, perhaps, C++ modules and/or module mapper). So what we are
      // going to do is try to rectify the situation by detecting and
      // automatically remapping such mis-inclusions. It works as follows.
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
      // require anything explicit from the user.
      //
      // One tricky area in this setup are target groups: if the generated
      // sources are mentioned in the buildfile as a group, then there might
      // be no header target (yet). The way we solve this is by requiring code
      // generator rules to cooperate and create at least the header target as
      // part of the group creation. While not all members of the group may be
      // generated depending on the options (e.g., inline files might be
      // suppressed), headers are usually non-optional.
      //
      srcout_map so_map;

      // Dynamic module mapper.
      //
      bool mod_mapper (false);

      // The gen argument to init_args() is in/out. The caller signals whether
      // to force the generated header support and on return it signals
      // whether this support is enabled. If gen is false, then stderr is
      // expected to be either discarded or merged with sdtout.
      //
      // Return NULL if the dependency information goes to stdout and a
      // pointer to the temporary file path otherwise.
      //
      auto init_args = [a, &t, ot, li, reprocess, pext,
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
            srcout_builder builder (ctx, so_map);

            // Try to be fast and efficient by reusing buffers as much as
            // possible.
            //
            string ds;

            for (auto i (args.begin ()), e (args.end ()); i != e; ++i)
            {
              const char* o (*i);

              // -I can either be in the "-Ifoo" or "-I foo" form. For VC it
              // can also be /I.
              //
              // Note also that append_library_options() may have translated
              // -I to -isystem or /external:I so we have to recognize those
              // as well.
              //
              {
                bool msvc (cclass == compiler_class::msvc);

                size_t p (0);
                if (o[0] == '-' || (msvc && o[0] == '/'))
                {
                  p = (o[1] == 'I'                                     ?  2 :
                       !msvc && strncmp (o + 1, "isystem",     7) == 0 ?  8 :
                       msvc  && strncmp (o + 1, "external:I", 10) == 0 ? 11 : 0);
                }

                if (p == 0)
                {
                  builder.skip ();
                  continue;
                }

                size_t n (strlen (o));
                if (n == p)
                {
                  if (++i == e)
                    break; // Let the compiler complain.

                  ds = *i;
                }
                else
                  ds.assign (o + p, n - p);
              }

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
                  if (!builder.next (move (d)))
                    ds = move (d).string (); // Move the buffer back out.
                }
                else
                  builder.skip ();
              }
              else
                builder.skip ();
            }
          }

          if (md.symexport)
            append_symexport_options (args, t);

          // Some compile options (e.g., -std, -m) affect the preprocessor.
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

              // Note that for MSVC stderr is merged with stdout and is then
              // parsed, so no append_diag_color_options() call.

              // See perform_update() for details on the choice of options.
              //
              // NOTE: see also the predefs rule if adding anything here.
              //
              {
                bool sc (find_option_prefixes (
                           {"/source-charset:", "-source-charset:"}, args));
                bool ec (find_option_prefixes (
                           {"/execution-charset:", "-execution-charset:"}, args));

                if (!sc && !ec)
                  args.push_back ("/utf-8");
                else
                {
                  if (!sc)
                    args.push_back ("/source-charset:UTF-8");

                  if (!ec)
                    args.push_back ("/execution-charset:UTF-8");
                }
              }

              if (cvariant != "clang" && isystem (*this))
              {
                if (find_option_prefixes ({"/external:I", "-external:I"}, args) &&
                    !find_option_prefixes ({"/external:W", "-external:W"}, args))
                  args.push_back ("/external:W0");
              }

              if (x_lang == lang::cxx &&
                  !find_option_prefixes ({"/EH", "-EH"}, args))
                args.push_back ("/EHsc");

              // NOTE: see similar code in search_modules().
              //
              if (!find_option_prefixes ({"/MD", "/MT", "-MD", "-MT"}, args))
                args.push_back ("/MD");

              args.push_back ("/P");            // Preprocess to file.
              args.push_back ("/showIncludes"); // Goes to stdout (with diag).
              if (pp != nullptr)
                args.push_back (pp);            // /C (preserve comments).
              args.push_back ("/WX");           // Warning as error (see above).

              msvc_sanitize_cl (args);

              psrc = ctx.fcache->create (t.path () + pext, !modules);

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
              append_options (args, cmode);
              append_sys_hdr_options (args); // Extra system header dirs (last).

              // If not gen, then stderr is discarded.
              //
              if (gen)
                append_diag_color_options (args);

              // See perform_update() for details on the choice of options.
              //
              // NOTE: see also the predefs rule if adding anything here.
              //
              if (!find_option_prefix ("-finput-charset=", args))
                args.push_back ("-finput-charset=UTF-8");

              if (ot == otype::s)
              {
                if (tclass == "linux" || tclass == "bsd")
                  args.push_back ("-fPIC");
              }

              if (ctype == compiler_type::clang && tsys == "win32-msvc")
              {
                if (!find_options ({"-nostdlib", "-nostartfiles"}, args))
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
                psrc = ctx.fcache->create (t.path () + pext, !modules);
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
        auto fail = [&ctx] (const auto& h) -> optional<bool>
        {
          bool df (!ctx.match_only && !ctx.dry_run_option);

          diag_record dr;
          dr << error << "header " << h << " not found and no rule to "
             << "generate it";

          if (df)
            dr << info << "failure deferred to compiler diagnostics";

          if (verb < 4)
            dr << info << "re-run with --verbose=4 for more information";

          if (df)
            return nullopt;
          else
            dr << endf;
        };

        if (const file* ht = enter_header (
              a, bs, t, li,
              move (hp), cache, cache /* normalized */,
              pfx_map, so_map).first)
        {
          // If we are reading the cache, then it is possible the file has
          // since been removed (think of a header in /usr/local/include that
          // has been uninstalled and now we need to use one from
          // /usr/include). This will lead to the match failure which we
          // translate to a restart. And, yes, this case will trip up
          // inject_header(), not enter_header().
          //
          if (optional<bool> u = inject_header (a, t, *ht, mt, false /*fail*/))
          {
            // Verify/add it to the dependency database.
            //
            if (!cache)
              dd.expect (ht->path ());

            skip_count++;
            return *u;
          }
          else if (cache)
          {
            dd.write (); // Invalidate this line.
            return true;
          }
          else
            return fail (*ht);
        }
        else
          return fail (hp); // hp is still valid.
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
                        move (hp), true /* cache */, false /* normalized */,
                        pfx_map, so_map).first);

        if (ht == nullptr) // hp is still valid.
        {
          diag_record dr;
          dr << error << "header " << hp << " not found and no rule to "
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

      // If things go wrong (and they often do in this area), give the user a
      // bit extra context.
      //
      auto df = make_diag_frame (
        [&src](const diag_record& dr)
        {
          if (verb != 0)
            dr << info << "while extracting header dependencies from " << src;
        });

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
              if (modules)
              {
                result.first = ctx.fcache->create_existing (t.path () + pext);
                result.second = true;
              }

              return;
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
              return;
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

            // We use the fdstream_mode::skip mode on stdout (cannot be used
            // on both) and so dbuf must be destroyed (closed) first.
            //
            ifdstream is (ifdstream::badbit);
            diag_buffer dbuf (ctx);

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

              if (mod_mapper) // Dependency info is implied by mapper requests.
              {
                assert (gen && !sense_diag); // Not used in this mode.

                // Note that here we use the skip mode on the diagnostics
                // stream which means we have to use own instance of stdout
                // stream for the correct destruction order (see below).
                //
                pr = process (cpath,
                              args,
                              -1,
                              -1,
                              diag_buffer::pipe (ctx),
                              nullptr, // CWD
                              env.empty () ? nullptr : env.data ());

                dbuf.open (args[0],
                           move (pr.in_efd),
                           fdstream_mode::non_blocking |
                           fdstream_mode::skip);
                try
                {
                  gcc_module_mapper_state mm_state (skip_count, imports);

                  // Note that while we read both streams until eof in normal
                  // circumstances, we cannot use fdstream_mode::skip for the
                  // exception case on both of them: we may end up being
                  // blocked trying to read one stream while the process may
                  // be blocked writing to the other. So in case of an
                  // exception we only skip the diagnostics and close the
                  // mapper stream hard. The latter (together with closing of
                  // the stdin stream) should happen first so the order of
                  // the following variable is important.
                  //
                  // Note also that we open the stdin stream in the blocking
                  // mode.
                  //
                  ifdstream is (move (pr.in_ofd),
                                fdstream_mode::non_blocking,
                                ifdstream::badbit); // stdout
                  ofdstream os (move (pr.out_fd));  // stdin (badbit|failbit)

                  // Read until we reach EOF on all streams.
                  //
                  // Note that if dbuf is not opened, then we automatically
                  // get an inactive nullfd entry.
                  //
                  fdselect_set fds {is.fd (), dbuf.is.fd ()};
                  fdselect_state& ist (fds[0]);
                  fdselect_state& dst (fds[1]);

                  bool more (false);
                  for (string l; ist.fd != nullfd || dst.fd != nullfd; )
                  {
                    // @@ Currently we will accept a (potentially truncated)
                    //    line that ends with EOF rather than newline.
                    //
                    if (ist.fd != nullfd && getline_non_blocking (is, l))
                    {
                      if (eof (is))
                      {
                        os.close ();
                        is.close ();

                        if (more)
                          throw_generic_ios_failure (EIO, "unexpected EOF");

                        ist.fd = nullfd;
                      }
                      else
                      {
                        optional<bool> r (
                          gcc_module_mapper (mm_state,
                                             a, bs, t, li,
                                             l, os,
                                             dd, update, bad_error,
                                             pfx_map, so_map));

                        more = !r.has_value ();

                        if (more || *r)
                          l.clear ();
                        else
                        {
                          os.close ();
                          is.close ();
                          ist.fd = nullfd;
                        }
                      }

                      continue;
                    }

                    ifdselect (fds);

                    if (dst.ready)
                    {
                      if (!dbuf.read ())
                        dst.fd = nullfd;
                    }
                  }

                  md.header_units += mm_state.header_units;
                }
                catch (const io_error& e)
                {
                  // Note that diag_buffer handles its own io errors so this
                  // is about mapper stdin/stdout.
                  //
                  if (pr.wait ())
                    fail << "io error handling " << x_lang << " compiler "
                         << "module mapper request: " << e;

                  // Fall through.
                }

                // The idea is to reduce this to the stdout case.
                //
                // We now write directly to depdb without generating and then
                // parsing an intermadiate dependency makefile.
                //
                pr.wait ();
                pr.in_ofd = nullfd;
              }
              else
              {
                // If we have no generated header support, then suppress all
                // diagnostics (if things go badly we will restart with this
                // support).
                //
                if (drmp == nullptr) // Dependency info goes to stdout.
                {
                  assert (!sense_diag); // Note: could support if necessary.

                  // For VC with /P the dependency info and diagnostics all go
                  // to stderr so redirect it to stdout.
                  //
                  int err (
                    cclass == compiler_class::msvc ?  1 : // stdout
                    !gen                           ? -2 : // /dev/null
                    diag_buffer::pipe (ctx, sense_diag /* force */));

                  pr = process (
                    cpath,
                    args,
                    0,
                    -1,
                    err,
                    nullptr, // CWD
                    env.empty () ? nullptr : env.data ());

                  if (cclass != compiler_class::msvc && gen)
                  {
                    dbuf.open (args[0],
                               move (pr.in_efd),
                               fdstream_mode::non_blocking); // Skip on stdout.
                  }
                }
                else // Dependency info goes to temporary file.
                {
                  // Since we only need to read from one stream (dbuf) let's
                  // use the simpler blocking setup.
                  //
                  int err (
                    !gen && !sense_diag ? -2 : // /dev/null
                    diag_buffer::pipe (ctx, sense_diag /* force */));

                  pr = process (cpath,
                                args,
                                0,
                                2, // Send stdout to stderr.
                                err,
                                nullptr, // CWD
                                env.empty () ? nullptr : env.data ());

                  if (gen || sense_diag)
                  {
                    dbuf.open (args[0], move (pr.in_efd));
                    dbuf.read (sense_diag /* force */);
                  }

                  if (sense_diag)
                  {
                    if (!dbuf.buf.empty ())
                    {
                      puse = false;
                      dbuf.buf.clear (); // Discard.
                    }
                  }

                  // The idea is to reduce this to the stdout case.
                  //
                  // Note that with -MG we want to read dependency info even
                  // if there is an error (in case an outdated header file
                  // caused it).
                  //
                  pr.wait ();
                  pr.in_ofd = fdopen (*drmp, fdopen_mode::in);
                }
              }

              // Read and process dependency information, if any.
              //
              if (pr.in_ofd != nullfd)
              {
                // We have two cases here: reading from stdout and potentially
                // stderr (dbuf) or reading from file (see the process startup
                // code above for details). If we have to read from two
                // streams, then we have to use the non-blocking setup. But we
                // cannot use the non-blocking setup uniformly because on
                // Windows it's only suppored for pipes. So things are going
                // to get a bit hairy.
                //
                // And there is another twist to this: for MSVC we redirect
                // stderr to stdout since the header dependency information is
                // part of the diagnostics. If, however, there is some real
                // diagnostics, we need to pass it through, potentially with
                // buffering. The way we achieve this is by later opening dbuf
                // in the EOF state and using it to buffer or stream the
                // diagnostics.
                //
                bool nb (dbuf.is.is_open ());

                // We may not read all the output (e.g., due to a restart).
                // Before we used to just close the file descriptor to signal
                // to the other end that we are not interested in the rest.
                // This works fine with GCC but Clang (3.7.0) finds this
                // impolite and complains, loudly (broken pipe). So now we are
                // going to skip until the end.
                //
                // Note that this means we are not using skip on dbuf (see
                // above for the destruction order details).
                //
                {
                  fdstream_mode m (fdstream_mode::text |
                                   fdstream_mode::skip);

                  if (nb)
                    m |= fdstream_mode::non_blocking;

                  is.open (move (pr.in_ofd), m);
                }

                fdselect_set fds;
                if (nb)
                  fds = {is.fd (), dbuf.is.fd ()};

                size_t skip (skip_count);
                string l, l2; // Reuse.
                for (bool first (true), second (false); !restart; )
                {
                  if (nb)
                  {
                    fdselect_state& ist (fds[0]);
                    fdselect_state& dst (fds[1]);

                    // We read until we reach EOF on both streams.
                    //
                    if (ist.fd == nullfd && dst.fd == nullfd)
                      break;

                    if (ist.fd != nullfd && getline_non_blocking (is, l))
                    {
                      if (eof (is))
                      {
                        ist.fd = nullfd;
                        continue;
                      }

                      // Fall through to parse (and clear) the line.
                    }
                    else
                    {
                      ifdselect (fds);

                      if (dst.ready)
                      {
                        if (!dbuf.read ())
                          dst.fd = nullfd;
                      }

                      continue;
                    }
                  }
                  else
                  {
                    if (eof (getline (is, l)))
                    {
                      if (bad_error && !l2.empty ()) // MSVC only (see below).
                        dbuf.write (l2, true /* newline */);

                      break;
                    }
                  }

                  l6 ([&]{trace << "header dependency line '" << l << "'";});

                  // Parse different dependency output formats.
                  //
                  switch (cclass)
                  {
                  case compiler_class::msvc:
                    {
                      // The first line should be the file we are compiling,
                      // unless this is clang-cl.
                      //
                      // If it is not, then we have several possibilities:
                      //
                      // First, it can be a command line warning, for example:
                      //
                      // cl : Command line warning D9025 : overriding '/W3' with '/W4'
                      //
                      // So we try to detect and skip them assuming they will
                      // also show up during the compilation proper.
                      //
                      // Another possibility is a mis-spelled option that is
                      // treated as another file to compile, for example:
                      //
                      // cl junk /nologo /P /showIncluses /TP foo.cxx
                      // junk
                      // foo.cxx
                      // c1xx: fatal error C1083: Cannot open source file: 'junk': No such file or directory
                      //
                      // Yet another possibility is that something went wrong
                      // even before we could compile anything.
                      //
                      // So the plan is to keep going (in the hope of C1083)
                      // but print the last line if there is no more input.
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
                              ; // Skip.
                            else
                            {
                              l2 = l;

                              if (!bad_error)
                              {
                                dbuf.open_eof (args[0]);
                                bad_error = true;
                              }
                            }

                            l.clear ();
                            continue;
                          }

                          l2.clear ();

                          // Fall through.
                        }

                        first = false;
                        l.clear ();
                        continue;
                      }

                      string f (next_show (l, good_error));

                      if (f.empty ()) // Some other diagnostics.
                      {
                        if (!bad_error)
                        {
                          dbuf.open_eof (args[0]);
                          bad_error = true;
                        }

                        dbuf.write (l, true /* newline */);
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
                          if (!l.empty ())
                            l5 ([&]{trace << "invalid header dependency line '"
                                          << l << "'";});

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
                        {
                          l.clear ();
                          continue;
                        }
                        else
                          pos = 3; // Skip "^: ".

                        // Fall through to the 'second' block.
                      }

                      while (pos != l.size ())
                      {
                        string f (
                          make_parser::next (
                            l, pos, make_parser::type::prereq).first);

                        if (pos != l.size () && l[pos] == ':')
                        {
                          l5 ([&]{trace << "invalid header dependency line '"
                                        << l << "'";});
                          bad_error = true;
                          break;
                        }

                        // Skip the source file.
                        //
                        if (second)
                        {
                          second = false;
                          continue;
                        }

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
                  {
                    // Note that it may be tempting to finish reading out the
                    // diagnostics before bailing out. But that may end up in
                    // a deadlock if the process gets blocked trying to write
                    // to stdout.
                    //
                    break;
                  }

                  l.clear ();
                }

                // We may bail out early from the above loop in case of a
                // restart or error. Which means the stderr stream (dbuf) may
                // still be open and we need to close it before closing the
                // stdout stream (which may try to skip).
                //
                // In this case we may also end up with incomplete diagnostics
                // so discard it.
                //
                // Generally, it may be tempting to start thinking if we
                // should discard buffered diagnostics in other cases, such as
                // restart. But remember that during serial execution it will
                // go straight to stderr so for consistency (and simplicity)
                // we should just print it unless there are good reasons not
                // to (also remember that in the restartable modes we normally
                // redirect stderr to /dev/null; see the process startup code
                // for details).
                //
                if (dbuf.is.is_open ())
                {
                  dbuf.is.close ();
                  dbuf.buf.clear ();
                }

                // Bail out early if we have deferred a failure.
                //
                // Let's ignore any buffered diagnostics in this case since
                // it would appear after the deferred failure note.
                //
                if (md.deferred_failure)
                {
                  is.close ();
                  return;
                }

                // In case of VC, we are parsing redirected stderr and if
                // things go south, we need to copy the diagnostics for the
                // user to see. Note that we should have already opened dbuf
                // at EOF above.
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
                      dbuf.write (l, true /* newline */);
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
                {
                  diag_record dr;

                  if (bad_error)
                    dr << fail << "expected error exit status from "
                       << x_lang << " compiler";

                  if (dbuf.is_open ())
                    dbuf.close (move (dr)); // Throws if error.
                }

                // Ignore expected successes (we are done).
                //
                if (!restart && psrc)
                  psrcw.close ();

                continue;
              }
              else if (pr.exit->normal ())
              {
                if (good_error) // Ignore expected errors (restart).
                {
                  if (dbuf.is_open ())
                    dbuf.close ();

                  continue;
                }
              }

              // Fall through.
            }
            catch (const io_error& e)
            {
              // Ignore buffered diagnostics (since reading it could be the
              // cause of this failure).
              //
              if (pr.wait ())
                fail << "unable to read " << x_lang << " compiler header "
                     << "dependency output: " << e;

              // Fall through.
            }

            assert (pr.exit && !*pr.exit);
            const process_exit& pe (*pr.exit);

            // For normal exit we assume the child process issued some
            // diagnostics.
            //
            if (pe.normal ())
            {
              // If this run was with the generated header support then it's
              // time to give up.
              //
              if (gen)
              {
                if (dbuf.is_open ())
                  dbuf.close (args, pe, 2 /* verbosity */);

                throw failed ();
              }

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
              {
                if (dbuf.is_open ())
                  dbuf.close ();

                l6 ([&]{trace << "trying again without generated headers";});
              }
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
                {
                  diag_record dr;
                  if (force_gen_skip && *force_gen_skip == skip_count)
                  {
                    dr <<
                      fail << "inconsistent " << x_lang << " compiler behavior" <<
                      info << "run the following two commands to investigate";

                    dr << info;
                    print_process (dr, args.data ()); // No pipes.

                    init_args ((gen = true));
                    dr << info << "";
                    print_process (dr, args.data ()); // No pipes.
                  }

                  if (dbuf.is_open ())
                    dbuf.close (move (dr)); // Throws if error.
                }

                restart = true;
                force_gen = true;
                force_gen_skip = skip_count;
                l6 ([&]{trace << "restarting with forced generated headers";});
              }
              continue;
            }
            else
            {
              if (dbuf.is_open ())
              {
                dbuf.close (args, pe, 2 /* verbosity */);
                throw failed ();
              }
              else
                run_finish (args, pr, 2 /* verbosity */);
            }
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

      result.first = move (psrc);
      result.second = puse;
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

      // Scanning .S files with our parser is hazardous since such files
      // sometimes use `#`-style comments. Presumably real compilers just
      // ignore them in some way, but it doesn't seem worth it to bother in
      // our case. Also, the checksum calculation over assembler tokens feels
      // iffy.
      //
      if (x_assembler_cpp (src))
      {
        tu.type = unit_type::non_modular;
        return "";
      }

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

              // Note: no append_diag_color_options() call since the
              // diagnostics is discarded.

              // See perform_update() for details on the choice of options.
              //
              {
                bool sc (find_option_prefixes (
                           {"/source-charset:", "-source-charset:"}, args));
                bool ec (find_option_prefixes (
                           {"/execution-charset:", "-execution-charset:"}, args));

                if (!sc && !ec)
                  args.push_back ("/utf-8");
                else
                {
                  if (!sc)
                    args.push_back ("/source-charset:UTF-8");

                  if (!ec)
                    args.push_back ("/execution-charset:UTF-8");
                }
              }

              if (cvariant != "clang" && isystem (*this))
              {
                if (find_option_prefixes ({"/external:I", "-external:I"}, args) &&
                    !find_option_prefixes ({"/external:W", "-external:W"}, args))
                  args.push_back ("/external:W0");
              }

              if (x_lang == lang::cxx &&
                  !find_option_prefixes ({"/EH", "-EH"}, args))
                args.push_back ("/EHsc");

              if (!find_option_prefixes ({"/MD", "/MT", "-MD", "-MT"}, args))
                args.push_back ("/MD");

              args.push_back ("/E");
              // args.push_back ("/C"); // See above.

              msvc_sanitize_cl (args);

              append_lang_options (args, md); // Compile as.

              break;
            }
          case compiler_class::gcc:
            {
              append_options (args, cmode);
              append_sys_hdr_options (args);

              // Note: no append_diag_color_options() call since the
              // diagnostics is discarded.

              // See perform_update() for details on the choice of options.
              //
              if (!find_option_prefix ("-finput-charset=", args))
                args.push_back ("-finput-charset=UTF-8");

              if (ot == otype::s)
              {
                if (tclass == "linux" || tclass == "bsd")
                  args.push_back ("-fPIC");
              }

              if (ctype == compiler_type::clang && tsys == "win32-msvc")
              {
                if (!find_options ({"-nostdlib", "-nostartfiles"}, args))
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

              args.push_back ("-E");
              append_lang_options (args, md);

              // Options that trigger preprocessing of partially preprocessed
              // output are a bit of a compiler-specific voodoo.
              //
              if (ps)
              {
                switch (ctype)
                {
                case compiler_type::gcc:
                  {
                    // Note that only these two *plus* -x do the trick.
                    //
                    args.push_back ("-fpreprocessed");
                    args.push_back ("-fdirectives-only");
                    break;
                  }
                case compiler_type::clang:
                  {
                    // See below for details.
                    //
                    if (ctype == compiler_type::clang &&
                        cmaj >= (cvariant != "apple" ? 15 : 16))
                    {
                      if (find_options ({"-pedantic",  "-pedantic-errors",
                                         "-Wpedantic", "-Werror=pedantic"},
                                        args))
                      {
                        args.push_back ("-Wno-gnu-line-marker");
                      }
                    }

                    break;
                  }
                case compiler_type::msvc:
                case compiler_type::icc:
                  assert (false);
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
            // diagnostics (thus no need for diag_buffer).
            //
            pr = process (cpath,
                          args,
                          0, -1, -2,
                          nullptr, // CWD
                          env.empty () ? nullptr : env.data ());
          }

          // Use binary mode to obtain consistent positions.
          //
          ifdstream is (move (pr.in_ofd),
                        fdstream_mode::binary | fdstream_mode::skip);

          parser p;
          p.parse (is, path_name (*sp), tu, cid);

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
                fail << "modules support required by " << src <<
                  info << "consider enabling modules with "
                       << x << ".features.modules=true in root.build";
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
            }

            // If we were forced to reprocess, assume the checksum is not
            // accurate (parts of the translation unit could have been
            // #ifdef'ed out; see __build2_preprocess).
            //
            // Also, don't use the checksum for header units since it ignores
            // preprocessor directives and may therefore cause us to ignore a
            // change to an exported macro. @@ TODO: maybe we should add a
            // flag to the parser not to waste time calculating the checksum
            // in these cases.
            //
            return reprocess || ut == unit_type::module_header
              ? string ()
              : move (p.checksum);
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
          run_finish (args, pr, 2 /* verbosity */); // Throws.
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

      context& ctx (bs.ctx);
      const scope& rs (*bs.root_scope ());

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
      // match against an actual (extracted) module name.
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
      // For std.* modules we only accept non-fuzzy matches (think std.compat
      // vs some compat.mxx). And if such a module is unresolved, then we
      // assume it is pre-built and will be found by some other means (e.g.,
      // VC's IFCPATH).
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
        // the B) score described above. Zero signifies no match.
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

        // Here is another corner case, the module is async_simple:IOExecutor
        // and the file names are:
        //
        // IOExecutor.mxx
        // SimpleIOExecutor.mxx
        //
        // The above implementation treats the latter as better because
        // `Simple` in SimpleIOExecutor matches `simple` in async_simple. It's
        // unclear what we can do about it without potentially breaking other
        // legitimate cases (think Boost_Simple:IOExecutor). Maybe we could
        // boost the exact partition name match score, similar to the exact
        // module match, as some sort of a heuristics? Let's try.
        //
        if (fi == 0 && mi != 0 && m[mi - 1] == ':')
        {
          // Pretend we matched one short of the next module component. This
          // way AsyncSimpleIOExecutor.mxx would still be a better match.
          //
          while (--mi != 0 && m[mi - 1] != '.')
            ;

          msep = (mi != 0); // For uncount logic below.
          mi++; // One short.
        }

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

#if 0
      assert (match ("IOExecutor",       "async_simple:IOExecutor") >
              match ("SimpleIOExecutor", "async_simple:IOExecutor"));

      assert (match ("IOExecutor",            "async_simple:IOExecutor") <
              match ("AsyncSimpleIOExecutor", "async_simple:IOExecutor"));

      assert (match ("IOExecutor",       "x.async_simple:IOExecutor") >
              match ("SimpleIOExecutor", "x.async_simple:IOExecutor"));

      assert (match ("IOExecutor",            "x.async_simple:IOExecutor") <
              match ("AsyncSimpleIOExecutor", "x.async_simple:IOExecutor"));
#endif

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
      // implicit import, if you will). Do you see where it's going? Nowhere
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
      //    bmi{}s at the back and store (in the target's auxiliary data
      //    storage) the start position. One bad aspect about this part is
      //    that we assume those bmi{}s have been matched by the same
      //    rule. But let's not kid ourselves, there will be no other rule
      //    that matches bmi{}s.
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
      // @@ Note: no longer the case for Clang either.
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
          else
            assert (name != m.name); // No duplicates.

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
            // If the extraction of the module information for this BMI failed
            // and we have deferred failure to compiler diagnostics, then
            // there will be no module name assigned. It would have been
            // better to make sure that's the cause, but that won't be easy.
            //
            const string* n (cast_null<string> (
                               pt->state[a].vars[c_module_name]));
            if (n != nullptr)
            {
              if (const target** p = check_exact (*n))
                *p = pt;
            }
          }
          else if (pt->is_a (*x_mod))
          {
            // This is an installed library with a list of module sources (the
            // source are specified as prerequisites but the fallback file
            // rule puts them into prerequisite_targets for us).
            //
            // The module names should be specified but if not assume
            // something else is going on (like a deferred failure) and
            // ignore.
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
            {
              // It seems natural to build a BMI type that corresponds to the
              // library type. After all, this is where the object file part
              // of the BMI is going to come from (unless it's a module
              // interface-only library).
              //
              *p = &this->make_module_sidebuild (
                a, bs, &l, link_type (l).type, *pt, *n).first; // GCC 4.9
            }
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

      // Pre-resolve standard library modules (std and std.compat) in an ad
      // hoc way.
      //

      // Similar logic to check_exact() above.
      //
      done = true;

      for (size_t i (0); i != n; ++i)
      {
        module_import& m (imports[i]);

        if (m.name == "std" || m.name == "std.compat")
        {
          otype ot (otype::e);
          const target* mt (nullptr);

          switch (ctype)
          {
          case compiler_type::clang:
            {
              // @@ TODO: cache x_stdlib value.
              //
              if (cast<string> (rs[x_stdlib]) != "libc++")
                fail << "standard library module '" << m.name << "' is "
                     << "currently only supported in libc++" <<
                  info << "try adding -stdlib=libc++ as compiler mode option";

              if (cmaj < 18)
                fail << "standard library module '" << m.name << "' is "
                     << "only supported in Clang 18 or later";

              // Find or insert std*.cppm (similar code to pkgconfig.cxx).
              //
              // Note: build_install_data is absolute and normalized.
              //
              mt = &ctx.targets.insert_locked (
                *x_mod,
                (dir_path (build_install_data) /= "libbuild2") /= "cc",
                dir_path (),
                m.name,
                string ("cppm"), // For C++14 during bootstrap.
                target_decl::implied,
                trace).first;

              // Which output type should we use, static or shared? The
              // correct way would be to detect whether static or shared
              // version of libc++ is to be linked and use the corresponding
              // type. And we could do that by looking for -static-libstdc++
              // in loption (and no, it's not -static-libc++).
              //
              // But, looking at the object file produced from std*.cppm, they
              // only contain one symbol, the static object initializer. And
              // this is unlikely to change since all other non-inline or
              // template symbols should be in libc++. So feels like it's not
              // worth the trouble and one variant should be good enough for
              // both cases. Let's use the shared one for less surprising
              // diagnostics (as in, "why are you linking obje{} to a shared
              // library?")
              //
              // (Of course, theoretically, std*.cppm could detect via a macro
              // whether they are being compiled with -fPIC or not and do
              // things differently, but this seems far-fetched).
              //
              ot = otype::s;

              break;
            }
          case compiler_type::msvc:
            {
              // For MSVC, the source files std.ixx and std.compat.ixx are
              // found in the modules/ subdirectory which is a sibling of
              // include/ in the MSVC toolset (and "that is a contract with
              // customers" to quote one of the developers).
              //
              // The problem of course is that there are multiple system
              // header search directories (for example, as specified in the
              // INCLUDE environment variable) and which one of them is for
              // the MSVC toolset is not specified. So what we are going to do
              // is search for one of the well-known standard C++ headers and
              // assume that the directory where we found it is the one we are
              // looking for. Or we could look for something MSVC-specific
              // like vcruntime.h.
              //
              dir_path modules;
              if (optional<path> p = find_system_header (path ("vcruntime.h")))
              {
                p->make_directory (); // Strip vcruntime.h.
                if (p->leaf () == path ("include")) // Sanity check.
                {
                  modules = path_cast<dir_path> (move (p->make_directory ()));
                  modules /= "modules";
                }
              }

              if (modules.empty ())
                fail << "unable to locate MSVC standard modules directory";

              mt = &ctx.targets.insert_locked (
                *x_mod,
                move (modules),
                dir_path (),
                m.name,
                string ("ixx"), // For C++14 during bootstrap.
                target_decl::implied,
                trace).first;

              // For MSVC it's easier to detect the runtime being used since
              // it's specified with the compile options (/MT[d], /MD[d]).
              //
              // Similar semantics as in extract_headers() except here we use
              // options visible from the root scope. Note that
              // find_option_prefixes() looks in reverse, so look in the
              // cmode, x_coptions, c_coptions order.
              //
              initializer_list<const char*> os {"/MD", "/MT", "-MD", "-MT"};

              const string* o;
              if ((o = find_option_prefixes (os, cmode))          != nullptr ||
                  (o = find_option_prefixes (os, rs, x_coptions)) != nullptr ||
                  (o = find_option_prefixes (os, rs, c_coptions)) != nullptr)
              {
                ot = (*o)[2] == 'D' ? otype::s : otype::a;
              }
              else
                ot = otype::s; // The default is /MD.

              break;
            }
          case compiler_type::gcc:
          case compiler_type::icc:
            {
              fail << "standard library module '" << m.name << "' is "
                   << "not yet supported in this compiler";
            }
          };

          pair<target&, ulock> tl (
            this->make_module_sidebuild ( // GCC 4.9
              a, bs, nullptr, ot, *mt, m.name));

          if (tl.second.owns_lock ())
          {
            // Special compile options for the std modules.
            //
            if (ctype == compiler_type::clang)
            {
              value& v (tl.first.append_locked (x_coptions));

              if (v.null)
                v = strings {};

              strings& cops (v.as<strings> ());

              switch (ctype)
              {
              case compiler_type::clang:
                {
                  cops.push_back ("-Wno-reserved-module-identifier");
                  break;
                }
              case compiler_type::msvc:
                // It appears nothing special is needed to compile MSVC
                // standard modules.
              case compiler_type::gcc:
              case compiler_type::icc:
                assert (false);
              };
            }

            tl.second.unlock ();
          }

          pts[start + i].target = &tl.first;
          m.score = match_max (m.name) + 1;
          continue; // Scan the rest to detect if all done.
        }

        done = false;
      }

      // Go over prerequisites and try to resolve imported modules with them.
      //
      if (!done)
      {
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
            else if (pt->is_a<liba> () ||
                     pt->is_a<libs> () ||
                     pt->is_a<libux> ())
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

          // While it would have been even better not to search for a target,
          // we need to get hold of the corresponding mxx{} (unlikely but
          // possible for bmi{} to have a different name).
          //
          // While we want to use group_prerequisite_members() below, we
          // cannot call resolve_group() since we will be doing it
          // "speculatively" for modules that we may use but also for modules
          // that may use us. This quickly leads to deadlocks. So instead we
          // are going to perform an ad hoc group resolution.
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
              const target* mt (p.search_existing ());
              const string* n (mt != nullptr
                               ? cast_null<string> (mt->vars[c_module_name])
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
          // As above (deffered failure).
          //
          const string* mn (
            cast_null<string> (bt->state[a].vars[c_module_name]));

          if (mn != nullptr && in != *mn)
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
                  info << "actual:  " << *mn <<
                  info << "consider adjusting module interface file names or" <<
                  info << "consider specifying module name with " << x
                  << ".module_name";
              }
            }
          }
        }

        // Hash (we know it's a file).
        //
        cs.append (bt->as<file> ().path ().string ());

        // Copy over bmi{}s from our prerequisites weeding out duplicates.
        //
        if (size_t j = bt->data<match_data> (a).modules.start)
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

            // As above (deferred failure).
            //
            const string* mn (cast_null<string> (et->state[a].vars[c_module_name]));

            if (mn != nullptr &&
                find_if (imports.begin (), imports.end (),
                         [mn] (const module_import& i)
                         {
                           return i.name == *mn;
                         }) == imports.end ())
            {
              pts.push_back (et);
              cs.append (et->as<file> ().path ().string ());

              // Add to the list of imports for further duplicate suppression.
              // We could have stored reference to the name (e.g., in score)
              // but it's probably not worth it if we have a small string
              // optimization.
              //
              import_type t (mn->find (':') != string::npos
                             ? import_type::module_part
                             : import_type::module_intf);
              imports.push_back (module_import {t, *mn, true, 0});
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
    // @@ Could we omit creating a subproject if the sidebuild scope is the
    //    project scope itself? This would speed up simple examples (and
    //    potentially direct compilation that we may support).
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
      // @@ TODO: maybe we should cache this in compile_rule ctor like we
      //          do for the header cache?
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
            if (cast_false<bool> (s->vars["cc.core.vars.loaded"]))
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

            // @@ What happens if different projects used different standards?
            //    Specifically, how do we detect this and what can the user do
            //    about it? For the latter question, forcing the same standard
            //    with config.cxx.std seems like the only sensible option. For
            //    the former, we could read the value of cxx.std using our
            //    buildfile first-line peeking mechanism. But doing that for
            //    every module interface feels inefficient so we will probably
            //    need to cache it on the per-project basis. Maybe/later.
            //
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

    // Synthesize a dependency for building a module binary interface of a
    // library on the side. If library is missing, then assume it's some
    // ad hoc/system library case (in which case we assume it's binless,
    // for now).
    //
    // The return value semantics is as in target_set::insert_locked().
    //
    pair<target&, ulock> compile_rule::
    make_module_sidebuild (action a,
                           const scope& bs,
                           const file* lt,
                           otype ot,
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

      const target_type& tt (compile_types (ot).bmi);

      // Store the BMI target in the subproject root. If the target already
      // exists then we assume all this is already done (otherwise why would
      // someone have created such a target).
      //
      if (const target* bt = bs.ctx.targets.find (
            tt,
            pd,
            dir_path (), // Always in the out tree.
            mf,
            nullopt,     // Use default extension.
            trace))
        return pair<target&, ulock> (const_cast<target&> (*bt), ulock ());

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
      if (lt != nullptr)
      {
        ps.push_back (prerequisite (*lt));
        for (prerequisite_member p: group_prerequisite_members (a, *lt))
        {
          // Ignore update=match.
          //
          lookup l;
          if (include (a, *lt, p, &l) != include_type::normal) // Excluded/ad hoc.
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
                trace,
                true /* skip_find */));

      // Note that this is racy and someone might have created this target
      // while we were preparing the prerequisite list.
      //
      if (p.second)
      {
        p.first.prerequisites (move (ps));

        // Unless this is a binless library, we don't need the object file
        // (see config_data::b_binless for details).
        //
        p.first.vars.assign (b_binless) = (lt == nullptr ||
                                           lt->mtime () == timestamp_unreal);
      }

      return p;
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
        appended_libraries ls;
        struct data
        {
          action              a;
          const file&         ht;
          const target*&      lt;
          appended_libraries& ls;
        } d {a, ht, lt, ls};

        auto lib = [&d] (
          const target* const* lc,
          const small_vector<reference_wrapper<const string>, 2>&,
          lflags,
          const string*,
          bool)
        {
          // Prune any further traversal if we already found it.
          //
          if (d.lt != nullptr)
            return false;

          const target* l (lc != nullptr ? *lc : nullptr); // Can be lib{}.

          if (l == nullptr)
            return true;

          // Suppress duplicates.
          //
          if (find (d.ls.begin (), d.ls.end (), l) != d.ls.end ())
            return false;

          // Feels like we should only consider non-utility libraries with
          // utilities being treated as "direct" use.
          //
          if (l->is_a<libux> ())
            return true;

          // Since the library is searched and matched, all the headers should
          // be in prerequisite_targets.
          //
          const auto& pts (l->prerequisite_targets[d.a]);
          if (find (pts.begin (), pts.end (), &d.ht) != pts.end ())
          {
            d.lt = l;
            return false;
          }

          d.ls.push_back (l);
          return true;
        };

        library_cache lib_cache;
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
              process_libraries (a, bs, nullopt, sys_lib_dirs,
                                 *f, la, 0, // lflags unused.
                                 imp, lib, nullptr,
                                 true /* self */,
                                 false /* proc_opt_group */,
                                 &lib_cache);

              if (lt != nullptr)
                break;
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
          // Ignore update=match.
          //
          lookup l;
          if (include (a, t, p, &l) != include_type::normal) // Excluded/ad hoc.
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
                trace,
                true /* skip_find */));
      file& bt (p.first.as<file> ());

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
    msvc_filter_cl (diag_buffer&, const path& src);

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
    append_module_options (environment&,
                           cstrings& args,
                           small_vector<string, 2>& stor,
                           action a,
                           const file& t,
                           const match_data& md,
                           const path& dd) const
    {
      unit_type ut (md.type);
      const module_positions& ms (md.modules);

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

          // If/when we get the ability to specify the mapping in a file.
          //
#if 0
          // In Clang the module implementation's unit .pcm is special and
          // must be "loaded". Note: not anymore, not from Clang 16 and is
          // deprecated in 17.
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
          for (size_t i (ms.start), n (pts.size ()); i != n; ++i)
          {
            const target* pt (pts[i]);

            if (pt == nullptr)
              continue;

            // Here we use whatever bmi type has been added. And we know all
            // of these are bmi's.
            //
            const file& f (pt->as<file> ());
            string s (relative (f.path ()).string ());

            s.insert (0, 1, '=');
            s.insert (0, cast<string> (f.state[a].vars[c_module_name]));
            s.insert (0, "-fmodule-file=");

            stor.push_back (move (s));
          }
#endif
          break;
        }
      case compiler_type::msvc:
        {
          if (ms.start == 0)
            return;

          // MSVC requires a transitive set of interfaces, including
          // implementation partitions.
          //
          auto& pts (t.prerequisite_targets[a]);
          for (size_t i (ms.start), n (pts.size ()); i != n; ++i)
          {
            const target* pt (pts[i]);

            if (pt == nullptr)
              continue;

            // Here we use whatever bmi type has been added. And we know all
            // of these are bmi's.
            //
            const file& f (pt->as<file> ());
            string s (relative (f.path ()).string ());

            s.insert (0, 1, '=');
            s.insert (0, cast<string> (f.state[a].vars[c_module_name]));

            stor.push_back (move (s));
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
      {
        if (ctype == compiler_type::msvc)
          args.push_back ("/reference");

        args.push_back (a.c_str ());
      }
    }

    target_state compile_rule::
    perform_update (action a, const target& xt, match_data& md) const
    {
      const file& t (xt.as<file> ());
      const path& tp (t.path ());

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

      const file& s (pr.second);
      const path* sp (&s.path ());

      // Make sure depdb is no older than any of our prerequisites (see md.mt
      // logic description above for details). Also save the sequence start
      // time if doing mtime checks (see the depdb::check_mtime() call below).
      //
      timestamp start (!ctx.dry_run && depdb::mtime_check ()
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
      // apply()). For named modules there may be no obj*{} if this is a
      // sidebuild (obj*{} is already in the library binary).
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

          // Note: could be overridden in mode.
          //
          append_diag_color_options (args);

          // Set source/execution charsets to UTF-8 unless a custom charset
          // is specified.
          //
          // Note that clang-cl supports /utf-8 and /*-charset.
          //
          {
            bool sc (find_option_prefixes (
                       {"/source-charset:", "-source-charset:"}, args));
            bool ec (find_option_prefixes (
                       {"/execution-charset:", "-execution-charset:"}, args));

            if (!sc && !ec)
              args.push_back ("/utf-8");
            else
            {
              if (!sc)
                args.push_back ("/source-charset:UTF-8");

              if (!ec)
                args.push_back ("/execution-charset:UTF-8");
            }
          }

          // If we have any /external:I options but no /external:Wn, then add
          // /external:W0 to emulate the -isystem semantics.
          //
          if (cvariant != "clang" && isystem (*this))
          {
            if (find_option_prefixes ({"/external:I", "-external:I"}, args) &&
                !find_option_prefixes ({"/external:W", "-external:W"}, args))
              args.push_back ("/external:W0");
          }

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

          if (x_lang == lang::cxx &&
              !find_option_prefixes ({"/EH", "-EH"}, args))
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
          if (!find_option_prefixes ({"/MD", "/MT", "-MD", "-MT"}, args))
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
          if (!relo.empty () &&
              find_options ({"/Zi", "/ZI", "-Zi", "-ZI"}, args))
          {
            if (fc)
              args.push_back ("/Fd:");
            else
              out1 = "/Fd";

            out1 += relo.string ();
            out1 += ".pdb";

            args.push_back (out1.c_str ());
          }

          if (ut == unit_type::module_intf      ||
              ut == unit_type::module_intf_part ||
              ut == unit_type::module_impl_part ||
              ut == unit_type::module_header)
          {
            assert (ut != unit_type::module_header); // @@ MODHDR

            relm = relative (tp);

            args.push_back ("/ifcOutput");
            args.push_back (relm.string ().c_str ());

            if (relo.empty ())
              args.push_back ("/ifcOnly");
            else
            {
              args.push_back ("/Fo:");
              args.push_back (relo.string ().c_str ());
            }
          }
          else
          {
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
          }

          // Note: no way to indicate that the source if already preprocessed.

          args.push_back ("/c");                   // Compile only.
          append_lang_options (args, md);          // Compile as.
          args.push_back (sp->string ().c_str ()); // Note: relied on being last.

          break;
        }
      case compiler_class::gcc:
        {
          append_options (args, cmode);

          // Clang 15 introduced the unqualified-std-cast-call warning which
          // warns about unqualified calls to std::move() and std::forward()
          // (because they can be "hijacked" via ADL). Surprisingly, this
          // warning is enabled by default, as opposed to with -Wextra or at
          // least -Wall. It has also proven to be quite disruptive, causing a
          // large number of warnings in a large number of packages. So we are
          // going to "remap" it to -Wextra for now and in the future may
          // "relax" it to -Wall and potentially to being enabled by default.
          // See GitHub issue #259 for background and details.
          //
          if (x_lang == lang::cxx           &&
              ctype == compiler_type::clang &&
              cmaj >= 15)
          {
            bool w (false);       // Seen -W[no-]unqualified-std-cast-call
            optional<bool> extra; // Seen -W[no-]extra

            for (const char* s: reverse_iterate (args))
            {
              if (s != nullptr)
              {
                if (strcmp (s, "-Wunqualified-std-cast-call") == 0 ||
                    strcmp (s, "-Wno-unqualified-std-cast-call") == 0)
                {
                  w = true;
                  break;
                }

                if (!extra) // Last seen option wins.
                {
                  if      (strcmp (s, "-Wextra") == 0) extra = true;
                  else if (strcmp (s, "-Wno-extra") == 0) extra = false;
                }
              }
            }

            if (!w && (!extra || !*extra))
              args.push_back ("-Wno-unqualified-std-cast-call");
          }

          if (md.pp != preprocessed::all)
            append_sys_hdr_options (args); // Extra system header dirs (last).

          // Note: could be overridden in mode.
          //
          append_diag_color_options (args);

          // Set the input charset to UTF-8 unless a custom one is specified.
          //
          // Note that the execution charset (-fexec-charset) is UTF-8 by
          // default.
          //
          // Note that early versions of Clang only recognize uppercase UTF-8.
          //
          if (!find_option_prefix ("-finput-charset=", args))
            args.push_back ("-finput-charset=UTF-8");

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
                if (!find_options ({"-nostdlib", "-nostartfiles"}, args))
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

          append_header_options (env, args, header_args, a, t, md, md.dd);
          append_module_options (env, args, module_args, a, t, md, md.dd);

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
                assert (ut != unit_type::module_header); // @@ MODHDR

                relm = relative (tp);

                // Without this option Clang's .pcm will reference source
                // files. In our case this file may be transient (.ii). Plus,
                // it won't play nice with distributed compilation.
                //
                // Note that this sort of appears to be the default from Clang
                // 17, but not quite, see llvm-project issued #72383.
                //
                args.push_back ("-Xclang");
                args.push_back ("-fmodules-embed-all-files");

                if (relo.empty ())
                {
                  args.push_back ("-o");
                  args.push_back (relm.string ().c_str ());
                  args.push_back ("--precompile");
                }
                else
                {
                  out1 = "-fmodule-output=" + relm.string ();
                  args.push_back (out1.c_str ());
                  args.push_back ("-o");
                  args.push_back (relo.string ().c_str ());
                  args.push_back ("-c");
                }

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

          append_lang_options (args, md);

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

      // We have no choice but to serialize early if we want the command line
      // printed shortly before actually executing the compiler. Failed that,
      // it may look like we are still executing in parallel.
      //
      scheduler::alloc_guard jobs_ag;
      if (!ctx.dry_run && cast_false<bool> (t[c_serialize]))
        jobs_ag = scheduler::alloc_guard (*ctx.sched, phase_unlock (nullptr));

      // With verbosity level 2 print the command line as if we are compiling
      // the source file, not its preprocessed version (so that it's easy to
      // copy and re-run, etc). Only at level 3 and above print the real deal.
      //
      // @@ TODO: why don't we print env (here and/or below)? Also link rule.
      //
      if (verb == 1)
      {
        const char* name (x_assembler_cpp (s) ? "as-cpp"   :
                          x_objective (s)     ? x_obj_name :
                          x_name);

        print_diag (name, s, t);
      }
      else if (verb == 2)
        print_process (args);

      // If we have the (partially) preprocessed output, switch to that.
      //
      // But we remember the original source/position to restore later.
      //
      bool psrc (md.psrc); // Note: false if cc.reprocess.
      bool ptmp (psrc && md.psrc.temporary);
      pair<size_t, const char*> osrc;
      if (psrc)
      {
        args.pop_back (); // nullptr
        osrc.second = args.back ();
        args.pop_back (); // sp
        osrc.first = args.size ();

        sp = &md.psrc.path ();

        // This should match with how we setup preprocessing.
        //
        switch (ctype)
        {
        case compiler_type::gcc:
          {
            // -fpreprocessed is implied by .i/.ii unless compiling a header
            // unit (there is no .hi/.hii). Also, we would need to pop -x
            // since it takes precedence over the extension, which would mess
            // up our osrc logic. So in the end it feels like always passing
            // explicit -fpreprocessed is the way to go.
            //
            // Also note that similarly there is no .Si for .S files.
            //
            args.push_back ("-fpreprocessed");
            args.push_back ("-fdirectives-only");
            break;
          }
        case compiler_type::clang:
          {
            // Clang 15 and later with -pedantic warns about GNU-style line
            // markers that it wrote itself in the -frewrite-includes output
            // (llvm-project issue 63284). So we suppress this warning unless
            // compiling from source.
            //
            // In Apple Clang this warning/option are absent in 14.0.3 (which
            // is said to be based on vanilla Clang 15.0.5) for some reason
            // (let's hope it's because they patched it out rather than due to
            // a misleading _LIBCPP_VERSION value).
            //
            if (ctype == compiler_type::clang &&
                cmaj >= (cvariant != "apple" ? 15 : 16))
            {
              if (find_options ({"-pedantic",  "-pedantic-errors",
                                 "-Wpedantic", "-Werror=pedantic"}, args))
              {
                args.push_back ("-Wno-gnu-line-marker");
              }
            }

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
          // name being compiled as the first line. So for cl.exe we filter
          // that noise out.
          //
          // For other compilers also redirect stdout to stderr, in case any
          // of them tries to pull off something similar. For sane compilers
          // this should be harmless.
          //
          bool filter (ctype == compiler_type::msvc);

          process pr (cpath,
                      args,
                      0, 2, diag_buffer::pipe (ctx, filter /* force */),
                      nullptr, // CWD
                      env.empty () ? nullptr : env.data ());

          diag_buffer dbuf (ctx, args[0], pr);

          if (filter)
            msvc_filter_cl (dbuf, *sp);

          dbuf.read ();

          // Restore the original source if we switched to preprocessed.
          //
          if (psrc)
          {
            args.resize (osrc.first);
            args.push_back (osrc.second);
            args.push_back (nullptr);
          }

          run_finish (dbuf, args, pr, 1 /* verbosity */);
        }
        catch (const process_error& e)
        {
          error << "unable to execute " << args[0] << ": " << e;

          if (e.child)
            exit (1);

          throw failed ();
        }

        jobs_ag.deallocate ();

        if (md.deferred_failure)
          fail << "expected error exit status from " << x_lang << " compiler";
      }

      // Remove preprocessed file (see above).
      //
      if (ptmp && verb >= 3)
        md.psrc.temporary = true;

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
    perform_clean (action a, const target& xt, const target_type& srct) const
    {
      const file& t (xt.as<file> ());

      // Preprocessed file extension.
      //
      const char* pext (x_assembler_cpp (srct) ? ".Si"      :
                        x_objective (srct)     ? x_obj_pext :
                        x_pext);

      // Compressed preprocessed file extension.
      //
      string cpext (t.ctx.fcache->compressed_extension (pext));

      clean_extras extras;
      switch (ctype)
      {
      case compiler_type::gcc:   extras = {".d", pext, cpext.c_str (), ".t"};           break;
      case compiler_type::clang: extras = {".d", pext, cpext.c_str ()};                 break;
      case compiler_type::msvc:  extras = {".d", pext, cpext.c_str (), ".idb", ".pdb"}; break;
      case compiler_type::icc:   extras = {".d"};                                       break;
      }

      return perform_clean_extra (a, t, extras);
    }
  }
}
