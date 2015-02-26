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
  match_impl (target& t)
  {
    for (auto tt (&t.type ());
         tt != nullptr && !t.recipe ();
         tt = tt->base)
    {
      auto i (rules.find (tt->id));

      if (i == rules.end ()) // No rules registered for this target type.
        continue;

      const auto& rules (i->second); // Hint map.

      string hint; // @@ TODO
      auto rs (rules.find_prefix (hint));

      for (auto i (rs.first); i != rs.second; ++i)
      {
        const string& n (i->first);
        const rule& ru (i->second);

        void* m;
        {
          auto g (
            make_exception_guard (
              [](target& t, const string& n)
              {
                info << "while matching rule " << n << " for target " << t;
              },
              t, n));

          m = ru.match (t, hint);
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

              m1 = ru1.match (t, hint);
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

            t.recipe (ru.apply (t, m));
            break;
          }
          else
            dr << info << "use rule hint to disambiguate this match";
        }
      }
    }

    if (!t.recipe ())
      fail << "no rule to update target " << t;
  }

  void
  search_and_match (target& t)
  {
    for (prerequisite& p: t.prerequisites)
    {
      if (p.target == nullptr)
        search (p);

      match (*p.target);
    }
  }

  target_state
  update (target& t)
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

        ts = t.recipe () (t);
        assert (ts != target_state::unknown && ts != target_state::failed);
        t.state (ts);
        return ts;
      }
    case target_state::uptodate:
    case target_state::updated:
      return ts;
    case target_state::failed:
      throw failed ();
    }
  }

  target_state
  update_prerequisites (target& t)
  {
    target_state ts (target_state::uptodate);

    for (const prerequisite& p: t.prerequisites)
    {
      assert (p.target != nullptr);

      if (update (*p.target) != target_state::uptodate)
        ts = target_state::updated;
    }

    return ts;
  }

  bool
  update_prerequisites (target& t, const timestamp& mt)
  {
    bool u (mt == timestamp_nonexistent);

    for (const prerequisite& p: t.prerequisites)
    {
      assert (p.target != nullptr);
      target& pt (*p.target);

      target_state ts (update (pt));

      if (!u)
      {
        // If this is an mtime-based target, then compare timestamps.
        //
        if (auto mpt = dynamic_cast<const mtime_target*> (&pt))
        {
          timestamp mp (mpt->mtime ());

          // What do we do if timestamps are equal? This can happen, for
          // example, on filesystems that don't have subsecond resolution.
          // There is not much we can do here except detect the case where
          // the prerequisite was updated in this run which means the
          // target must be out of date.
          //
          if (mt < mp || mt == mp && ts == target_state::updated)
            u = true;
        }
        else
        {
          // Otherwise we assume the prerequisite is newer if it was updated.
          //
          if (ts == target_state::updated)
            u = true;
        }
      }
    }

    return u;
  }
}
