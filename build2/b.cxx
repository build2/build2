// file      : build2/b.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <sstream>
#include <typeinfo>
#include <iostream>  // cout
#include <exception> // terminate(), set_terminate(), terminate_handler

#include <libbutl/pager.hxx>
#include <libbutl/fdstream.hxx>  // stderr_fd(), fdterm()
#include <libbutl/backtrace.hxx> // backtrace()

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/serializer.hxx>
#endif

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/dump.hxx>
#include <libbuild2/file.hxx>
#include <libbuild2/rule.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/module.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/context.hxx>
#include <libbuild2/variable.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/buildspec.hxx>
#include <libbuild2/operation.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/file-cache.hxx>
#include <libbuild2/diagnostics.hxx>
#include <libbuild2/prerequisite.hxx>

#include <libbuild2/parser.hxx>

#include <libbuild2/b-options.hxx>
#include <libbuild2/b-cmdline.hxx>

// Build system modules.
//
#include <libbuild2/dist/init.hxx>
#include <libbuild2/test/init.hxx>
#include <libbuild2/config/init.hxx>
#include <libbuild2/install/init.hxx>

#include <libbuild2/in/init.hxx>
#include <libbuild2/bin/init.hxx>
#include <libbuild2/c/init.hxx>
#include <libbuild2/cc/init.hxx>
#include <libbuild2/cxx/init.hxx>
#include <libbuild2/version/init.hxx>

#ifndef BUILD2_BOOTSTRAP
#  include <libbuild2/bash/init.hxx>
#  include <libbuild2/cli/init.hxx>
#endif

using namespace butl;
using namespace std;

namespace build2
{
  int
  main (int argc, char* argv[]);

#ifndef BUILD2_BOOTSTRAP
  // Structured result printer (--structured-result mode).
  //
  class result_printer
  {
  public:
    result_printer (const b_options& ops,
                    const action_targets& tgs,
                    json::stream_serializer& js)
        : ops_ (ops), tgs_ (tgs), json_serializer_ (js) {}

    ~result_printer ();

  private:
    void
    print_lines ();

    void
    print_json ();

  private:
    const b_options& ops_;
    const action_targets& tgs_;
    json::stream_serializer& json_serializer_;
  };

  void result_printer::
  print_lines ()
  {
    for (const action_target& at: tgs_)
    {
      if (at.state == target_state::unknown)
        continue; // Not a target/no result.

      const target& t (at.as<target> ());
      context& ctx (t.ctx);

      cout << at.state
           << ' ' << ctx.current_mif->name
           << ' ' << ctx.current_inner_oif->name;

      if (ctx.current_outer_oif != nullptr)
        cout << '(' << ctx.current_outer_oif->name << ')';

      // There are two ways one may wish to identify the target of the
      // operation: as something specific but inherently non-portable (say, a
      // filesystem path, for example c:\tmp\foo.exe) or as something regular
      // that can be used to refer to a target in a portable way (for example,
      // c:\tmp\exe{foo}; note that the directory part is still not portable).
      // Which one should we use is a good question. Let's go with the
      // portable one for now and see how it goes (we can always add a format
      // variant, e.g., --structured-result=lines-path). Note also that the
      // json format includes both.

      // Set the stream extension verbosity to 0 to suppress extension
      // printing by default (this can still be overriden by the target type's
      // print function as is the case for file{}, for example). And set the
      // path verbosity to 1 to always print absolute.
      //
      stream_verbosity sv (stream_verb (cout));
      stream_verb (cout, stream_verbosity (1, 0));

      cout << ' ' << t << endl;

      stream_verb (cout, sv);
    }
  }

  void result_printer::
  print_json ()
  {
    json::stream_serializer& s (json_serializer_);

    for (const action_target& at: tgs_)
    {
      if (at.state == target_state::unknown)
        continue; // Not a target/no result.

      const target& t (at.as<target> ());
      context& ctx (t.ctx);

      s.begin_object ();

      // Quoted target.
      //
      s.member_name ("target");
      dump_quoted_target_name (s, t);

      // Display target.
      //
      s.member_name ("display_target");
      dump_display_target_name (s, t);

      s.member ("target_type", t.type ().name, false /* check */);

      if (t.is_a<dir> ())
        s.member ("target_path", t.dir.string ());
      else if (const auto* pt = t.is_a<path_target> ())
        s.member ("target_path", pt->path ().string ());

      s.member ("meta_operation", ctx.current_mif->name, false /* check */);
      s.member ("operation", ctx.current_inner_oif->name, false /* check */);

      if (ctx.current_outer_oif != nullptr)
        s.member ("outer_operation",
                  ctx.current_outer_oif->name,
                  false /* check */);

      s.member ("state", to_string (at.state), false /* check */);

      s.end_object ();
    }
  }

  result_printer::
  ~result_printer ()
  {
    // Let's do some sanity checking even when we are not in the structred
    // output mode.
    //
#ifndef NDEBUG
    for (const action_target& at: tgs_)
    {
      switch (at.state)
      {
      case target_state::unknown:
      case target_state::unchanged:
      case target_state::changed:
      case target_state::failed:    break;    // Valid states.
      default:                      assert (false);
      }
    }
#endif

    if (ops_.structured_result_specified ())
    {
      switch (ops_.structured_result ())
      {
      case structured_result_format::lines:
        {
          print_lines ();
          break;
        }
      case structured_result_format::json:
        {
          print_json ();
          break;
        }
      }
    }
  }
#endif
}

// Print backtrace if terminating due to an unhandled exception. Note that
// custom_terminate is non-static and not a lambda to reduce the noise.
//
static terminate_handler default_terminate;

void
custom_terminate ()
{
  *diag_stream << backtrace ();

  if (default_terminate != nullptr)
    default_terminate ();
}

static void
terminate (bool trace)
{
  if (!trace)
    set_terminate (default_terminate);

  std::terminate ();
}

int build2::
main (int argc, char* argv[])
{
  default_terminate = set_terminate (custom_terminate);

  tracer trace ("main");

  init_process ();

  int r (0);
  b_options ops;
  scheduler sched;

  // Statistics.
  //
  size_t phase_switch_contention (0);

  try
  {
    // Parse the command line.
    //
    b_cmdline cmdl (parse_b_cmdline (trace, argc, argv, ops));

    // Handle --build2-metadata (see also buildfile).
    //
#ifndef BUILD2_BOOTSTRAP
    if (ops.build2_metadata_specified ())
    {
      auto& o (cout);

      // Note that the export.metadata variable should be the first non-
      // blank/comment line.
      //
      o << "# build2 buildfile b" << endl
        << "export.metadata = 1 b" << endl
        << "b.name = [string] b" << endl
        << "b.version = [string] '" << LIBBUILD2_VERSION_FULL << '\'' << endl
        << "b.checksum = [string] '" << LIBBUILD2_VERSION_FULL << '\'' << endl
        << "b.environment = [strings] BUILD2_VAR_OVR BUILD2_DEF_OPT" << endl
        << "b.static = [bool] " <<
#ifdef LIBBUILD2_STATIC
        "true"
#else
        "false"
#endif
        << endl;

      return 0;
    }
#endif

    // Handle --version.
    //
    if (ops.version ())
    {
      auto& o (cout);

      o << "build2 " << LIBBUILD2_VERSION_ID << endl
        << "libbutl " << LIBBUTL_VERSION_ID << endl
        << "host " << BUILD2_HOST_TRIPLET << endl;

#ifndef BUILD2_BOOTSTRAP
      o << "Copyright (c) " << BUILD2_COPYRIGHT << "." << endl;
#endif

      o << "This is free software released under the MIT license." << endl;
      return 0;
    }

    // Initialize the diagnostics state.
    //
    init_diag (cmdl.verbosity,
               ops.silent (),
               cmdl.progress,
               cmdl.diag_color,
               ops.no_line (),
               ops.no_column (),
               fdterm (stderr_fd ()));

    // Handle --help.
    //
    if (ops.help ())
    {
      try
      {
        pager p ("b help",
                 verb >= 2,
                 ops.pager_specified () ? &ops.pager () : nullptr,
                 &ops.pager_option ());

        print_b_usage (p.stream ());

        // If the pager failed, assume it has issued some diagnostics.
        //
        return p.wait () ? 0 : 1;
      }
      // Catch io_error as std::system_error together with the pager-specific
      // exceptions.
      //
      catch (const system_error& e)
      {
        fail << "pager failed: " << e;
      }
    }

    // Initialize the global state.
    //
    init (&::terminate,
          argv[0],
          ops.serial_stop (),
          cmdl.mtime_check,
          cmdl.config_sub,
          cmdl.config_guess);

    // Load builtin modules.
    //
    load_builtin_module (&config::build2_config_load);
    load_builtin_module (&dist::build2_dist_load);
    load_builtin_module (&test::build2_test_load);
    load_builtin_module (&install::build2_install_load);

    load_builtin_module (&bin::build2_bin_load);
    load_builtin_module (&cc::build2_cc_load);
    load_builtin_module (&c::build2_c_load);
    load_builtin_module (&cxx::build2_cxx_load);
    load_builtin_module (&version::build2_version_load);
    load_builtin_module (&in::build2_in_load);

#ifndef BUILD2_BOOTSTRAP
    load_builtin_module (&bash::build2_bash_load);
    load_builtin_module (&cli::build2_cli_load);
#endif

    // Start up the scheduler and allocate lock shards.
    //
    sched.startup (cmdl.jobs,
                   1 /* init_active */,
                   cmdl.max_jobs,
                   cmdl.jobs * ops.queue_depth (),
                   cmdl.max_stack);

    global_mutexes mutexes (sched.shard_size ());
    file_cache fcache (cmdl.fcache_compress);

    // Trace some overall environment information.
    //
    if (verb >= 5)
    {
      optional<string> p (getenv ("PATH"));

      trace << "work: " << work;
      trace << "home: " << home;
      trace << "path: " << (p ? *p : "<NULL>");
      trace << "type: " << (build_installed ? "installed" : "development");
      trace << "jobs: " << cmdl.jobs;
    }

    // Set the build context before parsing the buildspec since it relies on
    // the global scope being setup. We reset it for every meta-operation (see
    // below).
    //
    unique_ptr<context> pctx;
    auto new_context = [&ops, &cmdl,
                        &sched, &mutexes, &fcache,
                        &phase_switch_contention,
                        &pctx]
    {
      if (pctx != nullptr)
      {
        phase_switch_contention += (pctx->phase_mutex.contention +
                                    pctx->phase_mutex.contention_load);
        pctx = nullptr; // Free first to reuse memory.
      }

      optional<match_only_level> mo;
      if      (ops.load_only ())  mo = match_only_level::alias;
      else if (ops.match_only ()) mo = match_only_level::all;

      pctx.reset (new context (sched,
                               mutexes,
                               fcache,
                               mo,
                               ops.no_external_modules (),
                               ops.dry_run (),
                               ops.no_diag_buffer (),
                               !ops.serial_stop () /* keep_going */,
                               cmdl.cmd_vars));

      if (ops.trace_match_specified ())
        pctx->trace_match = &ops.trace_match ();

      if (ops.trace_execute_specified ())
        pctx->trace_execute = &ops.trace_execute ();
    };

    new_context ();

    // Parse the buildspec.
    //
    buildspec bspec;
    path_name bspec_name ("<buildspec>");
    try
    {
      istringstream is (cmdl.buildspec);
      is.exceptions (istringstream::failbit | istringstream::badbit);

      parser p (*pctx);
      bspec = p.parse_buildspec (is, bspec_name);
    }
    catch (const io_error&)
    {
      fail << "unable to parse buildspec '" << cmdl.buildspec << "'";
    }

    l5 ([&]{trace << "buildspec: " << bspec;});

    if (bspec.empty ())
      bspec.push_back (metaopspec ()); // Default meta-operation.

    // The reserve values were picked experimentally. They allow building a
    // sample application that depends on Qt and Boost without causing a
    // rehash.
    //
    // Note: omit reserving anything for the info meta-operation since it
    // won't be loading the buildfiles and needs to be as fast as possible.
    //
    bool mo_info (bspec.size () == 1 &&
                  bspec.front ().size () == 1 &&
                  (bspec.front ().name == "info" ||
                   (bspec.front ().name.empty () &&
                    bspec.front ().front ().name == "info")));

    if (!mo_info)
    {
      // Note: also adjust in bpkg if adjusting here.
      //
      pctx->reserve (context::reserves {
          30000 /* targets */,
          1100  /* variables */});
    }

    bool load_only (ops.load_only ());

    const path& buildfile (ops.buildfile_specified ()
                           ? ops.buildfile ()
                           : empty_path);

    bool dump_load (false);
    bool dump_match (false);
    bool dump_match_pre (false);
    bool dump_match_post (false);
    for (const string& p: ops.dump ())
    {
      if      (p == "load")       dump_load = true;
      else if (p == "match")      dump_match = true;
      else if (p == "match-pre")  dump_match_pre = true;
      else if (p == "match-post") dump_match_post = true;
      else fail << "unknown phase '" << p << "' specified with --dump";
    }

    dump_format dump_fmt (dump_format::buildfile);
    if (ops.dump_format_specified ())
    {
      const string& f (ops.dump_format ());

      if (f == "json-v0.1")
      {
#ifdef BUILD2_BOOTSTRAP
        fail << "json dump not supported in bootstrap build system";
#endif
        dump_fmt = dump_format::json;
      }
      else if (f != "buildfile")
      {
        diag_record dr (fail);

        dr << "unsupported format '" << f << "' specified with --dump-format";

        if (f.compare (0, 4, "json") == 0)
          dr << info << "supported json format version is json-v0.1";
      }
    }

    auto dump = [&trace, &ops, dump_fmt] (context& ctx, optional<action> a)
    {
      const dir_paths& scopes (ops.dump_scope ());
      const vector<pair<name, optional<name>>>& targets (ops.dump_target ());

      if (scopes.empty () && targets.empty ())
        build2::dump (ctx, a, dump_fmt);
      else
      {
        auto comp_norm = [] (dir_path& d, const char* what)
        {
          try
          {
            if (d.relative ())
              d.complete ();

            d.normalize ();
          }
          catch (const invalid_path& e)
          {
            fail << "invalid path '" << e.path << "' specified with " << what;
          }
        };

        // If exact is false then return any outer scope that contains this
        // directory except for the global scope.
        //
        auto find_scope = [&ctx, &comp_norm] (dir_path& d,
                                              bool exact,
                                              const char* what) -> const scope*
        {
          comp_norm (d, what);

          // This is always the output directory (specifically, see the target
          // case below).
          //
          const scope& s (ctx.scopes.find_out (d));

          return ((exact ? s.out_path () == d : s != ctx.global_scope)
                  ? &s
                  : nullptr);
        };

        // Dump scopes.
        //
        for (dir_path d: scopes)
        {
          const scope* s (find_scope (d, true, "--dump-scope"));

          if (s == nullptr)
            l5 ([&]{trace << "unknown target scope " << d
                          << " specified with --dump-scope";});

          build2::dump (s, a, dump_fmt);
        }

        // Dump targets.
        //
        for (const pair<name, optional<name>>& p: targets)
        {
          const target* t (nullptr);

          // Find the innermost known scope that contains this target. This
          // is where we are going to resolve its type.
          //
          dir_path d (p.second ? p.second->dir : p.first.dir);

          if (const scope* s = find_scope (d, false, "--dump-target"))
          {
            // Complete relative directories in names.
            //
            name n (p.first), o;

            if (p.second)
            {
              comp_norm (n.dir, "--dump-target");
              o.dir = move (d);
            }
            else
              n.dir = move (d);

            // Similar logic to parser::enter_target::find_target() as used by
            // the dump directive. Except here we treat unknown target type as
            // unknown target.
            //
            auto r (s->find_target_type (n, location ()));

            if (r.first != nullptr)
            {
              t = ctx.targets.find (*r.first, // target type
                                    n.dir,
                                    o.dir,
                                    n.value,
                                    r.second, // extension
                                    trace);

              if (t == nullptr)
                l5 ([&]
                    {
                      // @@ TODO: default_extension?
                      //
                      target::combine_name (n.value, r.second, false);
                      names ns {move (n)};
                      if (p.second)
                        ns.push_back (move (o));

                      trace << "unknown target " << ns
                            << " specified with --dump-target";
                    });
            }
            else
              l5 ([&]{trace << "unknown target type '" << n.type << "' in "
                            << *s << " specified with --dump-target";});

          }
          else
            l5 ([&]{trace << "unknown target scope " << d
                          << " specified with --dump-target";});

          build2::dump (t, a, dump_fmt);
        }
      }
    };

    // If not NULL, then lifted points to the operation that has been "lifted"
    // to the meta-operaion (see the logic below for details). Skip is the
    // position of the next operation.
    //
    opspec* lifted (nullptr);
    size_t skip (0);

    // The dirty flag indicated whether we managed to execute anything before
    // lifting an operation.
    //
    bool dirty (false); // Already (re)set for the first run.

#ifndef BUILD2_BOOTSTRAP
    // Note that this constructor is cheap and so we rather call it always
    // instead of resorting to dynamic allocations.
    //
    // Note also that we disable pretty-printing if there is also the JSON
    // dump and thus we need to combine the two in the JSON Lines format.
    //
    json::stream_serializer js (cout, dump_fmt == dump_format::json ? 0 : 2);

    if (ops.structured_result_specified () &&
        ops.structured_result () == structured_result_format::json)
      js.begin_array ();
#endif

    for (auto mit (bspec.begin ()); mit != bspec.end (); )
    {
      vector_view<opspec> opspecs;

      if (lifted == nullptr)
      {
        metaopspec& ms (*mit);

        if (ms.empty ())
          ms.push_back (opspec ()); // Default operation.

        // Continue where we left off after lifting an operation.
        //
        opspecs.assign (ms.data () + skip, ms.size () - skip);

        // Reset since unless we lift another operation, we move to the
        // next meta-operation (see bottom of the loop).
        //
        skip = 0;

        // This can happen if we have lifted the last operation in opspecs.
        //
        if (opspecs.empty ())
        {
          ++mit;
          continue;
        }
      }
      else
        opspecs.assign (lifted, 1);

      // Reset the build context for each meta-operation since there is no
      // guarantee their assumptions (e.g., in the load callback) are
      // compatible.
      //
      if (dirty)
      {
        new_context ();
        dirty = false;
      }

      context& ctx (*pctx);

      const location l (bspec_name, 0, 0); //@@ TODO (also bpkg::pkg_configure())

      meta_operation_id mid (0); // Not yet translated.
      const meta_operation_info* mif (nullptr);

      // See if this meta-operation wants to pre-process the opspecs. Note
      // that this functionality can only be used for build-in meta-operations
      // that were explicitly specified on the command line (so cannot be used
      // for perform) and that will be lifted early (see below).
      //
      values& mparams (lifted == nullptr ? mit->params : lifted->params);
      string  mname   (lifted == nullptr ? mit->name   : lifted->name);

      ctx.current_mname = mname; // Set early.

      if (!mname.empty ())
      {
        if (meta_operation_id m = ctx.meta_operation_table.find (mname))
        {
          // Can modify params, opspec, change meta-operation name.
          //
          if (auto f = ctx.meta_operation_table[m].process)
            mname = ctx.current_mname = f (
              ctx, mparams, opspecs, lifted != nullptr, l);
        }
      }

      // Expose early so can be used during bootstrap (with the same
      // limitations as for pre-processing).
      //
      scope& gs (ctx.global_scope.rw ());
      gs.assign (ctx.var_build_meta_operation) = mname;

      for (auto oit (opspecs.begin ()); oit != opspecs.end (); ++oit)
      {
        opspec& os (*oit);

        // A lifted meta-operation will always have default operation.
        //
        const values& oparams (lifted == nullptr ? os.params : values ());
        const string& oname   (lifted == nullptr ? os.name   : empty_string);

        ctx.current_oname = oname; // Set early.

        if (lifted != nullptr)
          lifted = nullptr; // Clear for the next iteration.

        if (os.empty ()) // Default target: dir{}.
          os.push_back (targetspec (name ("dir", string ())));

        operation_id oid (0), orig_oid (0);
        const operation_info* oif (nullptr);
        const operation_info* outer_oif (nullptr);

        operation_id pre_oid (0), orig_pre_oid (0);
        const operation_info* pre_oif (nullptr);

        operation_id post_oid (0), orig_post_oid (0);
        const operation_info* post_oif (nullptr);

        // Return true if this operation is lifted.
        //
        auto lift = [&ctx,
                     &oname, &mname,
                     &os, &mit, &lifted, &skip, &l, &trace] ()
        {
          meta_operation_id m (ctx.meta_operation_table.find (oname));

          if (m != 0)
          {
            if (!mname.empty ())
              fail (l) << "nested meta-operation " << mname << '('
                       << oname << ')';

            l5 ([&]{trace << "lifting operation " << oname
                          << ", id " << uint16_t (m);});

            lifted = &os;
            skip = lifted - mit->data () + 1;
          }

          return m != 0;
        };

        // We do meta-operation and operation batches sequentially (no
        // parallelism). But multiple targets in an operation batch can be
        // done in parallel.

        // First see if we can lift this operation early by checking if it
        // is one of the built-in meta-operations. This is important to make
        // sure we pre-process the opspec before loading anything.
        //
        if (!oname.empty () && lift ())
          break;

        // Next bootstrap projects for all the target so that all the variable
        // overrides are set (if we also load/search/match in the same loop
        // then we may end up loading a project (via import) before this
        // happends.
        //
        for (targetspec& ts: os)
        {
          name& tn (ts.name);

          // First figure out the out_base of this target. The logic is as
          // follows: if a directory was specified in any form, then that's
          // the out_base. Otherwise, we check if the name value has a
          // directory prefix. This has a good balance of control and the
          // expected result in most cases.
          //
          dir_path out_base (tn.dir);
          if (out_base.empty ())
          {
            const string& v (tn.value);

            // Handle a few common cases as special: empty name, '.', '..', as
            // well as dir{foo/bar} (without trailing '/'). This logic must be
            // consistent with find_target_type() and other places (grep for
            // "..").
            //
            if (v.empty () || v == "." || v == ".." || tn.type == "dir")
              out_base = dir_path (v);
            //
            // Otherwise, if this is a simple name, see if there is a
            // directory part in value.
            //
            else if (tn.untyped ())
            {
              // We cannot assume it is a valid filesystem name so we
              // will have to do the splitting manually.
              //
              path::size_type i (path::traits_type::rfind_separator (v));

              if (i != string::npos)
                out_base = dir_path (v, i != 0 ? i : 1); // Special case: "/".
            }
          }

          try
          {
            if (out_base.relative ())
              out_base = work / out_base;

            // This directory came from the command line so actualize it.
            //
            out_base.normalize (true);
          }
          catch (const invalid_path& e)
          {
            fail << "invalid out_base directory '" << e.path << "'";
          }

          // The order in which we determine the roots depends on whether
          // src_base was specified explicitly.
          //
          dir_path src_root;
          dir_path out_root;

          // Standard/alternative build file/directory naming.
          //
          optional<bool> altn;

          // Update these in buildspec.
          //
          bool& forwarded (ts.forwarded);
          dir_path& src_base (ts.src_base);

          if (!src_base.empty ())
          {
            // Make sure it exists. While we will fail further down if it
            // doesn't, the diagnostics could be confusing (e.g., unknown
            // operation because we didn't load bootstrap.build).
            //
            if (!exists (src_base))
              fail << "src_base directory " << src_base << " does not exist";

            try
            {
              if (src_base.relative ())
                src_base = work / src_base;

              // Also came from the command line, so actualize.
              //
              src_base.normalize (true);
            }
            catch (const invalid_path& e)
            {
              fail << "invalid src_base directory '" << e.path << "'";
            }

            // Make sure out_base is not a subdirectory of src_base. Who would
            // want to do that, you may ask. Well, you would be surprised...
            //
            if (out_base != src_base && out_base.sub (src_base))
              fail << "out_base directory is inside src_base" <<
                info << "src_base: " << src_base <<
                info << "out_base: " << out_base;

            // If the src_base was explicitly specified, search for src_root.
            //
            src_root = find_src_root (src_base, altn);

            // If not found, assume this is a simple project with src_root
            // being the same as src_base.
            //
            if (src_root.empty ())
            {
              src_root = src_base;
              out_root = out_base;
            }
            else
            {
              // Calculate out_root based on src_root/src_base.
              //
              try
              {
                out_root = out_base.directory (src_base.leaf (src_root));
              }
              catch (const invalid_path&)
              {
                fail << "out_base suffix does not match src_root" <<
                  info << "src_root: " << src_root <<
                  info << "out_base: " << out_base;
              }
            }
          }
          else
          {
            // If no src_base was explicitly specified, search for out_root.
            //
            auto p (find_out_root (out_base, altn));

            if (p.second) // Also src_root.
            {
              src_root = move (p.first);

              // Handle a forwarded configuration. Note that if we've changed
              // out_root then we also have to remap out_base.
              //
              out_root = bootstrap_fwd (ctx, src_root, altn);
              if (src_root != out_root)
              {
                out_base = out_root / out_base.leaf (src_root);
                forwarded = true;
              }
            }
            else
            {
              out_root = move (p.first);

              // If not found (i.e., we have no idea where the roots are),
              // then this can only mean a simple project. Which in turn means
              // there should be a buildfile in out_base.
              //
              // Note that unlike the normal project case below, here we don't
              // try to look for outer buildfiles since we don't have the root
              // to stop at. However, this shouldn't be an issue since simple
              // project won't normally have targets in subdirectories (or, in
              // other words, we are not very interested in "complex simple
              // projects").
              //
              if (out_root.empty ())
              {
                if (!find_buildfile (out_base, out_base, altn, buildfile))
                {
                  fail << "no buildfile in " << out_base <<
                    info << "consider explicitly specifying its src_base";
                }

                src_root = src_base = out_root = out_base;
              }
            }
          }

          // Now we know out_root and, if it was explicitly specified or the
          // same as out_root, src_root. The next step is to create the root
          // scope and load the out_root bootstrap files, if any. Note that we
          // might already have done this as a result of one of the preceding
          // target processing.
          //
          // If we know src_root, set that variable as well. This could be of
          // use to the bootstrap files (other than src-root.build, which,
          // BTW, doesn't need to exist if src_root == out_root).
          //
          scope& rs (*create_root (ctx, out_root, src_root)->second.front ());

          bool bstrapped (bootstrapped (rs));

          if (!bstrapped)
          {
            // See if the bootstrap process set/changed src_root.
            //
            value& v (bootstrap_out (rs, altn));

            if (v)
            {
              // If we also have src_root specified by the user, make sure
              // they match.
              //
              dir_path& p (cast<dir_path> (v));

              if (src_root.empty ())
                src_root = p;
              else if (src_root != p)
              {
                // We used to fail here but that meant there were no way to
                // actually fix the problem (i.e., remove a forward or
                // reconfigure the out directory). So now we warn (unless
                // quiet, which is helful to tools like the package manager
                // that are running info underneath).
                //
                // We also save the old/new values since we may have to remap
                // src_root for subprojects (amalgamations are handled by not
                // loading outer project for disfigure and info).
                //
                if (verb)
                  warn << "configured src_root " << p << " does not match "
                       << (forwarded ? "forwarded " : "specified ")
                       << src_root;

                ctx.new_src_root = src_root;
                ctx.old_src_root = move (p);
                p = src_root;
              }
            }
            else
            {
              // Neither bootstrap nor the user produced src_root.
              //
              if (src_root.empty ())
              {
                fail << "no bootstrapped src_root for " << out_root <<
                  info << "consider reconfiguring this out_root";
              }

              v = src_root;
            }

            setup_root (rs, forwarded);

            // Now that we have src_root, load the src_root bootstrap file,
            // if there is one.
            //
            // As an optimization, omit discovering subprojects for the info
            // meta-operation if not needed.
            //
            bootstrap_pre (rs, altn);
            bootstrap_src (rs, altn,
                           nullopt /* amalgamation */,
                           !mo_info || info_subprojects (mparams) /*subprojects*/);

            // If this is a simple project, then implicitly load the test and
            // install modules.
            //
            if (*rs.root_extra->project == nullptr)
            {
              boot_module (rs, "test", location ());
              boot_module (rs, "install", location ());
            }

            // bootstrap_post() delayed until after create_bootstrap_outer().
          }
          else
          {
            // Note that we only "upgrade" the forwarded value since the same
            // project root can be arrived at via multiple paths (think
            // command line and import).
            //
            if (forwarded)
              rs.assign (ctx.var_forwarded) = true;

            // Sync local variable that are used below with actual values.
            //
            if (src_root.empty ())
              src_root = rs.src_path ();

            if (!altn)
              altn = rs.root_extra->altn;
            else
              assert (*altn == rs.root_extra->altn);
          }

          // At this stage we should have both roots and out_base figured
          // out. If src_base is still undetermined, calculate it.
          //
          if (src_base.empty ())
          {
            src_base = src_root / out_base.leaf (out_root);

            if (!exists (src_base))
            {
              fail << src_base << " does not exist" <<
                info << "consider explicitly specifying src_base for "
                   << out_base;
            }
          }

          // Check that out_root that we have found is the innermost root
          // for this project. If it is not, then it means we are trying
          // to load a disfigured sub-project and that we do not support.
          // Why don't we support it? Because things are already complex
          // enough here.
          //
          // Note that the subprojects variable has already been processed
          // and converted to a map by the bootstrap_src() call above.
          //
          if (const subprojects* ps = *rs.root_extra->subprojects)
          {
            for (const auto& p: *ps)
            {
              if (out_base.sub (out_root / p.second))
                fail << tn << " is in a subproject of " << out_root <<
                  info << "explicitly specify src_base for this target";
            }
          }

          // The src bootstrap should have loaded all the modules that
          // may add new meta/operations. So at this stage they should
          // all be known. We store the combined action id in uint8_t;
          // see <operation> for details.
          //
          assert (ctx.operation_table.size () <= 128);
          assert (ctx.meta_operation_table.size () <= 128);

          // Since we now know all the names of meta-operations and
          // operations, "lift" names that we assumed (from buildspec syntax)
          // were operations but are actually meta-operations. Also convert
          // empty names (which means they weren't explicitly specified) to
          // the defaults and verify that all the names are known.
          //
          {
            if (!oname.empty () && lift ())
              break; // Out of targetspec loop.

            meta_operation_id m (0);
            operation_id o (0);

            if (!mname.empty ())
            {
              m = ctx.meta_operation_table.find (mname);

              if (m == 0)
                fail (l) << "unknown meta-operation " << mname;
            }

            if (!oname.empty ())
            {
              o = ctx.operation_table.find (oname);

              if (o == 0)
                fail (l) << "unknown operation " << oname;
            }

            // The default meta-operation is perform. The default operation is
            // assigned by the meta-operation below.
            //
            if (m == 0)
              m = perform_id;

            // If this is the first target in the meta-operation batch, then
            // set the batch meta-operation id.
            //
            bool first (mid == 0);
            if (first)
            {
              mid = m;
              mif = rs.root_extra->meta_operations[m];

              if (mif == nullptr)
                fail (l) << "target " << tn << " does not support meta-"
                         << "operation " << ctx.meta_operation_table[m].name;
            }
            //
            // Otherwise, check that all the targets in a meta-operation
            // batch have the same meta-operation implementation.
            //
            else
            {
              const meta_operation_info* mi (
                rs.root_extra->meta_operations[mid]);

              if (mi == nullptr)
                fail (l) << "target " << tn << " does not support meta-"
                         << "operation " << ctx.meta_operation_table[mid].name;

              if (mi != mif)
                fail (l) << "different implementations of meta-operation "
                         << mif->name << " in the same meta-operation batch";
            }

            // Create and bootstrap outer roots if any. Loading is done by
            // load_root() (that would be called by the meta-operation's
            // load() callback below).
            //
            if (mif->bootstrap_outer)
              create_bootstrap_outer (rs);

            if (!bstrapped)
              bootstrap_post (rs);

            if (first)
            {
              l5 ([&]{trace << "start meta-operation batch " << mif->name
                            << ", id " << static_cast<uint16_t> (mid);});

              if (mif->meta_operation_pre != nullptr)
                mif->meta_operation_pre (ctx, mparams, l);
              else if (!mparams.empty ())
                fail (l) << "unexpected parameters for meta-operation "
                         << mif->name;

              ctx.current_meta_operation (*mif);
              dirty = true;
            }

            // If this is the first target in the operation batch, then set
            // the batch operation id.
            //
            if (oid == 0)
            {
              auto lookup = [&ctx, &rs, &l, &tn] (operation_id o) ->
                const operation_info*
              {
                const operation_info* r (rs.root_extra->operations[o]);

                if (r == nullptr)
                  fail (l) << "target " << tn << " does not support "
                           << "operation " << ctx.operation_table[o];
                return r;
              };

              if (o == 0)
                o = default_id;

              // Save the original oid before de-aliasing.
              //
              orig_oid = o;
              oif = lookup (o);

              l5 ([&]{trace << "start operation batch " << oif->name
                            << ", id " << static_cast<uint16_t> (oif->id);});

              // Allow the meta-operation to translate the operation.
              //
              if (mif->operation_pre != nullptr)
                oid = mif->operation_pre (ctx, mparams, oif->id);
              else // Otherwise translate default to update.
                oid = (oif->id == default_id ? update_id : oif->id);

              if (oif->id != oid)
              {
                // Update the original id (we assume in the check below that
                // translation would have produced the same result since we've
                // verified the meta-operation implementation is the same).
                //
                orig_oid = oid;
                oif = lookup (oid);
                oid = oif->id; // De-alias.

                l5 ([&]{trace << "operation translated to " << oif->name
                              << ", id " << static_cast<uint16_t> (oid);});
              }

              if (oif->outer_id != 0)
                outer_oif = lookup (oif->outer_id);

              if (!oparams.empty ())
              {
                // Operation parameters belong to outer operation, if any.
                //
                auto* i (outer_oif != nullptr ? outer_oif : oif);

                if (i->operation_pre == nullptr)
                  fail (l) << "unexpected parameters for operation " << i->name;
              }

              // Handle pre/post operations.
              //
              if (auto po = oif->pre_operation)
              {
                if ((orig_pre_oid = po (
                       ctx,
                       outer_oif == nullptr ? oparams : values {},
                       mid,
                       l)) != 0)
                {
                  assert (orig_pre_oid != default_id);
                  pre_oif = lookup (orig_pre_oid);
                  pre_oid = pre_oif->id; // De-alias.
                }
              }

              if (auto po = oif->post_operation)
              {
                if ((orig_post_oid = po (
                       ctx,
                       outer_oif == nullptr ? oparams : values {},
                       mid)) != 0)
                {
                  assert (orig_post_oid != default_id);
                  post_oif = lookup (orig_post_oid);
                  post_oid = post_oif->id;
                }
              }
            }
            //
            // Similar to meta-operations, check that all the targets in
            // an operation batch have the same operation implementation.
            //
            else
            {
              auto check = [&ctx, &rs, &l, &tn] (operation_id o,
                                                 const operation_info* i)
              {
                const operation_info* r (rs.root_extra->operations[o]);

                if (r == nullptr)
                  fail (l) << "target " << tn << " does not support "
                           << "operation " << ctx.operation_table[o];

                if (r != i)
                  fail (l) << "different implementations of operation "
                           << i->name << " in the same operation batch";
              };

              check (orig_oid, oif);

              if (oif->outer_id != 0)
                check (oif->outer_id, outer_oif);

              if (pre_oid != 0)
                check (orig_pre_oid, pre_oif);

              if (post_oid != 0)
                check (orig_post_oid, post_oif);
            }
          }

          // If we cannot find the buildfile in this directory, then try our
          // luck with the nearest outer buildfile, in case our target is
          // defined there (common with non-intrusive project conversions
          // where everything is built from a single root buildfile).
          //
          // Note: we use find_plausible_buildfile() and not find_buildfile()
          // to look in outer directories.
          //
          optional<path> bf (
            find_buildfile (src_base, src_base, altn, buildfile));

          if (!bf)
          {
            bf = find_plausible_buildfile (tn, rs,
                                           src_base, src_root,
                                           altn, buildfile);
            if (!bf)
              fail << "no buildfile in " << src_base << " or parent "
                   << "directories" <<
                info << "consider explicitly specifying src_base for "
                   << out_base << endf;

            if (!bf->empty ())
            {
              // Adjust bases to match the directory where we found the
              // buildfile since that's the scope it will be loaded
              // in. Note: but not the target since it is resolved relative
              // to work; see below.
              //
              src_base = bf->directory ();
              out_base = out_src (src_base, out_root, src_root);
            }
          }

          if (verb >= 5)
          {
            trace << "bootstrapped " << tn << ':';
            trace << "  out_base:     " << out_base;
            trace << "  src_base:     " << src_base;
            trace << "  out_root:     " << out_root;
            trace << "  src_root:     " << src_root;
            trace << "  forwarded:    " << (forwarded ? "true" : "false");
            if (const dir_path* a = *rs.root_extra->amalgamation)
            {
              trace << "  amalgamation: " << *a;
              trace << "  bundle scope: " << *rs.bundle_scope ();
              trace << "  strong scope: " << *rs.strong_scope ();
              trace << "  weak scope:   " << *rs.weak_scope ();
            }
          }

          // Enter project-wide (as opposed to global) variable overrides.
          //
          // And, yes, this means non-global overrides are not visible during
          // bootstrap. If you are wondering why, it's because the project
          // boundaries (specifically, amalgamation) are only known after
          // bootstrap.
          //
          ctx.enter_project_overrides (rs, out_base, ctx.var_overrides);

          ts.root_scope = &rs;
          ts.out_base = move (out_base);
          ts.buildfile = move (*bf);
        } // target

        // If this operation has been lifted, break out.
        //
        if (lifted == &os)
        {
          assert (oid == 0); // Should happend on the first target.
          break;
        }

        if (load_only && (mid != perform_id || oid != update_id))
          fail << "--load-only requires perform(update) action";

        // Now load the buildfiles and search the targets.
        //
        action_targets tgs;
        tgs.reserve (os.size ());

        for (targetspec& ts: os)
        {
          name& tn (ts.name);
          scope& rs (*ts.root_scope);

          l5 ([&]{trace << "loading " << tn;});

          // Load the buildfile.
          //
          mif->load (mparams, rs, ts.buildfile, ts.out_base, ts.src_base, l);

          // Next search and match the targets. We don't want to start
          // building before we know how to for all the targets in this
          // operation batch.
          //
          const scope& bs (ctx.scopes.find_out (ts.out_base));

          // Find the target type and extract the extension.
          //
          auto rp (bs.find_target_type (tn, l));
          const target_type* tt (rp.first);
          optional<string>& e (rp.second);

          if (tt == nullptr)
            fail (l) << "unknown target type " << tn.type;

          if (load_only && !tt->is_a<alias> ())
            fail << "--load-only requires alias target";

          if (mif->search != nullptr)
          {
            // If the directory is relative, assume it is relative to work
            // (must be consistent with how we derived out_base above).
            //
            dir_path& d (tn.dir);

            try
            {
              if (d.relative ())
                d = work / d;

              d.normalize (true); // Actualize since came from command line.
            }
            catch (const invalid_path& e)
            {
              fail << "invalid target directory '" << e.path << "'";
            }

            if (ts.forwarded)
              d = rs.out_path () / d.leaf (rs.src_path ()); // Remap.

            // Figure out if this target is in the src tree.
            //
            dir_path out (ts.out_base != ts.src_base && d.sub (ts.src_base)
                          ? out_src (d, rs)
                          : dir_path ());

            mif->search (mparams,
                         rs, bs,
                         ts.buildfile,
                         target_key {tt, &d, &out, &tn.value, e},
                         l,
                         tgs);
          }
        } // target

        // Delay until after match in the --load-only mode (see below).
        //
        if (dump_load && !load_only)
          dump (ctx, nullopt /* action */);

        // Finally, match the rules and perform the operation.
        //
        if (pre_oid != 0)
        {
          l5 ([&]{trace << "start pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});

          if (mif->operation_pre != nullptr)
            mif->operation_pre (ctx, mparams, pre_oid); // Can't be translated.

          ctx.current_operation (*pre_oif, oif);

          if (oif->operation_pre != nullptr)
            oif->operation_pre (ctx, oparams, false /* inner */, l);

          if (pre_oif->operation_pre != nullptr)
            pre_oif->operation_pre (ctx, {}, true /* inner */, l);

          action a (mid, pre_oid, oid);

          {
#ifndef BUILD2_BOOTSTRAP
            result_printer p (ops, tgs, js);
#endif
            uint16_t diag (ops.structured_result_specified () ? 0 : 1);

            if (mif->match != nullptr)
              mif->match (mparams, a, tgs, diag, true /* progress */);

            if (dump_match_pre)
              dump (ctx, a);

            if (mif->execute != nullptr && !ctx.match_only)
              mif->execute (mparams, a, tgs, diag, true /* progress */);
          }

          if (pre_oif->operation_post != nullptr)
            pre_oif->operation_post (ctx, {}, true /* inner */);

          if (oif->operation_post != nullptr)
            oif->operation_post (ctx, oparams, false /* inner */);

          if (mif->operation_post != nullptr)
            mif->operation_post (ctx, mparams, pre_oid);

          l5 ([&]{trace << "end pre-operation batch " << pre_oif->name
                        << ", id " << static_cast<uint16_t> (pre_oid);});

          tgs.reset ();
        }

        ctx.current_operation (*oif, outer_oif);

        if (outer_oif != nullptr && outer_oif->operation_pre != nullptr)
          outer_oif->operation_pre (ctx, oparams, false /* inner */, l);

        if (oif->operation_pre != nullptr)
          oif->operation_pre (ctx,
                              outer_oif == nullptr ? oparams : values {},
                              true /* inner */,
                              l);

        action a (mid, oid, oif->outer_id);

        {
#ifndef BUILD2_BOOTSTRAP
          result_printer p (ops, tgs, js);
#endif
          uint16_t diag (ops.structured_result_specified () ? 0 : 2);

          if (mif->match != nullptr)
            mif->match (mparams, a, tgs, diag, true /* progress */);

          if (dump_match)
            dump (ctx, a);

          if (mif->execute != nullptr && !ctx.match_only)
            mif->execute (mparams, a, tgs, diag, true /* progress */);
        }

        if (oif->operation_post != nullptr)
          oif->operation_post (ctx,
                               outer_oif == nullptr ? oparams : values {},
                               true /* inner */);

        if (outer_oif != nullptr && outer_oif->operation_post != nullptr)
          outer_oif->operation_post (ctx, oparams, false /* inner */);

        if (post_oid != 0)
        {
          tgs.reset ();

          l5 ([&]{trace << "start post-operation batch " << post_oif->name
                        << ", id " << static_cast<uint16_t> (post_oid);});

          if (mif->operation_pre != nullptr)
            mif->operation_pre (ctx, mparams, post_oid); // Can't be translated.

          ctx.current_operation (*post_oif, oif);

          if (oif->operation_pre != nullptr)
            oif->operation_pre (ctx, oparams, false /* inner */, l);

          if (post_oif->operation_pre != nullptr)
            post_oif->operation_pre (ctx, {}, true /* inner */, l);

          action a (mid, post_oid, oid);

          {
#ifndef BUILD2_BOOTSTRAP
            result_printer p (ops, tgs, js);
#endif
            uint16_t diag (ops.structured_result_specified () ? 0 : 1);

            if (mif->match != nullptr)
              mif->match (mparams, a, tgs, diag, true /* progress */);

            if (dump_match_post)
              dump (ctx, a);

            if (mif->execute != nullptr && !ctx.match_only)
              mif->execute (mparams, a, tgs, diag, true /* progress */);
          }

          if (post_oif->operation_post != nullptr)
            post_oif->operation_post (ctx, {}, true /* inner */);

          if (oif->operation_post != nullptr)
            oif->operation_post (ctx, oparams, false /* inner */);

          if (mif->operation_post != nullptr)
            mif->operation_post (ctx, mparams, post_oid);

          l5 ([&]{trace << "end post-operation batch " << post_oif->name
                        << ", id " << static_cast<uint16_t> (post_oid);});
        }

        if (dump_load && load_only)
          dump (ctx, nullopt /* action */);

        if (mif->operation_post != nullptr)
          mif->operation_post (ctx, mparams, oid);

        l5 ([&]{trace << "end operation batch " << oif->name
                      << ", id " << static_cast<uint16_t> (oid);});
      } // operation

      if (mid != 0)
      {
        if (mif->meta_operation_post != nullptr)
          mif->meta_operation_post (ctx, mparams);

        l5 ([&]{trace << "end meta-operation batch " << mif->name
                      << ", id " << static_cast<uint16_t> (mid);});
      }

      if (lifted == nullptr && skip == 0)
        ++mit;
    } // meta-operation

#ifndef BUILD2_BOOTSTRAP
    if (ops.structured_result_specified () &&
        ops.structured_result () == structured_result_format::json)
    {
      js.end_array ();
      cout << endl;
    }
#endif

    phase_switch_contention += (pctx->phase_mutex.contention +
                                pctx->phase_mutex.contention_load);
  }
  catch (const failed&)
  {
    // Diagnostics has already been issued.
    //
    r = 1;
  }

  // Shutdown the scheduler and print statistics.
  //
  scheduler::stat st (sched.shutdown ());

  // In our world we wait for all the tasks to complete, even in case of a
  // failure (see, for example, wait_guard).
  //
  assert (st.task_queue_remain == 0);

  if (ops.stat ())
  {
    text << '\n'
         << "build statistics:" << "\n\n"
         << "  thread_max_active       " << st.thread_max_active     << '\n'
         << "  thread_max_total        " << st.thread_max_total      << '\n'
         << "  thread_helpers          " << st.thread_helpers        << '\n'
         << "  thread_max_waiting      " << st.thread_max_waiting    << '\n'
         << '\n'
         << "  task_queue_depth        " << st.task_queue_depth      << '\n'
         << "  task_queue_full         " << st.task_queue_full       << '\n'
         << '\n'
         << "  wait_queue_slots        " << st.wait_queue_slots      << '\n'
         << "  wait_queue_collisions   " << st.wait_queue_collisions << '\n'
         << '\n'
         << "  phase_switch_contention " << phase_switch_contention << '\n';
  }

  return r;
}

int
main (int argc, char* argv[])
{
  return build2::main (argc, argv);
}
