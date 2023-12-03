// file      : libbuild2/test/common.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/common.hxx>

#include <libbuild2/target.hxx>
#include <libbuild2/algorithm.hxx>

#include <libbuild2/script/timeout.hxx>

#include <libbuild2/test/module.hxx>

using namespace std;

namespace build2
{
  namespace test
  {
    // Determine if we have the target (first), id path (second), or both (in
    // which case we also advance the iterator).
    //
    static pair<const name*, const name*>
    sense (names::const_iterator& i)
    {
      const name* tn (nullptr);
      const name* pn (nullptr);

      if (i->pair)
      {
        tn = &*i++;
        pn = &*i;
      }
      else
      {
        // If it has a type (exe{hello}) or a directory (basics/), then
        // we assume it is a target.
        //
        (i->typed () || !i->dir.empty () ? tn : pn) = &*i;
      }

      // Validate the target.
      //
      if (tn != nullptr)
      {
        if (tn->qualified ())
          fail << "project-qualified target '" << *tn << " in config.test";
      }

      // Validate the id path.
      //
      if (pn != nullptr)
      {
        if (!pn->simple () || pn->empty ())
          fail << "invalid id path '" << *pn << " in config.test";
      }

      return make_pair (tn, pn);
    }

    bool common::
    pass (const target& a) const
    {
      if (test_ == nullptr)
        return true;

      // We need to "enable" aliases that "lead up" to the targets we are
      // interested in. So see if any target is in a subdirectory of this
      // alias.
      //
      // If we don't see any targets (e.g., only id paths), then we assume all
      // targets match and therefore we always pass.
      //
      bool r (true);

      // Directory part from root to this alias (the same in src and out).
      //
      const dir_path d (a.out_dir ().leaf (root_->out_path ()));

      for (auto i (test_->begin ()); i != test_->end (); ++i)
      {
        if (const name* n = sense (i).first)
        {
          // Reset result to false if no match (but we have seen a target).
          //
          r = n->dir.sub (d);

          // See test() below for details on this special case.
          //
          if (!r && !n->typed ())
            r = d.sub (n->dir);

          if (r)
            break;
        }
      }

      return r;
    }

    bool common::
    test (const target& t) const
    {
      if (test_ == nullptr)
        return true;

      // If we don't see any targets (e.g., only id paths), then we assume
      // all of them match.
      //
      bool r (true);

      // Directory part from root to this alias (the same in src and out).
      //
      const dir_path d (t.out_dir ().leaf (root_->out_path ()));
      const target_type& tt (t.type ());

      for (auto i (test_->begin ()); i != test_->end (); ++i)
      {
        if (const name* n = sense (i).first)
        {
          // Reset result to false if no match (but we have seen a target).
          //

          // When specifying a directory, for example, config.tests=tests/,
          // one would intuitively expect that all the tests under it will
          // run. But that's not what will happen with the below test: while
          // the dir{tests/} itself will match, any target underneath won't.
          // So we are going to handle this type if a target specially by
          // making it match any target in or under it.
          //
          // Note that we only do this for tests/, not dir{tests/} since it is
          // not always the semantics that one wants. Sometimes one may want
          // to run tests (scripts) just for the tests/ target but not for any
          // of its prerequisites. So dir{tests/} is a way to disable this
          // special logic.
          //
          // Note: the same code as in test() below.
          //
          if (!n->typed ())
            r = d.sub (n->dir);
          else
            // First quickly and cheaply weed out names that cannot possibly
            // match. Only then search for a target (as if it was a
            // prerequisite), which can be expensive.
            //
            // We cannot specify an src target in config.test since we used
            // the pair separator for ids. As a result, we search for both
            // out and src targets.
            //
            r =
              t.name == n->value &&                   // Name matches.
              tt.name == n->type &&                   // Target type matches.
              d == n->dir        &&                   // Directory matches.
              search_existing (*n, *root_) == &t;

          if (r)
            break;
        }
      }

      return r;
    }

    bool common::
    test (const target& t, const path& id) const
    {
      if (test_ == nullptr)
        return true;

      // If we don't see any id paths (e.g., only targets), then we assume
      // all of them match.
      //
      bool r (true);

      // Directory part from root to this alias (the same in src and out).
      //
      const dir_path d (t.out_dir ().leaf (root_->out_path ()));
      const target_type& tt (t.type ());

      for (auto i (test_->begin ()); i != test_->end (); ++i)
      {
        auto p (sense (i));

        if (const name* n = p.second)
        {
          // If there is a target, check that it matches ours.
          //
          if (const name* n = p.first)
          {
            // Note: the same code as in test() above.
            //
            bool r;

            if (!n->typed ())
              r = d.sub (n->dir);
            else
              r =
                t.name == n->value &&
                tt.name == n->type &&
                d == n->dir        &&
                search_existing (*n, *root_) == &t;

            if (!r)
              continue; // Not our target.
          }

          // If the id (group) "leads up" to what we want to run or we
          // (group) lead up to the id, then match.
          //
          const path p (n->value);

          // Reset result to false if no match (but we have seen an id path).
          //
          if ((r = p.sub (id) || id.sub (p)))
            break;
        }
      }

      return r;
    }

    optional<timestamp> common::
    operation_deadline () const
    {
      if (!operation_timeout)
        return nullopt;

      duration::rep r (operation_deadline_.load (memory_order_consume));

      if (r == timestamp_unknown_rep)
      {
        duration::rep t (timestamp (system_clock::now () + *operation_timeout).
                         time_since_epoch ().count ());

        if (operation_deadline_.compare_exchange_strong (r,
                                                         t,
                                                         memory_order_release,
                                                         memory_order_consume))
          r = t;
      }

      return timestamp (duration (r));
    }

    // Helpers.
    //
    optional<timestamp>
    operation_deadline (const target& t)
    {
      optional<timestamp> r;

      for (const scope* s (t.base_scope ().root_scope ());
           s != nullptr;
           s = s->parent_scope ()->root_scope ())
      {
        if (auto* m = s->find_module<module> (module::name))
          r = earlier (r, m->operation_deadline ());
      }

      return r;
    }

    optional<duration>
    test_timeout (const target& t)
    {
      optional<duration> r;

      for (const scope* s (t.base_scope ().root_scope ());
           s != nullptr;
           s = s->parent_scope ()->root_scope ())
      {
        if (auto* m = s->find_module<module> (module::name))
          r = earlier (r, m->test_timeout);
      }

      return r;
    }

    optional<timestamp>
    test_deadline (const target& t)
    {
      optional<timestamp> r (operation_deadline (t));

      if (optional<duration> d = test_timeout (t))
        r = earlier (r, system_clock::now () + *d);

      return r;
    }
  }
}
