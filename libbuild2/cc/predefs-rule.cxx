// file      : libbuild2/cc/predefs-rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/predefs-rule.hxx>

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/serializer.hxx>
#endif

#include <libbuild2/json.hxx>
#include <libbuild2/depdb.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/parser.hxx> // parser::verify_variable_name().
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/make-parser.hxx>

#include <libbuild2/cc/compile-rule.hxx>

namespace build2
{
  namespace cc
  {
    using macro_name_map = map<string, optional<string>>;

    enum class output_type {header, json, buildfile};

    struct predefs_rule::match_data
    {
      depdb::reopen_state dd;
      size_t skip_count;
      size_t pts_n; // Number of static prerequisites in prerequisite_targets.

      const scope& bs;
      const file* src;
      timestamp mt;

      const path& tp;
      output_type ot;
      bool poptions;
      const string* def_val;
      const macro_name_map* mmap;

      const predefs_rule& rule;

      target_state
      operator() (action a, const target& t)
      {
        return rule.perform_update (a, t, *this);
      }
    };

    predefs_rule::
    predefs_rule (data&& d, const compile_rule& cr)
        : common (move (d)),
          rule_name (string (x) += ".predefs"),
          rule_id (rule_name + " 1"),
          c_rule (cr)
    {
    }

    bool predefs_rule::
    match (action a, target& t, const string& hint, match_extra&) const
    {
      tracer trace (x, "predefs_rule::match");

      // We only match with an explicit hint (failed that, we will turn every
      // header into predefs). Likewise for buildfile{} and json{} output --
      // the dependency (if there is any) is probably too generic.
      //
      // Note also that we only expect to be registered for the header,
      // json{}, and buildscript{} target types.
      //
      if (hint != rule_name)
        return false;

      // Don't match if unsupported compiler. In particular, this allows the
      // user to provide a fallback rule.
      //
      switch (cclass)
      {
      case compiler_class::gcc:
        {
          // For Clang until version 12 we only support pure predefs (see
          // perform_update() below for details).
          //
          if (ctype == compiler_type::clang && cmaj < 12)
          {
            for (prerequisite_member p: group_prerequisite_members (a, t))
            {
              // If excluded or ad hoc, then don't factor it into our tests.
              //
              if (include (a, t, p) != include_type::normal)
                continue;

              if (p.is_a (*x_hdrs[0]) || p.is_a<h> ())
              {
                l4 ([&]{trace << "unsupported compiler/version";});
                return false;
              }
            }
          }

          break;
        }
      case compiler_class::msvc:
        {
          // Only MSVC 19.20 or later. Not tested with clang-cl.
          //
          if (cvariant.empty () && (cmaj > 19 || (cmaj == 19 && cmin >= 20)))
            ;
          else
          {
            l4 ([&]{trace << "unsupported compiler/version";});
            return false;
          }
        }
      }

      return true;
    }

    recipe predefs_rule::
    apply (action a, target& xt, match_extra&) const
    {
      tracer trace (x, "predefs_rule::apply");

      file& t (xt.as<file> ());

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      const path& tp (t.derive_path ());

      // Inject dependency on the output directory.
      //
      const fsdir* dir (inject_fsdir (a, t));

      // Match prerequisites.
      //
      match_prerequisite_members (a, t);

      if (a == perform_update_id)
      {
        auto& pts (t.prerequisite_targets[a]);

        // See if we preprocess a user-supplied header or an empty translation
        // unit (pure predefs). We assume the first header, if any, is the
        // one.
        //
        const file* s (nullptr);
        for (const prerequisite_target& p: pts)
        {
          if (const target* pt = p.target)
          {
            // Note: allow using C header as input for the C++ rule.
            //
            if (!p.adhoc () && (pt->is_a (*x_hdrs[0]) || pt->is_a<h> ()))
            {
              s = &pt->as<file> ();
              break;
            }
          }
        }

        const path& sp (s != nullptr ? s->path () : empty_path);

        // Determine what we are producing.
        //
        output_type ot;
        if      (t.is_a (*x_hdrs[0]))  ot = output_type::header;
        else if (t.is_a<json> ())      ot = output_type::json;
        else if (t.is_a<buildfile> ()) ot = output_type::buildfile;
        else assert (false);

        // Note that any command line macros that we specify with -D will end
        // up in the predefs, which is something we usually don't want for
        // pure predefs and may or may not want when preprocessing a
        // user-specified file (see cc.predefs.poptions documentation for
        // details).
        //
        bool poptions;
        if (lookup l = t[c_predefs_poptions])
        {
          poptions = cast<bool> (l);
        }
        else if (s == nullptr)
          poptions = false;
        else
          fail << "explicit " << x << ".predefs.poptions must be specified for "
               << t << endf;

        const string* def_val (cast_null<string> (t[c_predefs_default]));

        const macro_name_map* mmap (
          cast_null<macro_name_map> (t[c_predefs_macros]));

        if (mmap == nullptr && ot == output_type::buildfile)
          fail << "explicit " << x << ".predefs.macros must be specified for "
               << t;

        // Make sure the output directory exists (so we have a place to create
        // depdb).
        //
        if (dir != nullptr)
          fsdir_rule::perform_update_direct (a, *dir);

        // Use depdb to track changes to options, compiler, etc (similar to
        // the compile_rule).
        //
        // Note: the below logic is similar to the compile rule except that we
        // extract dependencies as a byproduct of preprocessing, which is more
        // similar to depdb --byproduct logic in ad hoc buildscript recipes.
        // So use both of these as a reference.
        //
        depdb dd (tp + ".d");

        // First should come the rule name/version.
        //
        if (dd.expect (rule_id) != nullptr)
          l4 ([&]{trace << "rule mismatch forcing update of " << t;});

        // Then the compiler checksum.
        //
        if (dd.expect (cast<string> (rs[x_checksum])) != nullptr)
          l4 ([&]{trace << "compiler mismatch forcing update of " << t;});

        // Then the compiler environment checksum.
        //
        if (dd.expect (env_checksum) != nullptr)
          l4 ([&]{trace << "environment mismatch forcing update of " << t;});

        // Then the options checksum (as below).
        //
        {
          sha256 cs;

          if (poptions)
          {
            append_options (cs, t, x_poptions);
            append_options (cs, t, c_poptions);
          }
          append_options (cs, t, c_coptions);
          append_options (cs, t, x_coptions);
          append_options (cs, cmode);

          if (dd.expect (cs.string ()) != nullptr)
            l4 ([&]{trace << "options mismatch forcing update of " << t;});
        }

        // Then the default macro value.
        //
        if (dd.expect (def_val != nullptr ? def_val->c_str () : "1") != nullptr)
          l4 ([&]{trace << "default macro value mismatch forcing update of "
                        << t;});

        // Then the macro map checksum.
        //
        {
          sha256 cs;

          if (mmap != nullptr)
          {
            for (const auto& p: *mmap)
            {
              cs.append (p.first);
              if (p.second)
                cs.append (*p.second);
            }
          }

          if (dd.expect (cs.string ()) != nullptr)
            l4 ([&]{trace << "macro list/mapping mismatch forcing update of "
                          << t;});
        }

        // Finally the source file.
        //
        // Note that we write an entry even if there is no source file (pure
        // predefs) for regularity.
        //
        if (dd.expect (sp) != nullptr)
          l4 ([&]{trace << "source file mismatch forcing update of " << t;});

        // Determine if we need to do an update based on the above checks.
        //
        bool update (false);
        timestamp mt;

        if (dd.writing ())
          update = true;
        else
        {
          if ((mt = t.mtime ()) == timestamp_unknown)
            t.mtime (mt = mtime (tp)); // Cache.

          update = dd.mtime > mt;
        }

        // If updating for any of the above reasons, treat it as if doesn't
        // exist.
        //
        if (update)
          mt = timestamp_nonexistent;

        // Update prerequisite targets (normally just the source file).
        //
        using dyndep = dyndep_rule;

        for (prerequisite_target& p: pts)
        {
          const target* pt (p.target);

          if (pt == nullptr || pt == dir)
            continue;

          update = dyndep::update (
            trace, a, *pt, update ? timestamp_unknown : mt) || update;

          p.include |= prerequisite_target::include_udm;
        }

        match_data md {
          {},
          0, pts.size (), // Number of static prerequisites.
          bs, s, timestamp_unknown,
          tp, ot, poptions, def_val, mmap,
          *this};

        // Unless we are already updating, verify the entries (extracted
        // header dependencies) in depdb. This is the `if(cache)` part of the
        // logic.
        //
        if (!update)
        {
          // Note that we have to update each header for the same reason as
          // the main source file -- if any of them changed, then we must
          // assume the subsequent entries are invalid.
          //
          auto add = [this, a, &bs, &t, &md, mt] (path fp) -> optional<bool>
          {
            // Reuse compile_rule::enter/inject_header() instead of generic
            // dyndep::enter/inject_file()
            //
            // Disable prefix and srcout mapping (only applies to generated
            // headers).
            //
            optional<prefix_map> pfx_map (prefix_map {});
            srcout_map so_map;

            if (const build2::file* ft = c_rule.enter_header (
                  a, bs, t,
                  linfo {} /* unused (since passing pfx_map) */,
                  move (fp), true /* cache */, true /* normalized */,
                  pfx_map, so_map).first)
            {
              if (optional<bool> u = c_rule.inject_existing_header (
                    a, t, md.pts_n,
                    *ft, mt,
                    false /* fail */))
              {
                md.skip_count++;
                return *u;
              }
            }

            return nullopt;
          };

          auto df = make_diag_frame (
            [&t, s] (const diag_record& dr)
            {
              if (verb != 0)
              {
                dr << info << "while extracting header dependencies ";
                if (s != nullptr)
                  dr << "from " << *s;
                else
                  dr << "for " << t;
              }
            });

          while (!update)
          {
            // We should always end with a blank line.
            //
            string* l (dd.read ());

            // If the line is invalid, run the compiler.
            //
            if (l == nullptr)
            {
              update = true;
              break;
            }

            if (l->empty ()) // Done, nothing changed.
              break;

            // Note: the path is absolute and normalized.
            //
            if (optional<bool> r = add (path (move (*l))))
            {
              if (*r)
                update = true;
            }
            else
            {
              // Invalidate this line and trigger update.
              //
              dd.write ();
              update = true;
            }

            if (update)
              l4 ([&]{trace << "outdated extracted header dependencies "
                            << "forcing update of " << t;});
          }
        }

        // Note that in case of a dry run we will have an incomplete (but
        // valid) database which will be updated on the next non-dry
        // run. Except that we may still end up performing a non-dry-run
        // update due to update during match or load.
        //
        if (!update /*|| ctx.dry_run_option*/)
          dd.close (false /* mtime_check */);
        else
          md.dd = dd.close_to_reopen ();

        // Pass on update/mtime.
        //
        md.mt = update ? timestamp_nonexistent : mt;

        return md;
      }
      else if (a == perform_clean_id)
      {
        return [] (action a, const target& t)
        {
          // Also remove the temporary input source file in case it wasn't
          // removed at the end of the update.
          //
          // Note that we don't need to bother reading and injecting extracted
          // header dependencies from depdb since they can only be existing
          // files in the byproduct mode.
          //
          return perform_clean_extra (a, t.as<file> (), {".d", ".t"});
        };
      }
      else
        return noop_recipe; // Configure update.
    }

    void
    msvc_sanitize_cl (cstrings&); // Sanitize options (msvc.cxx).

    void predefs_rule::
    write_macro_buildfile (ofdstream& os,
                           const string& n, const json_value& v) const
    {
      // Verify the variable name is not reserved.
      //
      if (const char* w = parser::verify_variable_name (n))
        fail << "macro name '" << n << "' is reserved buildfile variable name" <<
          info << "variable " << w <<
          info << "use " << x << ".predefs.macros to assign it different name";

      os << n << " = ";

      switch (v.type)
      {
      case json_type::null:
        {
          os << "[null]";
          break;
        }
      case json_type::boolean:
        {
          os << "[bool] " << (v.as_bool () ? "true" : "false");
          break;
        }
      case json_type::signed_number:
        {
          os << "[int64] " << v.as_int64 ();
          break;
        }
      case json_type::unsigned_number:
        {
          os << "[uint64] " << v.as_uint64 ();
          break;
        }
      case json_type::hexadecimal_number:
        {
          os << "[uint64] " << build2::to_string (v.as_uint64 (), 16);
          break;
        }
      case json_type::string:
        {
          // We don't know what it is so let's save it untyped.
          //
          to_stream (os, name (v.as_string ()), quote_mode::normal, '@');
          break;
        }
      default:
        assert (false);
      }

      os << '\n';
    }

#ifndef BUILD2_BOOTSTRAP
    void predefs_rule::
    write_macro_json (json_buffer_serializer& js,
                      const string& n, const json_value& v) const
    {
      js.member_name (n);

      switch (v.type)
      {
      case json_type::null:               js.value (nullptr);       break;
      case json_type::boolean:            js.value (v.as_bool ());  break;
      case json_type::signed_number:      js.value (v.as_int64 ()); break;
      case json_type::unsigned_number:
      case json_type::hexadecimal_number: js.value (v.as_uint64 ()); break;
      case json_type::string:             js.value (v.as_string ()); break;
      default:                            assert (false);
      }
    }
#endif

    target_state predefs_rule::
    perform_update (action a, const target& xt, match_data& md) const
    {
      tracer trace (x, "predefs_rule::perform_update");

      const file& t (xt.as<file> ());
      const path& tp (md.tp);

      context& ctx (t.ctx);

      // Update prerequisites and determine if any render us out-of-date.
      // Actually, they were already updated in apply() but we still need to
      // do this to keep the dependency counts straight.
      //
      optional<target_state> ps (execute_prerequisites (a, t, md.mt));

      if (!ps)
        assert (md.mt == timestamp_nonexistent); // Otherwise no depdb state.

      if (md.mt != timestamp_nonexistent)
        return *ps;

      const file* s (md.src);
      const path& sp (s != nullptr ? s->path () : path ());

      bool poptions (md.poptions);
      output_type ot (md.ot);

      // Prepare the compiler command-line.
      //
      cstrings args {cpath.recall_string ()};

      // Append compile options.
      //
      if (poptions)
      {
        // @@ Note: in the compile rule we also do append_library_options().
        //    Maybe we will support this one day (also when hashing above).
        //
        append_options (args, t, x_poptions);
        append_options (args, t, c_poptions);
      }
      append_options (args, t, c_coptions);
      append_options (args, t, x_coptions);
      append_options (args, cmode);

      // The output and source paths, relative to the working directory for
      // easier to read diagnostics.
      //
      path relo (relative (tp));
      path rels (s != nullptr ? relative (sp) : path ());

      bool mhdr (false); // True if we are writing header manually.

      // Add compiler-specific command-line arguments.
      //
      switch (cclass)
      {
      case compiler_class::gcc:
        {
          // Add implied options which may affect predefs, similar to the
          // compile rule.
          //
          if (!find_option_prefix ("-finput-charset=", args))
            args.push_back ("-finput-charset=UTF-8");

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

          args.push_back ("-E");  // Stop after the preprocessing stage.
          args.push_back ("-dM"); // Generate #define directives.

          // Header dependency information.
          //
          // Note that we do this even for the pure predefs since GCC on Linux
          // implicitly includes /usr/include/stdc-predef.h (comes from libc).
          //
          // Note also that in case of the json/buildfile output, we write
          // both the output and this information to stdout. In both GCC and
          // Clang we get the macros first and the dependency information
          // second.
          //
          // Except that Clang until version 5 is unable to write both to the
          // same stream. And Clang until version 12 produces interleaved
          // output with the dependency information written in the middle of a
          // macro definition. So for now we only support Clang before version
          // 12 for pure predefs and without dependency information.
          //
          if (ctype == compiler_type::clang && cmaj < 12)
          {
            assert (s == nullptr); // Shouldn't have matched otherwise.
          }
          else
          {
            // GCC prior to version 8 did not support `-` as an -MF argument.
            // However, using -M and omitting -MF produces the same result in
            // this case (confirmed with GCC 4.9).
            //
            // There is just one snag: if we are writing directly into the
            // header (e.g., with -o predefs.h), then the dependency
            // information goes there as well. Seems like writing the header
            // manually (similar to the MSVC case below) is the least bad
            // workaround.
            //
            if (ctype == compiler_type::gcc && cmaj < 8)
            {
              args.push_back ("-M");

              if (ot == output_type::header)
                mhdr = true;
            }
            else
            {
              // Note that we should have been able to use -M instead of -MD
              // (due to -E) but for some reason Clang doesn't like that.
              //
              args.push_back ("-MD"); // Generate dependency information.
              args.push_back ("-MF"); // Write it to stdout.
              args.push_back ("-");
            }

            // Regularize the target name in the make dependency output the
            // same as in the compile rule.
            //
            args.push_back ("-MQ");
            args.push_back ("^");
          }

          // Output.
          //
          if (ot == output_type::header && !mhdr)
          {
            // Output goes directly to the target file.
            //
            args.push_back ("-o");
            args.push_back (relo.string ().c_str ());
          }
          else
          {
            // Output goes to stdout (default for -E).
            //
            // However, without explicit `-o -`, Clang will append the .o
            // extension to the target in the make dependency information
            // (e.g., `-.o:`).
            //
            args.push_back ("-o");
            args.push_back ("-");
          }

          // Input.
          //
          args.push_back ("-x");
          switch (x_lang)
          {
          case lang::c:   args.push_back ("c"); break;
          case lang::cxx: args.push_back ("c++"); break;
          }

          // With GCC and Clang we can compile /dev/null as stdin by
          // specifying `-` and thus omitting the temporary file.
          //
          // Note that in this case the make dependency information won't
          // contain the source file prerequisite. For example:
          //
          // ^: /usr/include/stdc-predef.h
          //
          // Note also that if there are no prerequisites, GCC omits the
          // entire dependency declararion (Clang still prints ^:). We deal
          // with that ad hoc in read_gcc() below. Can be reproduced with
          // -nostdinc.
          //
          args.push_back (rels.empty ()
                          ? "-"
                          : rels.string ().c_str ()); // Note: expected last.

          break;
        }
      case compiler_class::msvc:
        {
          // Add implied options which may affect predefs, similar to the
          // compile rule.
          //
          {
            // Note: these affect the _MSVC_EXECUTION_CHARACTER_SET, _UTF8
            // macros.
            //
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

          if (x_lang == lang::cxx)
          {
            if (!find_option_prefixes ({"/EH", "-EH"}, args))
              args.push_back ("/EHsc");
          }

          if (!find_option_prefixes ({"/MD", "/MT", "-MD", "-MT"}, args))
            args.push_back ("/MD");

          msvc_sanitize_cl (args);

          args.push_back ("/nologo");

          args.push_back ("/EP"); // Preprocess to stdout without `#line`s.
          args.push_back ("/PD"); // Print all macro definitions.
          args.push_back ("/Zc:preprocessor"); // Preproc. conformance mode.

          if (ot == output_type::header)
          {
            // We can only write directly into the header in case of a pure
            // predefs. Otherwise, we have to write the header manually
            // filtering out the preprocessed output (unlike GCC/Clang, MSVC
            // still produces regular output with /PD and there doesn't seem
            // to be any way to suppress it).
            //
            if (s == nullptr)
            {
              // /EP may seem like it contradicts /P but it's the recommended
              // way to suppress `#line`s from the output of the /P option
              // (see /P in the "MSVC Compiler Options" documentation).
              //
              args.push_back ("/P");  // Write preprocessor output to a file.

              // Output (note that while the /Fi: variant is only availbale
              // starting with VS2013, /Zc:preprocessor is only available
              // starting from VS2019).
              //
              args.push_back ("/Fi:");
              args.push_back (relo.string ().c_str ());
            }
            else
              mhdr = true;
          }


          // Input.
          //
          switch (x_lang)
          {
          case lang::c:   args.push_back ("/TC"); break;
          case lang::cxx: args.push_back ("/TP"); break;
          }

          // Input path.
          //
          // Note that with MSVC we have to use a temporary file. In
          // particular compiling `nul` does not work.
          //
          if (s == nullptr)
            rels = relo + ".t";

          args.push_back (rels.string ().c_str ()); // Note: expected last.

          break;
        }
      }

      args.push_back (nullptr);

      // Run the compiler.
      //
      if (verb == 1)
        print_diag ((string (x_name) + "-predefs").c_str (), t);
      else if (verb == 2)
        print_process (args);

      // Switch to the absolute source file path.
      //
      // Note that if it's the empty .t, then we keep the relative path since
      // it doesn't contain any #include's.
      //
      if (s != nullptr)
        *(args.end () - 2) = sp.string ().c_str ();

      if (verb >= 3)
        print_process (args);

      // Sequence start time for mtime checks below.
      //
      timestamp start;

      if (!ctx.dry_run)
      {
        start = depdb::mtime_check () ? system_clock::now () : timestamp_unknown;

        // Create an empty temporary source file, if necessary.
        //
        auto_rmfile rms;
        if (!rels.empty () && s == nullptr)
        {
          rms = auto_rmfile (rels);

          if (exists (rels, false /* follow_symlinks */))
            rmfile (ctx, rels, 3 /* verbosity */);

          touch (ctx, rels, true /* create */, 3 /* verbosity */);
        }

        // Setup output unless the compiler is writing directly into the
        // header.
        //
        struct odata
        {
          const match_data& md;
          set<string> mset; // Seen macros from macro_map.

          ofdstream os;
#ifndef BUILD2_BOOTSTRAP
          json_stream_serializer js;
#endif
          odata (const match_data& d)
              : md (d)
#ifndef BUILD2_BOOTSTRAP
              , js (os) // Ok to create with unopened stream.
#endif
          {}
        } od (md);

        function<void (string, const json_value&)> add_macro;

        auto_rmfile rmo (relo);
        if (ot != output_type::header || mhdr)
        try
        {
          od.os.open (tp);

          switch (ot)
          {
          case output_type::json:
            {
#ifdef BUILD2_BOOTSTRAP
              fail << "json output requested during bootstrap" << endf;
#else
              od.js.begin_object ();

              add_macro = [this, &od] (string n, const json_value& v)
              {
                try
                {
                  // Do closing here not to duplicate exception handling.
                  //
                  if (n.empty ())
                  {
                    // Write explicit null values if we have an explicit map.
                    //
                    if (od.md.mmap != nullptr)
                    {
                      for (const auto& p: *od.md.mmap)
                      {
                        const string& n (p.second ? *p.second : p.first);
                        if (od.mset.find (n) == od.mset.end ())
                          write_macro_json (od.js, n, json_value (nullptr));
                      }
                    }

                    od.js.end_object ();
                    od.os << '\n';
                  }
                  else
                  {
                    write_macro_json (od.js, n, v);

                    if (od.md.mmap != nullptr)
                      od.mset.insert (move (n));
                  }
                }
                catch (const invalid_json_output& e)
                {
                  fail << "invalid json output in " << od.md.tp << ": " << e;
                }
                catch (const io_error& e)
                {
                  fail << "unable to write to " << od.md.tp << ": " << e;
                }
              };
#endif
              break;
            }
          case output_type::buildfile:
            {
              od.os << "# Created automatically by the " << rule_name <<
                " rule, do not edit.\n"
                    << "#\n";

              add_macro = [this, &od] (string n, const json_value& v)
              {
                try
                {
                  // Do closing here not to duplicate exception handling.
                  //
                  if (n.empty ())
                  {
                    // Set null values for undefined macros.
                    //
                    for (const auto& p: *od.md.mmap)
                    {
                      const string& n (p.second ? *p.second : p.first);
                      if (od.mset.find (n) == od.mset.end ())
                        write_macro_buildfile (od.os, n, json_value (nullptr));
                    }
                  }
                  else
                  {
                    write_macro_buildfile (od.os, n, v);
                    od.mset.insert (move (n));
                  }
                }
                catch (const io_error& e)
                {
                  fail << "unable to write to " << od.md.tp << ": " << e;
                }
              };

              break;
            }
          case output_type::header:
            {
              assert (mhdr);
              break;
            }
          }
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << tp << ": " << e;
        }

        // Extract the header dependency information as a byproduct of
        // preprocessing. Essentially, this is the `if(!cache)` part of the
        // verification logic we have in apply().
        //
        depdb dd (move (md.dd));

        // The cache=false version of the add lambda in apply().
        //
        struct data
        {
          action a;
          const file& t;
          match_data& md;
          depdb& dd;
        } d {a, t, md, dd};

        function<void (path)> add_dep (
          [this, &d] (path fp)
          {
            // It feels like we should never end up with a relative path here
            // since we preprocess an absolute path and all our -I path were
            // verified to be absolute.
            //
            // Note that enter_header() treats a relative path as a
            // non-existent, (presumably) generated header, which we don't
            // support.
            //
            assert (fp.absolute ());

            auto df = make_diag_frame (
              [&d] (const diag_record& dr)
              {
                if (verb != 0)
                {
                  dr << info << "while extracting header dependencies ";
                  if (d.md.src != nullptr)
                    dr << "from " << *d.md.src;
                  else
                    dr << "for " << d.t;
                }
              });

            optional<prefix_map> pfx_map (prefix_map {});
            srcout_map so_map;

            if (const build2::file* ft = c_rule.find_header (
                  d.a, d.md.bs, d.t,
                  linfo {} /* unused (since passing pfx_map) */,
                  move (fp), false /* cache */, false /* normalized */,
                  true /* dynamic */,
                  pfx_map, so_map).first)
            {
              // Skip if this is one of the static prerequisites.
              //
              auto& pts (d.t.prerequisite_targets[d.a]);
              for (size_t i (0); i != d.md.pts_n; ++i)
              {
                if (pts[i].target == ft)
                  return;
              }

              // Skip until where we left off.
              //
              if (d.md.skip_count != 0)
              {
                --d.md.skip_count;
                return;
              }

              // Verify it has noop recipe.
              //
              c_rule.verify_existing_header (d.a, d.t, d.md.pts_n, *ft);

              d.dd.write (ft->path ());
            }
            else
              d.dd.write (fp); // Still valid (and now normalized).
          });

        try
        {
          // For MSVC, the header dependency information (/showIncludes)
          // appears to always go to stderr, regardless of whether the
          // preprocessed output goes to a file or stdout. Lucky us.
          //
          // For other compilers, the make dependency information and,
          // potentially, macros, always go to stdout, so we redirect that.
          // For MSVC, there is stdout output unless we are writing pure
          // predefs to a header.
          //
          // We also redirect stdin to /dev/null in case that's used instead
          // of the temporary file.
          //
          // Note: somewhat similar logic as in compile_rule.
          //
          bool msvc (cclass == compiler_class::msvc);
          bool rstdout (!msvc || ot != output_type::header || mhdr);

          process pr (
            cpath,
            args,
            -2,                                       /* stdin (/dev/null) */
            rstdout ? -1 : 2,                         /* stdout (pipe/stderr) */
            diag_buffer::pipe (ctx, msvc /* force */) /* stderr */);

          // Note that while we read both streams until eof in the normal
          // circumstances, we cannot use fdstream_mode::skip for the
          // exception case on both of them: we may end up being blocked
          // trying to read one stream while the process may be blocked
          // writing to the other. So in case of an exception we only skip the
          // diagnostics and close stdout hard. The latter should happen first
          // so the order of the dbuf/is variables is important.
          //
          diag_buffer dbuf (ctx, args[0], pr, (fdstream_mode::non_blocking |
                                               fdstream_mode::skip |
                                               fdstream_mode::text));

          optional<bool> ee; // Expected error.
          optional<string> io;
          try
          {
            ifdstream is (ifdstream::badbit);

            if (rstdout)
              is.open (move (pr.in_ofd), (fdstream_mode::non_blocking |
                                          fdstream_mode::text));

            // Note: io_error thrown by these functions is assumed to relate
            // to input, not output.
            //
            if (ctype == compiler_type::msvc)
              ee = read_msvc (dbuf, is, od.os, add_macro, add_dep, md, rels);
            else
              read_gcc (dbuf, is, od.os, add_macro, add_dep, md, mhdr);

            if (rstdout)
              is.close ();
          }
          catch (const io_error& e)
          {
            // Presumably the child process failed so let run_finish() deal
            // with that first.
            //
            io = e.what ();
          }

          run_finish (dbuf, args, pr, 1 /* verbosity */);

          if (ee && *ee)
          {
            // @@ Would have been better to do it via dbuf (like the compile
            //    rule).
            //
            fail << "expected error exit status from " << x_lang
                 << " compiler";
          }

          if (io)
            fail << "unable to read " << args[0] << " output: " << *io;

          if (od.os.is_open ())
          {
            if (add_macro != nullptr)
              add_macro (string (), json_value ()); // End.

            od.os.close ();
          }

          rmo.cancel ();
        }
        catch (const process_error& e)
        {
          error << "unable to execute " << args[0] << ": " << e;

          if (e.child)
            exit (1);

          throw failed ();
        }

        // Add the terminating blank line.
        //
        dd.expect ("");
        dd.close ();

        md.dd.path = move (dd.path); // For mtime check below.
      }

      timestamp now (system_clock::now ());

      if (!ctx.dry_run)
        depdb::check_mtime (start, md.dd.path, tp, now);

      t.mtime (now);
      return target_state::changed;
    }

    // Parse a macro name after at the specified position (e.g., after
    // `#define`) and check it against the macro list/mapping (if any) If not
    // listed or has arguments, return empty string. Otherwise, return the
    // mapped (if specified) or the original name. Update position to point to
    // after the name.
    //
    static string
    parse_macro_name (tracer& trace,
                      const string& l, size_t& b,
                      const macro_name_map* mm)
    {
      size_t n (l.size ());

      // Skip leading whitespaces.
      //
      for (char c; b != n && ((c = l[b]) == ' ' || c == '\t'); ++b) ;

      size_t e (l.find_first_of (" (\t", b));
      if (e == string::npos)
        e = n;

      bool a (e != n && l[e] == '(');

      // Check the macro list/mapping.
      //
      string r;

      if (mm != nullptr)
      {
        r.assign (l, b, e - b);

        auto i (mm->find (r));

        if (i == mm->end ())
        {
          l5 ([&]{trace << "skipping macro '" << l << "': not listed";});
          return string ();
        }

        if (a)
          fail << "listed macro " << r << " has arguments";

        if (i->second)
        {
          if (i->second->empty ())
            fail << "empty name mapping for macro " << r;

          r = *i->second;
        }
      }
      else if (a)
      {
        l5 ([&]{trace << "skipping macro '" << l << "': has arguments";});
        return string ();
      }
      else
        r.assign (l, b, e - b);

      b = e;
#if 0
      if (a)
      {
        // Skip past the end of the argument list keeping track of the
        // parenthesis balance for good measure.
        //
        size_t m (1);
        while (++b != n && m != 0)
        {
          switch (l[b])
          {
          case '(': ++m; break;
          case ')': --m; break;
          }
        }
        assert (m == 0);
      }
#endif

      return r;
    }

    // Parse a macro value at the specified position (i.e., after the name)
    // and return it as json_value. The default value (or 1 if no default is
    // provided) is returned for a macro that is not defined to any value.
    //
    static json_value
    parse_macro_value (const string& l, size_t b, const string* def_val)
    {
      size_t n (l.size ());

      // Skip leading whitespaces. Note that there is no reason to expect
      // any trailing whitespaces.
      //
      for (char c; b != n && ((c = l[b]) == ' ' || c == '\t'); ++b) ;

      // We recognize boolean true/false (not commonly used) and integers,
      // including hex (0x) and type suffixes (ULL, etc). Everything else we
      // treat as a string. Note that the user can distinguish between
      // char/string literals and other stuff (e.g., names) by checking for
      // the leading quote.
      //
      auto parse_value = [] (const string& s, size_t p) -> json_value
      {
        size_t n (s.size () - p);

        // Fast-path common cases (0 and 1, but let's do all single digits).
        //
        if (n == 1)
        {
          char c (s[p]);

          if (c >= '0' && c <= '9')
            return json_value (uint64_t (c - '0'));
        }

        // Handle boolean.
        //
        if (s.compare (p, n, "true",  4) == 0) return json_value (true);
        if (s.compare (p, n, "false", 5) == 0) return json_value (false);

        assert (n != 0);

        // Handle integers.
        //
        // Check for characters that definitely cannot be part of an integer
        // (note: we must exclude `abcdef` and `x` for hex as well as `ul` for
        // suffixes).
        //
        //
        if (s.find_first_of (".\"\'"
                             "ghijkmnopqrstvwyz"
                             "GHIJKMNOPQRSTVWYZ", p) == string::npos)
        try
        {
          // Some integers are wrapped in parenthesis (e.g., `(-123)`) so
          // we unwrap them.
          //
          bool paren (n > 2 && s[p] == '(' && s[p + n - 1] == ')');

          string v (s, p + (paren ? 1 : 0), n - (paren ? 2 : 0));
          size_t n (v.size ());

          // For some reason MSVC writes explicit sign as `-/+ 123` not as
          // `-/+123`. Let's hack around that. Note that removing this
          // whitespace does not change the result's validity as an integer.
          //
          if ((v[0] == '-' || v[0] == '+') && v[1] == ' ')
          {
            v.erase (1, 1);
            --n;
          }

          bool sig (v[0] == '-');

          // Note: sto*() may throw invalid_argument or out_of_range.
          //
          union
          {
            uint64_t u;
            int64_t  s;
          } r;

          size_t i;
          if (sig)
            r.s = stoll (v, &i, 0);
          else
            r.u = stoull (v, &i, 0);

          if (i != n)
          {
            // Check for type suffixes.
            //
            // Note that `u` and `l`/`ll` can be in different order and of
            // different cases, for example `uLL` or `llU` (but not `lul` or
            // `lL`).
            //
            // Note also that we rely on the fact that v is `\0`-terminated
            // and that's what will be returned by operator[] for the last
            // character.
            //
            auto u = [&v] (size_t i) -> bool
            {
              return v[i] == 'u' || v[i] == 'U';
            };

            auto l = [&v] (size_t i) -> size_t
            {
              return (v[i] == 'l' ? (v[i + 1] == 'l' ? 2 : 1) :
                      v[i] == 'L' ? (v[i + 1] == 'L' ? 2 : 1) : 0);
            };

            if (u (i))
            {
              ++i;
              i += l (i);
            }
            else if (size_t d = l (i))
            {
              i += d;

              if (u (i))
                ++i;
              else if (!sig)
              {
                int64_t t (static_cast<int64_t> (r.u));
                r.s = t;
                sig = true;
              }
            }
          }

          if (i == n)
          {
            if (sig)
              return json_value (r.s);

            // Determine if it's hex.
            //
            size_t p (v[0] == '+' ? 1 : 0);
            return json_value (r.u,
                               v[p++] == '0' && (v[p] == 'x' || v[p] == 'X'));
          }

          // Fall through to return as string.
        }
        catch (const std::exception&)
        {
          // Fall through to return as string.
        }

        return json_value (string (s, p, n));
      };

      if (b == n)
      {
        return (def_val == nullptr ? json_value (uint64_t (1)) :
                def_val->empty ()  ? json_value (string ())    :
                parse_value (*def_val, 0));
      }

      return parse_value (l, b);
    }

    void predefs_rule::
    read_gcc (diag_buffer& dbuf, ifdstream& is,
              ofdstream& os,
              const function<void (string, const json_value&)>& add_mac,
              const function<void (path)>& add_dep,
              match_data& md,
              bool mhdr) const
    {
      tracer trace (x, "predefs_rule::read_gcc");

      // Sometimes we won't have dependency information (see above).
      //
      bool dep (!(ctype == compiler_type::clang && cmaj < 12));

      // Read until we reach EOF on all streams.
      //
      // Note that if dbuf is not opened, then we automatically get an
      // inactive nullfd entry.
      //
      fdselect_set fds {is.fd (), dbuf.is.fd ()};
      fdselect_state& ist (fds[0]);
      fdselect_state& dst (fds[1]);

      // First we should see a bunch of #define lines, unless writing a header
      // directly, followed by the first line of the make dependency
      // information that starts with `^:` and can span multiple lines.
      //
      enum class state {macro_first, macro_next, dep_first, dep_next, end};
      state st (md.ot != output_type::header || mhdr ? state::macro_first :
                dep                                  ? state::dep_first   :
                state::end);

      // Parse a line of make dependency information returning true if more
      // lines are expected.
      //
      auto parse_make_line = [&add_dep,
                              make = make_parser ()] (const string& l) mutable
      {
        using make_state = make_parser;
        using make_type = make_parser::type;

        size_t pos (0);
        do
        {
          pair<make_type, path> r (make.next (l, pos, location ()));

          if (r.second.empty ())
            continue;

          if (r.first == make_type::target)
            continue;

          add_dep (move (r.second));
        }
        while (pos != l.size ());

        return make.state != make_state::end;
      };

      for (string l; ist.fd != nullfd || dst.fd != nullfd; )
      {
        // Note: getline_non_blocking() accumulates the string potentially
        // over several calls.
        //
        if (ist.fd != nullfd && getline_non_blocking (is, l))
        {
          if (eof (is))
            ist.fd = nullfd;
          else
          {
            switch (st)
            {
            case state::macro_first:
            case state::macro_next:
              {
                // @@ TODO: note that we currently don't handle raw string
                //    literals and thus will mis-parse something like this:
                //
                // #define FOO R"(
                // #define BAR
                // )"
                //
                if (l.compare (0, 8, "#define ", 8) == 0)
                {
                  if (st == state::macro_first)
                    st = state::macro_next;

                  if (md.ot != output_type::header)
                  {
                    size_t p (8);
                    string n (parse_macro_name (trace, l, p, md.mmap));

                    if (!n.empty ())
                      add_mac (move (n), parse_macro_value (l, p, md.def_val));
                  }
                  else
                  {
                    try
                    {
                      os << l << '\n';
                    }
                    catch (const io_error& e)
                    {
                      fail << "unable to write to " << md.tp << ": " << e;
                    }
                  }

                  break;
                }
                else
                {
                  // There should definitely be some macros.
                  //
                  if (st == state::macro_first)
                    throw io_error ("unexpected output line '" + l + "'");

                  st = state::dep_first;
                }
              }
              // Fall through.
            case state::dep_first:
              {
                if (l.compare (0, 2, "^:", 2) == 0)
                {
                  st = parse_make_line (l) ? state::dep_next : state::end;
                  break;
                }
              }
              // Fall through.
            case state::end:
              {
                throw io_error ("unexpected output line '" + l + "'");
              }
            case state::dep_next:
              {
                if (!parse_make_line (l))
                  st = state::end;

                break;
              }
            }

            l.clear ();
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

      if (st != state::end)
      {
        if (dep)
        {
          // GCC may not have the dependency information if we are compiling
          // stdin and there are no implied prerequisites (see above for
          // details).
          //
          if (ctype == compiler_type::gcc && md.src == nullptr)
          {
            if (st == (md.ot != output_type::header || mhdr
                       ? state::macro_next
                       : state::dep_first))
              return;
          }

          throw io_error ("missing dependency information");
        }
        else if (st == state::macro_first)
          throw io_error ("missing macro information");
      }
    }

    // Reuse the /showIncludes output parsing from the compile rule.
    //
    optional<bool>
    msvc_first_show (const string&, const string&);

    string
    msvc_next_show (const string&, bool&);

    bool predefs_rule::
    read_msvc (diag_buffer& dbuf, ifdstream& is,
               ofdstream& os,
               const function<void (string, const json_value&)>& add_mac,
               const function<void (path)>& add_dep,
               match_data& md,
               const path& rels) const
    {
      tracer trace (x, "predefs_rule::read_msvc");

      // Read until we reach EOF on all streams.
      //
      // Note that if `is` is not opened, then we automatically get an
      // inactive nullfd entry.
      //
      fdselect_set fds {is.fd (), dbuf.is.fd ()};
      fdselect_state& ist (fds[0]);
      fdselect_state& dst (fds[1]);

      // Unless writing pure predefs directly into a header, we first may see
      // preprocessed output followed by a bunch of #define lines.
      //
      enum class state {preproc, macro, end};
      state st (ist.fd != nullfd ? state::preproc : state::end);

      bool dfirst (true); // First diagnostics line.
      bool error (false); // True if have seen error diagnostics.

      for (string ol, dl; ist.fd != nullfd || dst.fd != nullfd; )
      {
        // Note: getline_non_blocking() accumulates the string potentially
        // over several calls.
        //
        if (ist.fd != nullfd && getline_non_blocking (is, ol))
        {
          if (eof (is))
            ist.fd = nullfd;
          else
          {
            string& l (ol);

#ifndef _WIN32
            trim_right (l); // Strip CRLF junk.
#endif
            switch (st)
            {
            case state::preproc:
              {
                // @@ TODO: while there won't be real #define directives
                //    in the preprocessed output, we will get tripped up
                //    if one is specified as a raw string literal:
                //
                // const char* s = R"(
                // #define BAR
                // )";
                //
                // Note: normally lots of blank lines in the output so first
                // check for that.
                //
                if (l.empty ())
                  continue; // No need to clear.

                if (l.compare (0, 8, "#define ", 8) != 0)
                  break;

                st = state::macro;
              }
              // Fall through.
            case state::macro:
              {
                // @@ TODO: note that we currently don't handle raw string
                //    literals and thus will mis-parse something like this:
                //
                // #define FOO R"(
                // #define BAR
                // )"
                //
                if (l.compare (0, 8, "#define ", 8) == 0)
                {
                  if (md.ot != output_type::header)
                  {
                    size_t p (8);
                    string n (parse_macro_name (trace, l, p, md.mmap));

                    if (!n.empty ())
                      add_mac (move (n), parse_macro_value (l, p, md.def_val));
                  }
                  else
                  {
                    try
                    {
                      os << l << '\n';
                    }
                    catch (const io_error& e)
                    {
                      fail << "unable to write to " << md.tp << ": " << e;
                    }
                  }

                  break;
                }
              }
              // Fall through.
            case state::end:
              {
                throw io_error ("unexpected output line '" + l + "'");
              }
            }

            l.clear ();
          }

          continue;
        }

        if (dst.fd != nullfd && getline_non_blocking (dbuf.is, dl))
        {
          if (eof (dbuf.is))
          {
            bool r (dbuf.read ()); // Close.
            assert (!r);
            dst.fd = nullfd;
          }
          else
          {
            // Note: this twisted logic is similar to the compile rule's. The
            // main difference is that we treat missing headers as an error.

            string& l (dl);

#ifndef _WIN32
            trim_right (l); // Strip CRLF junk.
#endif
            if (dfirst)
            {
              optional<bool> r (msvc_first_show (l, rels.leaf ().string ()));

              if (r && *r)
                dfirst = false;
              else
              {
                if (!r) // Not a D9XXX warning.
                  error = true;

                dbuf.write (l, true /* newline */);
              }

              l.clear ();
              continue;
            }

            bool e (false);
            string f (msvc_next_show (l, e));

            if (e || f.empty ()) // Diagnostics.
            {
              // @@ What if it's a warning, not an error. There is some
              //    thinking in the compile rule's comments (which makes the
              //    same assumption) that it's not easy to trigger a
              //    preprocessor warning in MSVC.

              error = true;
              dbuf.write (l, true /* newline */);
            }
            else
            {
              // If there was an error, skip adding the dependency but
              // continue filtering the diagnosics.
              //
              if (!error)
                add_dep (path (move (f)));
            }

            l.clear ();
          }

          continue;
        }

        ifdselect (fds);
      }

      if (st == state::preproc)
        throw io_error ("missing macro information");

      return error;
    }
  }
}
