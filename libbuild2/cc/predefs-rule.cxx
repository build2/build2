// file      : libbuild2/cc/predefs-rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/predefs-rule.hxx>

#include <libbuild2/depdb.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

namespace build2
{
  namespace cc
  {
    predefs_rule::
    predefs_rule (data&& d)
        : common (move (d)),
          rule_name (string (x) += ".predefs"),
          rule_id (rule_name + " 1")
    {
    }

    bool predefs_rule::
    match (action, target&, const string& hint, match_extra&) const
    {
      tracer trace (x, "predefs_rule::match");

      // We only match with an explicit hint (failed that, we will turn every
      // header into predefs).
      //
      if (hint == rule_name)
      {
        // Don't match if unsupported compiler. In particular, this allows the
        // user to provide a fallback rule.
        //
        switch (cclass)
        {
        case compiler_class::gcc: return true;
        case compiler_class::msvc:
          {
            // Only MSVC 19.20 or later. Not tested with clang-cl.
            //
            if (cvariant.empty () && (cmaj > 19 || (cmaj == 19 && cmin >= 20)))
              return true;

            l4 ([&]{trace << "unsupported compiler/version";});
            break;
          }
        }
      }

      return false;
    }

    recipe predefs_rule::
    apply (action a, target& xt, match_extra&) const
    {
      file& t (xt.as<file> ());
      t.derive_path ();

      // Inject dependency on the output directory.
      //
      inject_fsdir (a, t);

      if (a == perform_update_id)
      {
        return [this] (action a, const target& xt)
        {
          return perform_update (a, xt);
        };
      }
      else if (a == perform_clean_id)
      {
        return [] (action a, const target& t)
        {
          // Also remove the temporary input source file in case it wasn't
          // removed at the end of the update.
          //
          return perform_clean_extra (a, t.as<file> (), {".d", ".t"});
        };
      }
      else
        return noop_recipe; // Configure update.
    }

    // Filter noise, sanitize options (msvc.cxx).
    //
    void
    msvc_filter_cl (diag_buffer&, const path& src);

    void
    msvc_sanitize_cl (cstrings&);

    target_state predefs_rule::
    perform_update (action a, const target& xt) const
    {
      tracer trace (x, "predefs_rule::perform_update");

      const file& t (xt.as<file> ());
      const path& tp (t.path ());

      context& ctx (t.ctx);

      const scope& rs (t.root_scope ());

      // Execute prerequisites (the output directory being the only one thus
      // not mtime checking).
      //
      execute_prerequisites (a, t);

      // Use depdb to track changes to options, compiler, etc (similar to
      // the compile_rule).
      //
      depdb dd (tp + ".d");
      {
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

        // Finally the options checksum (as below).
        //
        {
          sha256 cs;
          append_options (cs, t, c_coptions);
          append_options (cs, t, x_coptions);
          append_options (cs, cmode);

          if (dd.expect (cs.string ()) != nullptr)
            l4 ([&]{trace << "options mismatch forcing update of " << t;});
        }
      }

      // Update if depdb mismatch.
      //
      bool update (dd.writing () || dd.mtime > t.load_mtime ());

      dd.close ();

      if (!update)
        return target_state::unchanged; // No mtime-based prerequisites.

      // Prepare the compiler command-line.
      //
      cstrings args {cpath.recall_string ()};

      // Append compile options.
      //
      // Note that any command line macros that we specify with -D will end up
      // in the predefs, which is something we don't want. So no poptions.
      //
      append_options (args, t, c_coptions);
      append_options (args, t, x_coptions);
      append_options (args, cmode);

      // The output and input paths, relative to the working directory for
      // easier to read diagnostics.
      //
      path relo (relative (tp));
      path reli;

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

          // Output.
          //
          args.push_back ("-o");
          args.push_back (relo.string ().c_str ());

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
          args.push_back ("-");

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

          // /EP may seem like it contradicts /P but it's the recommended
          // way to suppress `#line`s from the output of the /P option (see
          // /P in the "MSVC Compiler Options" documentation).
          //
          args.push_back ("/P");  // Write preprocessor output to a file.
          args.push_back ("/EP"); // Preprocess to stdout without `#line`s.

          args.push_back ("/PD");              // Print all macro definitions.
          args.push_back ("/Zc:preprocessor"); // Preproc. conformance mode.

          // Output (note that while the /Fi: variant is only availbale
          // starting with VS2013, /Zc:preprocessor is only available
          // starting from VS2019).
          //
          args.push_back ("/Fi:");
          args.push_back (relo.string ().c_str ());

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
          reli = relo + ".t";
          args.push_back (reli.string ().c_str ());

          break;
        }
      }

      args.push_back (nullptr);

      // Run the compiler.
      //
      if (verb >= 2)
        print_process (args);
      else if (verb)
        print_diag ((string (x_name) + "-predefs").c_str (), t);

      if (!ctx.dry_run)
      {
        // Create an empty temporary input source file, if necessary.
        //
        auto_rmfile rmi;
        if (!reli.empty ())
        {
          rmi = auto_rmfile (reli);

          if (exists (reli, false /* follow_symlinks */))
            rmfile (ctx, reli, 3 /* verbosity */);

          touch (ctx, reli, true /* create */, 3 /* verbosity */);
        }

        try
        {
          // VC cl.exe sends diagnostics to stdout. It also prints the file
          // name being compiled as the first line. So for cl.exe we filter
          // that noise out.
          //
          // For other compilers also redirect stdout to stderr, in case any
          // of them tries to pull off something similar. For sane compilers
          // this should be harmless.
          //
          // We also redirect stdin to /dev/null in case that's used instead
          // of the temporary file.
          //
          // Note: similar logic as in compile_rule.
          //
          bool filter (ctype == compiler_type::msvc);

          process pr (cpath,
                      args,
                      -2,                                         /* stdin  */
                      2,                                          /* stdout */
                      diag_buffer::pipe (ctx, filter /* force */) /* stderr */);

          diag_buffer dbuf (ctx, args[0], pr);

          if (filter)
            msvc_filter_cl (dbuf, reli);

          dbuf.read ();

          run_finish (dbuf, args, pr, 1 /* verbosity */);
          dd.check_mtime (tp);
        }
        catch (const process_error& e)
        {
          error << "unable to execute " << args[0] << ": " << e;

          if (e.child)
            exit (1);

          throw failed ();
        }
      }

      t.mtime (system_clock::now ());
      return target_state::changed;
    }
  }
}
