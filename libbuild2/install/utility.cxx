// file      : libbuild2/install/utility.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/install/utility.hxx>

#include <libbuild2/variable.hxx>
#include <libbuild2/diagnostics.hxx>

namespace build2
{
  namespace install
  {
    const scope*
    install_scope (const target& t)
    {
      context& ctx (t.ctx);

      // Note: go straight for the public variable pool.
      //
      const variable& var (*ctx.var_pool.find ("config.install.scope"));

      if (const string* s = cast_null<string> (ctx.global_scope[var]))
      {
        if (*s == "project")
          return &t.root_scope ();
        else if (*s == "bundle")
          return &t.bundle_scope ();
        else if (*s == "strong")
          return &t.strong_scope ();
        else if (*s == "weak")
          return &t.weak_scope ();
        else if (*s != "global")
          fail << "invalid " << var << " value '" << *s << "'";
      }

      return nullptr;
    }

    bool
    filter_entry (const scope& rs,
                  const dir_path& base,
                  const path& leaf,
                  entry_type type)
    {
      assert (type != entry_type::unknown &&
              (type == entry_type::directory) == leaf.empty ());

      const filters* fs (cast_null<filters> (rs["install.filter"]));

      if (fs == nullptr || fs->empty ())
        return true;

      tracer trace ("install::filter");

      // Parse, resolve, and apply each filter in order.
      //
      // If redoing all this work for every entry proves too slow, we can
      // consider some form of caching (e.g., on the per-project basis).
      //
      auto i (fs->begin ());

      bool negate (false);
      if (i->first == "!")
      {
        negate = true;
        ++i;
      }

      size_t limit (0); // See below.

      for (auto e (fs->end ()); i != e; ++i)
      {
        const pair<string, optional<string>>& kv (*i);

        path k;
        try
        {
          k = path (kv.first);

          if (k.absolute ())
            k.normalize ();
        }
        catch (const invalid_path&)
        {
          fail << "invalid path '" << kv.first << "' in config.install.filter "
               << "value";
        }

        bool v;
        {
          const string& s (kv.second ? *kv.second : string ());

          size_t p (s.find (','));

          if (s.compare (0, p, "true") == 0)
            v = true;
          else if (s.compare (0, p, "false") == 0)
            v = false;
          else
            fail << "expected true or false instead of '" << string (s, 0, p)
                 << "' in config.install.filter value" << endf;

          if (p != string::npos)
          {
            if (s.compare (p + 1, string::npos, "symlink") == 0)
            {
              if (type != entry_type::symlink)
                continue;
            }
            else
              fail << "unknown modifier '" << string (s, p + 1) << "' in "
                   << "config.install.filter value";
          }
        }

        // @@ TODO (see below for all the corner cases). Note that in a sense
        //    we already have the match file in any subdirectory support via
        //    simple patterns so perhaps this is not worth the trouble. Or we
        //    could support some limited form (e.g., `**` should be in the
        //    last component). But it may still be tricky to determine if
        //    it is a sub-filter.
        //
        if (path_pattern_recursive (k))
          fail << "recursive wildcard pattern '" << kv.first << "' in "
               << "config.install.filter value";

        if (k.simple () && !k.to_directory ())
        {
          // Simple name/pattern matched against the leaf.
          //
          // @@ What if it is `**`?
          //
          if (path_pattern (k))
          {
            if (!path_match (leaf, k))
              continue;
          }
          else
          {
            if (k != leaf)
              continue;
          }
        }
        else
        {
          // Split into directory and leaf.
          //
          // @@ What if leaf is `**`?
          //
          dir_path d;
          if (k.to_directory ())
          {
            d = path_cast<dir_path> (move (k));
            k = path (); // No leaf.
          }
          else
          {
            d = k.directory ();
            k.make_leaf ();
          }

          // Resolve relative directory.
          //
          // Note that this resolution is potentially project-specific (that
          // is, different projects may have different install.* locaitons).
          //
          // Note that if the first component is/contains a wildcard (e.g.,
          // `*/`), then the resulution will fail, which feels correct (what
          // does */ mean?).
          //
          if (d.relative ())
          {
            // @@ Strictly speaking, this should be base, not root scope.
            //
            d = resolve_dir (rs, move (d));
          }

          // Return the number of path components in the path.
          //
          auto path_comp = [] (const path& p)
          {
            size_t n (0);
            for (auto i (p.begin ()); i != p.end (); ++i)
              ++n;
            return n;
          };

          // We need the sub() semantics but which uses pattern match instead
          // of equality for the prefix. Looks like chopping off the path and
          // calling path_match() on that is the best we can do.
          //
          // @@ Assumes no `**` components.
          //
          auto path_sub = [&path_comp] (const dir_path& ent,
                                        const dir_path& pat,
                                        size_t n = 0)
          {
            if (n == 0)
              n = path_comp (pat);

            dir_path p;
            for (auto i (ent.begin ()); n != 0 && i != ent.end (); --n, ++i)
              p.combine (*i, i.separator ());

            return path_match (p, pat);
          };

          // The following checks should continue on no match and fall through
          // to return.
          //
          if (k.empty ()) // Directory.
          {
            // Directories have special semantics.
            //
            // Consider this sequence of filters:
            //
            //   include/x86_64-linux-gnu/@true
            //   include/x86_64-linux-gnu/details/@false
            //   include/@false
            //
            // It seems the semantics we want is that only subcomponent
            // filters should apply. Maybe remember the latest matched
            // directory as a current limit? But perhaps we don't need to
            // remember the directory itself but the number of path
            // components?
            //
            // I guess for patterns we will use the actual matched directory,
            // not the pattern, to calculate the limit? @@ Because we
            // currently don't support `**`, we for now can count components
            // in the pattern.

            // Check if this is a sub-filter.
            //
            size_t n (path_comp (d));
            if (n <= limit)
              continue;

            if (path_pattern (d))
            {
              if (!path_sub (base, d, n))
                continue;
            }
            else
            {
              if (!base.sub (d))
                continue;
            }

            if (v)
            {
              limit = n;
              continue; // Continue looking for sub-filters.
            }
          }
          else
          {
            if (path_pattern (d))
            {
              if (!path_sub (base, d))
                continue;
            }
            else
            {
              if (!base.sub (d))
                continue;
            }

            if (path_pattern (k))
            {
              // @@ Does not handle `**`.
              //
              if (!path_match (leaf, k))
                continue;
            }
            else
            {
              if (k != leaf)
                continue;
            }
          }
        }

        if (negate)
          v = !v;

        l4 ([&]{trace << (base / leaf)
                      << (v ? " included by " : " excluded by ")
                      << kv.first << '@' << *kv.second;});
        return v;
      }

      return !negate;
    }
  }
}
