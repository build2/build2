// file      : libbuild2/cc/compiledb.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/compiledb.hxx>

#include <cstring>  // strlen()
#include <iostream> // cout

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/parser.hxx>
#endif

#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/cc/module.hxx>

#include <libbuild2/cc/target.hxx>
#include <libbuild2/bin/target.hxx>

using namespace std;

namespace build2
{
  namespace cc
  {
    string compiledbs_key ("cc::compiledbs");

    // compiledb
    //
    compiledb::
    ~compiledb ()
    {
    }

    // Return true if this entry should be written to the database with the
    // specified name.
    //
    static bool
    filter (const scope& rs,
            const core_module& m,
            const string& name,
            const file& ot, const file& it)
    {
      tracer trace ("cc::compiledb_filter");

      bool r (true);
      const char* w (nullptr); // Why r is false.

      // First check if writing to this database is enabled.
      //
      // No filter means not enabled.
      //
      if (m.cdb_filter_ == nullptr)
      {
        r = false;
        w = "no database name filter";
      }
      else
      {
        // Iterate in reverse (so that later values override earlier) and take
        // the first name match.
        //
        r = false;
        for (const pair<optional<string>, bool>& p:
               reverse_iterate (*m.cdb_filter_))
        {
          if (!p.first || *p.first == name)
          {
            r = p.second;
            break;
          }
        }

        if (!r)
          w = "no match in database name filter";
      }

      // Verify the name is known in this amalgamation. Note that without
      // this check we may end up writing to unrelated databases in other
      // amalgamations (think linked configurations).
      //
      if (r)
      {
        r = false;
        for (const core_module* pm (&m);
             pm != nullptr;
             pm = pm->outer_module_)
        {
          const strings& ns (pm->cdb_names_);

          if (find (ns.begin (), ns.end (), name) != ns.end ())
          {
            r = true;
            break;
          }
        }

        if (!r)
          w = "database name unknown in amalgamation";
      }

      // Filter based on the output target.
      //
      // If there is no filter specified, then accept all targets.
      //
      if (r && m.cdb_filter_output_ != nullptr)
      {
        // If the filter is empty, then there is no match.
        //
        if (m.cdb_filter_output_->empty ())
        {
          r = false;
          w = "empty output target type filter";
        }
        else
        {
          const target_type& ott (ot.type ());

          // Iterate in reverse (so that later values override earlier) and
          // take the first name match.
          //
          r = false;
          for (const pair<optional<string>, string>& p:
                 reverse_iterate (*m.cdb_filter_output_))
          {
            if (p.first && *p.first != name)
              continue;

            using namespace bin;

            const string& n (p.second);

            if (ott.name == n || n == "target")
            {
              r = true;
            }
            //
            // Handle obj/bmi/hbmi{} groups ad hoc.
            //
            else if (n == "obj")
            {
              r = ott.is_a<obje> () || ott.is_a<objs> () || ott.is_a<obja> ();
            }
            else if (n == "bmi")
            {
              r = ott.is_a<bmie> () || ott.is_a<bmis> () || ott.is_a<bmia> ();
            }
            else if (n == "hbmi")
            {
              r = ott.is_a<hbmie> () || ott.is_a<hbmis> () || ott.is_a<hbmia> ();
            }
            else
            {
              // Handle the commonly-used, well-known targets directly (see
              // note in core_config_init() for why we cannot pre-lookup
              // them).
              //
              const target_type* tt (
                n == "obje"   ? &obje::static_type :
                n == "objs"   ? &objs::static_type :
                n == "obja"   ? &obja::static_type :
                n == "bmie"   ? &bmie::static_type :
                n == "bmis"   ? &bmis::static_type :
                n == "bmia"   ? &bmia::static_type :
                n == "hbmie"  ? &hbmie::static_type :
                n == "hbmis"  ? &hbmis::static_type :
                n == "hbmia"  ? &hbmia::static_type :
                rs.find_target_type (n));

              if (tt == nullptr)
                fail << "unknown target type '" << n << "' in "
                     << "config.cc.compiledb.filter.output value";

              r = ott.is_a (*tt);
            }

            if (r)
              break;
          }

          if (!r)
            w = "no match in output target type filter";
        }
      }

      // Filter based on the input target.
      //
      // If there is no filter specified, then accept all targets.
      //
      if (r && m.cdb_filter_input_ != nullptr)
      {
        // If the filter is empty, then there is no match.
        //
        if (m.cdb_filter_input_->empty ())
        {
          r = false;
          w = "empty input target type filter";
        }
        else
        {
          const target_type& itt (it.type ());

          // Iterate in reverse (so that later values override earlier) and
          // take the first name match.
          //
          r = false;
          for (const pair<optional<string>, string>& p:
                 reverse_iterate (*m.cdb_filter_input_))
          {
            if (p.first && *p.first != name)
              continue;

            const string& n (p.second);

            if (itt.name == n || n == "target")
              r = true;
            else
            {
              // The same optimization as above. Note: cxx{}, etc., are in the
              // cxx module so we have to look them up.
              //
              const target_type* tt (
                n == "c" ? &c::static_type :
                n == "m" ? &m::static_type :
                n == "S" ? &m::static_type :
                rs.find_target_type (n));

              if (tt == nullptr)
                fail << "unknown target type '" << n << "' in "
                     << "config.cc.compiledb.filter.input value";

              r = itt.is_a (*tt);
            }

            if (r)
              break;
          }

          if (!r)
            w = "no match in input target type filter";
        }
      }

      l6 ([&]
          {
            if (r)
              trace << "keep " << ot << " in " << name;
            else
              trace << "omit " << ot << " from " << name << ": " << w;
          });

      return r;
    }

    bool compiledb::
    match (const scope& bs,
           const file& ot, const path_type& op,
           const file& it,
           bool changed)
    {
      const compiledb_set* dbs (compiledbs (bs.ctx));

      if (dbs == nullptr || dbs->empty ())
        return false;

      const scope& rs (*bs.root_scope ());
      const auto* m (rs.find_module<core_module> (core_module::name));

      assert (m != nullptr);

      bool u (false);

      for (const unique_ptr<compiledb>& db: *dbs)
      {
        if (filter (rs, *m, db->name, ot, it))
          u = db->match (ot, op, changed) || u;
      }

      return u;
    }

    void compiledb::
    execute (const scope& bs,
             const file& ot, const path_type& op,
             const file& it, const path_type& ip,
             const process_path& cpath, const cstrings& args,
             const path_type& relo, const path_type& abso,
             const path_type& relm, const path_type& absm)
    {
      const compiledb_set* dbs (compiledbs (bs.ctx));

      if (dbs == nullptr || dbs->empty ())
        return;

      const scope& rs (*bs.root_scope ());
      const auto* m (rs.find_module<core_module> (core_module::name));

      assert (m != nullptr);

      assert (relo.empty () == abso.empty () &&
              relm.empty () == absm.empty ());

      for (const unique_ptr<compiledb>& db: *dbs)
      {
        if (filter (rs, *m, db->name, ot, it))
          db->execute (ot, op, it, ip, cpath, args, relo, abso, relm, absm);
      }
    }

    void
    compiledb_pre (context& ctx, action a, const action_targets&)
    {
      // Note: won't be registered if compiledbs is NULL or empty.
      //
      const compiledb_set* dbs (compiledbs (ctx));

      // Note: may be called directly with empty action_targets.

      assert (a.inner_action () == perform_update_id);

      tracer trace ("cc::compiledb_pre");

      bool nctx (ctx.nested_context ());

      l6 ([&]{trace << (nctx ? "nested" : "normal") << " context " << &ctx;});

      for (const unique_ptr<compiledb>& db: *dbs)
        db->pre (ctx);
    }

    void
    compiledb_post (context& ctx,
                    action a,
                    const action_targets& ts,
                    bool failed)
    {
      // Note: won't be registered if compiledbs is NULL or empty.
      //
      const compiledb_set* dbs (compiledbs (ctx));

      assert (a.inner_action () == perform_update_id);

      tracer trace ("cc::compiledb_post");

      bool nctx (ctx.nested_context ());

      l6 ([&]{trace << (nctx ? "nested" : "normal") << " context " << &ctx
                    << ", failed: " << failed;});

      for (const unique_ptr<compiledb>& db: *dbs)
        db->post (ctx, a, ts, failed);
    }

#ifndef BUILD2_BOOTSTRAP

    namespace json = butl::json;

    // compiledb_stdout
    //
    compiledb_stdout::
    compiledb_stdout (string n)
        : compiledb (move (n), path_type ()),
          state_ (state::init),
          nesting_ (0),
          js_ (cout, 0 /* indentation */, "" /* multi_value_separator */)
    {
    }

    void compiledb_stdout::
    pre (context&)
    {
      // If the previous operation batch failed, then we shouldn't be here.
      //
      assert (state_ != state::failed);

      // The nested contexts (used to build build system modules, update
      // during load targets) poses a problem: we can receive its callbacks
      // before the main context's or nested in the pre/post calls of the main
      // context (or both, in fact). Plus there may be multiple pre/post call
      // sequences corresponding to the nested contexts of both kinds. The
      // three distinct cases are (using module as an example):
      //
      // 1. Module is loaded as part of the initial buildfile load (e.g., from
      //    root.build) -- in this case we will observe module pre/post before
      //    the main context's pre/post.
      //
      //    In fact, to be precise, we will only observe them if cc is loaded
      //    before such a module.
      //
      // 2. Module is loaded via the interrupting load (e.g., from a directory
      //    buildfile that is loaded implicitly during match) -- in this case
      //    we will observe pre/post calls nested into the main context's
      //    pre/post.
      //
      // 3. The module context is used to build an ad hoc C++ recipe -- in
      //    this case we also get nested calls like in (2) since this happens
      //    during the recipe's match().
      //
      // One thing to keep in mind (and which we rely upon quite a bit below)
      // is that the main context's post will always be last (within any given
      // operation; there could be another for the subsequent operation in a
      // batch).
      //
      // Handling the nested calls case is relatively straightforward: we can
      // keep track and ignore all the nested calls.
      //
      // The before case is where things get complicated. We could "take" the
      // first nested context pre call and then wait until the main post,
      // unless we see a nested context post call with failed=true, in which
      // case there will be no further pre/post calls. There is, however, a
      // nuance: the module (again, as an example) is loaded and build for any
      // operation, not just update, which means that if the main operation is
      // not update (say, it's clean), we won't see any of the main context's
      // pre/post calls.
      //
      // The way we are going to resolve this problem is different for the
      // stdout and file implementations:
      //
      // For stdout we will just say that it should only be used with the
      // update operation. There is really no good reason to use it with
      // anything else anyway. See compiledb_stdout::post() for additional
      // details.
      //
      // For file we will rely on its persistence and simply close and reopen
      // the database for each pre/post sequence, the same way as if they were
      // separate operations in a batch.
      //
      if (nesting_++ != 0) // Nested pre() call.
        return;

      if (state_ == state::init) // First pre() call.
      {
        state_ = state::empty;
        cout << "[\n";
      }
    }

    bool compiledb_stdout::
    match (const file&, const path_type&, bool)
    {
      return true;
    }

    static inline const char*
    rel_to_abs (const char* a,
                const string& rs, const string& as,
                string& buf)
    {
      if (size_t rn = rs.size ())
      {
        size_t an (strlen (a));

        if (an >= rn && rs.compare (0, rn, a, rn) == 0)
        {
          if (an == rn)
            return as.c_str ();

          buf = as;
          buf.append (a + rn, an - rn);

          return buf.c_str ();
        }
      }

      return nullptr;
    }

    void compiledb_stdout::
    execute (const file&, const path_type& op,
             const file&, const path_type& ip,
             const process_path& cpath, const cstrings& args,
             const path_type& relo, const path_type& abso,
             const path_type& relm, const path_type& absm)
    {
      const string& ro (relo.string ());
      const string& ao (abso.string ());

      const string& rm (relm.string ());
      const string& am (absm.string ());

      mlock l (mutex_);

      switch (state_)
      {
      case state::full:
        {
          cout << ",\n";
          break;
        }
      case state::empty:
        {
          state_ = state::full;
          break;
        }
      case state::failed:
        return;
      case state::init:
        assert (false);
        return;
      }

      try
      {
        // Duplicate what we have in the file implementation (instead of
        // factoring it out to something common) in case here we need to
        // adjust things (change order, omit some values; for example to
        // accommodate broken consumers). We have this freedom here but not
        // there.
        //
        js_.begin_object ();
        {
          js_.member ("output", op.string ());
          js_.member ("file", ip.string ());

          js_.member_begin_array ("arguments");
          {
            string buf; // Reuse.
            for (auto b (args.begin ()), i (b), e (args.end ());
                 i != e && *i != nullptr;
                 ++i)
            {
              const char* r;

              if (i == b)
                r = cpath.effect_string ();
              else
              {
                // Untranslate relative paths back to absolute.
                //
                const char* a (*i);

                if ((r = rel_to_abs (a, ro, ao, buf)) == nullptr &&
                    (r = rel_to_abs (a, rm, am, buf)) == nullptr)
                  r = a;
              }

              js_.value (r);
            }
          }
          js_.end_array ();

          js_.member ("directory", work.string ());
        }
        js_.end_object ();
      }
      catch (const invalid_json_output& e)
      {
        // There is no way (nor reason; the output will most likely be invalid
        // anyway) to reuse the failed json serializer so make sure we ignore
        // all the subsequent callbacks.
        //
        state_ = state::failed;

        l.unlock ();

        fail << "invalid compilation database json output: " << e;
      }
    }

    void compiledb_stdout::
    post (context& ctx, action, const action_targets&, bool failed)
    {
      assert (nesting_ != 0);
      if (--nesting_ != 0) // Nested post() call.
        return;

      bool nctx (ctx.nested_context ());

      switch (state_)
      {
      case state::empty:
      case state::full:
        {
          // If this is a nested context's post, wait for the main context's
          // post (last) unless the nested operation failed (in which case
          // there will be no main pre/post).
          //
          // Note that there is no easy way to diagnose the case where we
          // won't get the main pre/post calls. Instead, we will just produce
          // invalid JSON (array won't be closed). In a somewhat hackish way,
          // this actually makes the `b [-n] clean update` sequence work: we
          // will take the pre() call from clean and the main post() from
          // update.
          //
          if (nctx && !failed)
            return;

          if (state_ == state::full)
            cout << '\n';

          cout << "]\n";
          break;
        }
      case state::failed:
        return;
      case state::init:
        assert (false);
      }

      state_ = state::init;
    }

    // compiledb_file
    //
    compiledb_file::
    compiledb_file (string n, path_type p)
        : compiledb (move (n), move (p)),
          state_ (state::closed),
          nesting_ (0)
    {
    }

    void compiledb_file::
    pre (context&)
    {
      // If the previous operation batch failed, then we shouldn't be here.
      //
      assert (state_ != state::failed);

      // See compiledb_stdout::pre() for background on dealing with the nested
      // context. Here are some file-specific nuances:
      //
      // We are going to load the database on the first pre call and flush
      // (but not close) it on the matching post. Flushing means that we will
      // update the file but still keep the in-memory state, in case there is
      // another pre/post session coming. This is both a performance
      // optimization but also the way we handle prunning no longer present
      // entries, which gets tricky across multiple pre/post sessions (see
      // post() for details).
      //
      if (nesting_++ != 0) // Nested pre() call.
        return;

      if (state_ == state::closed) // First pre() call.
      {
        // Load the contents of the file if it exists, marking all the entries
        // as (presumed) absent.
        //
        if (exists (path))
        {
          uint64_t line (1);
          try
          {
            ifdstream ifs (path, ifdstream::badbit);

            // Parse the top-level array manually (see post() for the expected
            // format).
            //
            auto throw_invalid_input = [] (const string& d)
            {
              throw invalid_json_input ("", 0, 1, 0, d);
            };

            enum {first, second, next, last, end} s (first);

            for (string l; !eof (getline (ifs, l)); line++)
            {
              switch (s)
              {
              case first:
                {
                  if (l != "[")
                    throw_invalid_input ("beginning of array expected");

                  s = second;
                  continue;
                }
              case second:
                {
                  if (l == "]")
                  {
                    s = end;
                    continue;
                  }

                  s = next;
                }
                // Fall through.
              case next:
                {
                  if (!l.empty () && l.back () == ',')
                    l.pop_back ();
                  else
                    s = last;

                  break;
                }
              case last:
                {
                  if (l != "]")
                    throw_invalid_input ("end of array expected");

                  s = end;
                  continue;
                }
              case end:
                {
                  throw_invalid_input ("junk after end of array");
                }
              }

              // Parse just the output target path, which must come first.
              //
              json_parser jp (l, "" /* name */);

              jp.next_expect (json_event::begin_object);
              string op (move (jp.next_expect_member_string ("output")));

              auto r (db_.emplace (move (op), entry {entry_status::absent, l}));
              if (!r.second)
                throw_invalid_input (
                  "duplicate output value '" + r.first->first + '\'');
            }

            if (s != end)
              throw_invalid_input ("corrupt input text");
          }
          catch (const invalid_json_input& e)
          {
            state_ = state::failed;

            location l (path, line, e.column);
            fail (l) << "invalid compilation database json input: " << e <<
              info << "remove this file if it was produced by a different tool";
          }
          catch (const io_error& e)
          {
            state_ = state::failed;
            fail << "unable to read " << path << ": " << e;
          }
        }

        absent_ = db_.size ();
        changed_ = false;

        state_ = state::open;
      }
    }

    bool compiledb_file::
    match (const file&, const path_type& op, bool changed)
    {
      mlock l (mutex_);

      switch (state_)
      {
      case state::open:
        break;
      case state::failed:
        return false;
      case state::closed:
        assert (false);
        return false;
      }

      // Mark an existing entry as present or changed. And if one does not
      // exist, then (for now) as missing.
      //
      auto i (db_.find (op.string ()));

      if (i != db_.end ())
      {
        entry& e (i->second);

        // Note: we can end up with present entries via the nested context
        // (see post() below). And we can see changed entries in a subsequent
        // nested context.
        //
        switch (e.status)
        {
        case entry_status::present:
        case entry_status::changed:
          assert (!changed);
          break;
        case entry_status::absent:
          {
            e.status = changed ? entry_status::changed : entry_status::present;

            absent_--;
            changed_ = changed_ || (e.status == entry_status::changed);
            break;
          }
        case entry_status::missing:
          assert (false);
        }

        return false;
      }
      else
      {
        db_.emplace (op.string (), entry {entry_status::missing, string ()});

        changed_ = true;

        return true;
      }
    }

    void compiledb_file::
    execute (const file&, const path_type& op,
             const file&, const path_type& ip,
             const process_path& cpath, const cstrings& args,
             const path_type& relo, const path_type& abso,
             const path_type& relm, const path_type& absm)
    {
      const string& ro (relo.string ());
      const string& ao (abso.string ());

      const string& rm (relm.string ());
      const string& am (absm.string ());

      mlock l (mutex_);

      switch (state_)
      {
      case state::open:
        break;
      case state::failed:
        return;
      case state::closed:
        assert (false);
        return;
      }

      auto i (db_.find (op.string ()));

      // We should have had the match() call before execute().
      //
      assert (i != db_.end () && i->second.status != entry_status::absent);

      entry& e (i->second);

      if (e.status == entry_status::present) // Present and unchanged.
        return;

      // The entry is either missing or changed.
      //
      try
      {
        e.json.clear ();
        json_buffer_serializer js (e.json, 0 /* indentation */);

        js.begin_object ();
        {
          js.member ("output", op.string ()); // Note: must come first.
          js.member ("file", ip.string ());

          js.member_begin_array ("arguments");
          {
            string buf; // Reuse.
            for (auto b (args.begin ()), i (b), e (args.end ());
                 i != e && *i != nullptr;
                 ++i)
            {
              const char* r;

              if (i == b)
                r = cpath.effect_string ();
              else
              {
                // Untranslate relative paths back to absolute.
                //
                const char* a (*i);

                if ((r = rel_to_abs (a, ro, ao, buf)) == nullptr &&
                    (r = rel_to_abs (a, rm, am, buf)) == nullptr)
                  r = a;
              }

              js.value (r);
            }
          }
          js.end_array ();

          js.member ("directory", work.string ());
        }
        js.end_object ();
      }
      catch (const invalid_json_output& e)
      {
        // There is no way (nor reason; the output will most likely be invalid
        // anyway) to reuse the failed json serializer so make sure we ignore
        // all the subsequent callbacks.
        //
        state_ = state::failed;

        l.unlock ();

        fail << "invalid compilation database json output: " << e;
      }

      e.status = entry_status::changed;
    }

    void compiledb_file::
    post (context& ctx, action a, const action_targets& ts, bool failed)
    {
      assert (nesting_ != 0);
      if (--nesting_ != 0) // Nested post() call.
        return;

      switch (state_)
      {
      case state::open:
        break;
      case state::failed:
        return;
      case state::closed:
        assert (false);
        return;
      }

      bool nctx (ctx.nested_context ());

      tracer trace ("cc::compiledb_file::post");

      // See if we need to update the file.
      //
      if (changed_)
        l6 ([&]{trace << "updating due to missing/changed entries: " << path;});

      // Don't prune the stale entries if the operation failed since we may
      // not have gotten to execute some of them.
      //
      // And if this is a nested context's post, then also don't prune the
      // stale entries, instead waiting for the main context's post (if there
      // will be one; this means we will only prune on update).
      //
      // Actually, this pruning business is even trickier than that: if we
      // are not updating the entire project (say, rather only a subdirectory
      // or even a specific target), then we will naturally not get any
      // match/execute calls for targets of this project that don't get pulled
      // into this build. Which means that we cannot just prune entries that
      // we did not match/execute. It feels the correct semantics is to only
      // prune the entries if they are in a subdirectory of the dir{} targets
      // which we are building.
      //
      // Another dimension of this is an update-for-x pre-operation. In this
      // case we may be only updating a subset of targets (for example, only
      // tests plus what they pull, in case of update-for-test). So feels like
      // we should not prune in pre-operations (see GH #498).
      //
      // What do we do about the nested context, where we update a specific
      // target, say libs{} for module context? We could use its directory
      // instead but that may lead to undesirable results. For example, if
      // there are unit tests in the same directory, we will end up dropping
      // their entries. It feels like the correct approach is to just ignore
      // nested context's entries entirely. If someone wants to prune the
      // compilation database of, say, a module, they will just need to update
      // it directly (i.e., via the main context). Note that we cannot apply
      // the same "simplification" to the changed entries since we will only
      // observe the change once.
      //
      // Note also that the update-during-load context is only used when the
      // main context's action is other than perform_update. And if it is,
      // then such targets are updated directly in the main context, which can
      // happen in two different ways: initial load and interrupting load (see
      // update_during_load() for details). Interrupting load is covered by
      // the above logic since the update during load happens within the
      // normal pre/post calls. Initial load, however, is tricky: we end up
      // with an independent sequence(s) of pre/post calls before the main
      // one, essentially as-if we had a batch of updates except that we don't
      // actually start a new operation and which means that the target will
      // already have been matched/executed and we will consider it as absent.
      // So we handle that case specially by checking if the pre/post call is
      // updated during initial load and if so not marking the targets as
      // absent.
      //
      bool absent (false);

      if (!failed && !nctx && !a.outer () && absent_ != 0)
      {
        // Pre-scan the entries and drop the appropriate absent ones.
        //
        for (auto i (db_.begin ()); i != db_.end (); )
        {
          const entry& e (i->second);

          if (e.status == entry_status::absent)
          {
            // Absent entries should be rare enough during the normal
            // development that we don't need to bother with caching the
            // directories.
            //
            bool a (false);
            for (const action_target& at: ts)
            {
              const target& t (at.as<target> ());
              if (t.is_a<dir> ())
              {
                const string& p (i->first);
                const string& d (t.out_dir ().string ());

                if (path_traits::sub (p.c_str (), p.size (),
                                      d.c_str (), d.size ()))
                {
                  // Remove this entry from the in-memory state so that it
                  // matches the file state.
                  //
                  i = db_.erase (i);
                  --absent_;
                  a = absent = true;
                  break;
                }
              }
            }

            if (a)
              continue;
          }

          ++i;
        }
      }

      if (absent)
        l6 ([&]{trace << "updating due to absent entries: " << path;});

      try
      {
        auto_rmfile rm;
        ofdstream ofs;

        bool u (changed_ || absent); // Update the file.

        if (u)
        {
          rm = auto_rmfile (path);
          ofs.open (path);

          // We parse the top-level array manually (see pre() above) and the
          // expected format is as follows:
          //
          // [
          // {"output":...},
          // ...
          // {"output":...}
          // ]
          //
          ofs.write ("[\n", 2);
        }

        // Iterate over the entries resetting their status and writing them to
        // the file if necessary.
        //
        bool first (true);
        for (auto& p: db_)
        {
          entry& e (p.second);

          // First sort out the status also skipping appropriate entries.
          //
          switch (e.status)
          {
          case entry_status::absent:
            {
              // This is an absent entry that we should keep (see pre-scan
              // above).
              //
              break;
            }
          case entry_status::missing:
            {
              // This should only happen if this operation has failed (see
              // also below) or we are in the match-only mode.
              //
              assert (failed || ctx.match_only);
              continue;
            }
          case entry_status::present:
          case entry_status::changed:
            {
              // This is tricky: if this is a nested context, then we don't
              // want to mark the entries as absent since they will then get
              // dropped by the main operation context.
              //
              // Or if we are updating during (initial) load (see above).
              //
              if (nctx || ctx.update_during_load == 1)
                e.status = entry_status::present;
              else
              {
                // Note: this is necessary for things to work across multiple
                // operations in a batch.
                //
                e.status = entry_status::absent;
                absent_++;
              }
            }
          }

          if (u)
          {
            if (first)
              first = false;
            else
              ofs.write (",\n", 2);

            ofs.write (e.json.c_str (), e.json.size ());
          }
        }

        if (u)
        {
          ofs.write (first ? "]\n" : "\n]\n", first ? 2 : 3);

          ofs.close ();
          rm.cancel ();
        }
      }
      catch (const io_error& e)
      {
        state_ = state::failed;
        fail << "unable to write to " << path << ": " << e;
      }

      // If this operation has failed, then our state may not be accurate
      // (e.g., entries with missing status) but we also don't expect any
      // further pre calls. Let's change out state to failed as a sanity
      // check.
      //
      if (failed)
        state_ = state::failed;
      else
        changed_ = false;

      // Note: keep in the open state (see pre() for details).
    }

#endif // BUILD2_BOOTSTRAP
  }
}
