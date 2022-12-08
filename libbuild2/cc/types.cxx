// file      : libbuild2/cc/types.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/cc/types.hxx>

#include <libbuild2/cc/utility.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    const string header_group_all            ("all");
    const string header_group_all_importable ("all-importable");
    const string header_group_std            ("std");
    const string header_group_std_importable ("std-importable");

    // Find the position where the group should be inserted unless the group
    // is already there.
    //
    using groups = importable_headers::groups;

    static inline optional<groups::const_iterator>
    find_angle (const groups& gs, const string& g)
    {
      for (auto i (gs.begin ()); i != gs.end (); ++i)
      {
        // After last angle-bracket file.
        //
        if (i->front () != '<' || i->back () != '>' || path_pattern (*i))
          return i;

        if (*i == g)
          return nullopt;
      }

      return gs.begin ();
    }

    static inline optional<groups::const_iterator>
    find_angle_pattern (const groups& gs, const string& g)
    {
      for (auto i (gs.begin ()); i != gs.end (); ++i)
      {
        // After last angle-bracket file pattern.
        //
        if (i->front () != '<' || i->back () != '>')
          return i;

        if (*i == g)
          return nullopt;
      }

      return gs.begin ();
    }

    auto importable_headers::
    insert_angle (const dir_paths& sys_hdr_dirs,
                   const string& s) -> pair<const path, groups>*
    {
      assert (s.front () == '<' && s.back () == '>');

      // First see if it has already been inserted.
      //
      auto i (group_map.find (s));
      if (i == group_map.end ())
      {
        path f (s, 1, s.size () - 2);

        path p; // Reuse the buffer.
        for (const dir_path& d: sys_hdr_dirs)
        {
          if (file_exists ((p = d, p /= f),
                           true /* follow_symlinks */,
                           true /* ignore_errors */))
            goto found;
        }

        return nullptr;

      found:
        normalize_header (p);

        // Note that it's possible this header has already been entered as
        // part of a different group.
        //
        auto j (header_map.emplace (move (p), groups {}).first);

        if (auto p = find_angle (j->second, s))
          j->second.insert (*p, s);

        i = group_map.emplace (s, reinterpret_cast<uintptr_t> (&*j)).first;
      }

      return reinterpret_cast<pair<const path, groups>*> (i->second);
    }

    auto importable_headers::
    insert_angle (path p, const string& s) -> pair<const path, groups>&
    {
      assert (s.front () == '<' && s.back () == '>');

      // First see if it has already been inserted.
      //
      auto i (group_map.find (s));
      if (i == group_map.end ())
      {
        // Note that it's possible this header has already been entered as
        // part of a different group.
        //
        auto j (header_map.emplace (move (p), groups {}).first);

        if (auto p = find_angle (j->second, s))
          j->second.insert (*p, s);

        i = group_map.emplace (s, reinterpret_cast<uintptr_t> (&*j)).first;
      }

      return *reinterpret_cast<pair<const path, groups>*> (i->second);
    }

    size_t importable_headers::
    insert_angle_pattern (const dir_paths& sys_hdr_dirs, const string& pat)
    {
      tracer trace ("importable_headers::insert_angle_pattern");

      assert (pat.front () == '<' && pat.back () == '>' && path_pattern (pat));

      // First see if it has already been inserted.
      //
      auto i (group_map.find (pat));
      if (i == group_map.end ())
      {
        path f (pat, 1, pat.size () - 2);

        struct data
        {
          uintptr_t       n;
          const string&   pat;
          const dir_path* dir;
        } d {0, pat, nullptr};

        auto process = [&d, this] (path&& pe, const string&, bool interm)
        {
          if (interm)
            return true;

          path p (*d.dir / pe);
          normalize_header (p);

          string s (move (pe).string ());
          s.insert (0, 1, '<');
          s.push_back ('>');

          // Note that it's possible this header has already been entered as
          // part of a different group.
          //
          auto j (header_map.emplace (move (p), groups {}).first);

          if (auto p = find_angle (j->second, s))
            j->second.insert (*p, move (s));

          if (auto p = find_angle_pattern (j->second, d.pat))
            j->second.insert (*p, d.pat);

          d.n++;
          return true;
        };

        for (const dir_path& dir: sys_hdr_dirs)
        {
          d.dir = &dir;

          try
          {
            path_search (
              f,
              process,
              dir,
              path_match_flags::follow_symlinks,
              [&trace] (const dir_entry& de)
              {
                l5 ([&]{trace << "skipping inaccessible/dangling entry "
                              << de.base () / de.path ();});
                return true;
              });
          }
          catch (const system_error& e)
          {
            fail << "unable to scan " << dir << ": " << e;
          }
        }

        i = group_map.emplace (pat, d.n).first;
      }

      return static_cast<size_t> (i->second);
    }
  }
}
