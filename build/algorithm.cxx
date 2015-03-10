// file      : build/algorithm.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/algorithm>

#include <memory>   // unique_ptr
#include <utility>  // move
#include <cassert>

#include <build/path>
#include <build/scope>
#include <build/target>
#include <build/prerequisite>
#include <build/rule>
#include <build/search>
#include <build/utility>
#include <build/diagnostics>

using namespace std;

namespace build
{
  target&
  search (prerequisite& p)
  {
    assert (p.target == nullptr);

    if (target* t = p.type.search (p))
      return *t;

    return create_new_target (p);
  }

  void
  match_impl (action a, target& t)
  {
    for (auto tt (&t.type ());
         tt != nullptr && !t.recipe (a);
         tt = tt->base)
    {
      auto i (current_rules->find (tt->id));

      if (i == current_rules->end () || i->second.empty ())
        continue; // No rules registered for this target type, try base.

      const auto& rules (i->second); // Hint map.

      // @@ TODO
      //
      // Different rules can be used for different operations (update
      // vs test is a good example). So, at some point, we will probably
      // have to support a list of hints or even an operation-hint map
      // (e.g., 'hint=cxx test=foo' if cxx supports the test operation
      // but we want the foo rule instead). This is also the place where
      // the '{build clean}=cxx' construct (which we currently do not
      // support) can come handy.
      //
      // Also, ignore the hint (that is most likely ment for a different
      // operation) if this is a unique match.
      //
      string hint;
      auto rs (rules.size () == 1
               ? make_pair (rules.begin (), rules.end ())
               : rules.find_prefix (hint));

      for (auto i (rs.first); i != rs.second; ++i)
      {
        const string& n (i->first);
        const rule& ru (i->second);

        void* m (nullptr);
        {
          auto g (
            make_exception_guard (
              [](target& t, const string& n)
              {
                info << "while matching rule " << n << " for target " << t;
              },
              t, n));

          m = ru.match (a, t, hint);
        }

        if (m != nullptr)
        {
          // Do the ambiguity test.
          //
          bool ambig (false);

          diag_record dr;

          for (++i; i != rs.second; ++i)
          {
            const string& n1 (i->first);
            const rule& ru1 (i->second);

            void* m1;
            {
              auto g (
                make_exception_guard (
                  [](target& t, const string& n1)
                  {
                    info << "while matching rule " << n1 << " for target "
                         << t;
                  },
                  t, n1));

              m1 = ru1.match (a, t, hint);
            }

            if (m1 != nullptr)
            {
              if (!ambig)
              {
                dr << fail << "multiple rules matching target " << t
                   << info << "rule " << n << " matches";
                ambig = true;
              }

              dr << info << "rule " << n1 << " also matches";
            }
          }

          if (!ambig)
          {
            auto g (
              make_exception_guard (
                [](target& t, const string& n)
                {
                  info << "while applying rule " << n << " for target " << t;
                },
                t, n));

            t.recipe (a, ru.apply (a, t, m));
            break;
          }
          else
            dr << info << "use rule hint to disambiguate this match";
        }
      }
    }

    if (!t.recipe (a))
      fail << "no rule to update target " << t;
  }

  void
  search_and_match (action a, target& t)
  {
    for (prerequisite& p: t.prerequisites)
    {
      if (p.target == nullptr)
        search (p);

      match (a, *p.target);
    }
  }

  target_state
  execute (action a, target& t)
  {
    // Implementation with some multi-threading ideas in mind.
    //
    switch (target_state ts = t.state ())
    {
    case target_state::unknown:
      {
        t.state (target_state::failed); // So the rule can just throw.

        auto g (
          make_exception_guard (
            [](target& t){info << "while updating target " << t;},
            t));

        ts = t.recipe (a) (a, t);
        assert (ts != target_state::unknown && ts != target_state::failed);
        t.state (ts);
        return ts;
      }
    case target_state::unchanged:
    case target_state::changed:
      return ts;
    case target_state::failed:
      throw failed ();
    }
  }

  target_state
  execute_prerequisites (action a, target& t)
  {
    target_state ts (target_state::unchanged);

    for (const prerequisite& p: t.prerequisites)
    {
      assert (p.target != nullptr);

      if (execute (a, *p.target) != target_state::unchanged)
        ts = target_state::changed;
    }

    return ts;
  }

  bool
  execute_prerequisites (action a, target& t, const timestamp& mt)
  {
    bool e (mt == timestamp_nonexistent);

    for (const prerequisite& p: t.prerequisites)
    {
      assert (p.target != nullptr);
      target& pt (*p.target);

      target_state ts (execute (a, pt));

      if (!e)
      {
        // If this is an mtime-based target, then compare timestamps.
        //
        if (auto mpt = dynamic_cast<const mtime_target*> (&pt))
        {
          timestamp mp (mpt->mtime ());

          // What do we do if timestamps are equal? This can happen, for
          // example, on filesystems that don't have subsecond resolution.
          // There is not much we can do here except detect the case where
          // the prerequisite was changed in this run which means the
          // action must be executed on the target as well.
          //
          if (mt < mp || (mt == mp && ts == target_state::changed))
            e = true;
        }
        else
        {
          // Otherwise we assume the prerequisite is newer if it was changed.
          //
          if (ts == target_state::changed)
            e = true;
        }
      }
    }

    return e;
  }
}
