// file      : build2/pkgconfig/init.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/pkgconfig/init.hxx>

#include <build2/scope.hxx>
#include <build2/target.hxx>
#include <build2/variable.hxx>
#include <build2/diagnostics.hxx>

#include <build2/config/utility.hxx>
#include <build2/install/utility.hxx>

#include <build2/pkgconfig/target.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace pkgconfig
  {
    bool
    config_init (scope& rs,
                 scope& bs,
                 const location& l,
                 unique_ptr<module_base>&,
                 bool,
                 bool optional,
                 const variable_map& hints)
    {
      tracer trace ("pkgconfig::config_init");
      l5 ([&]{trace << "for " << bs.out_path ();});

      // We only support root loading (which means there can only be one).
      //
      if (&rs != &bs)
        fail (l) << "pkgconfig.config module must be loaded in project root";

      // Enter variables.
      //
      // config.pkgconfig.target is a hint.
      //
      auto& vp (var_pool.rw (rs));

      const variable& c_x (vp.insert<path> ("config.pkgconfig", true));
      const variable& x_path (vp.insert<process_path> ("pkgconfig.path"));

      const variable& c_x_tgt (
        vp.insert<target_triplet> ("config.pkgconfig.target"));

      // Configure.
      //

      // Adjust module priority (between compilers and binutils).
      //
      config::save_module (rs, "pkgconfig", 325);

      process_path pp;
      bool new_val (false); // Set any new values?

      auto p (config::omitted (rs, c_x));

      if (p.first)
      {
        const path& x (cast<path> (p.first));

        try
        {
          // If this is a user-specified value, then it's non-optional.
          //
          pp = process::path_search (x, true);
          new_val = p.second;
        }
        catch (const process_error& e)
        {
          fail << "unable to execute " << x << ": " << e;
        }
      }

      string d; // Default name (pp.initial may be its shallow copy).

      // If we have a target hint, then next try <triplet>-pkg-config.
      //
      if (pp.empty ())
      {
        if (const auto* t = cast_null<target_triplet> (hints[c_x_tgt]))
        {
          d = t->string ();
          d += "-pkg-config";

          l5 ([&]{trace << "trying " << d;});
          pp = process::try_path_search (d, true);
        }
      }

      // Finallly, try just pkg-config.
      //
      if (pp.empty ())
      {
        d = "pkg-config";

        l5 ([&]{trace << "trying " << d;});
        pp = process::try_path_search (d, true);
      }

      bool conf (!pp.empty ());

      if (!conf && !optional)
        fail (l) << "unable to find pkg-config program";

      // Config report.
      //
      if (verb >= (new_val ? 2 : 3))
      {
        diag_record dr (text);
        dr << "pkgconfig " << project (rs) << '@' << rs.out_path () << '\n';

        if (conf)
          dr << "  pkg-config " << pp;
        else
          dr << "  pkg-config " << "not found, leaving unconfigured";
      }

      if (conf)
        rs.assign (x_path) = move (pp);

      return conf;
    }

    bool
    init (scope& rs,
          scope&,
          const location& loc,
          unique_ptr<module_base>&,
          bool,
          bool optional,
          const variable_map& hints)
    {
      tracer trace ("pkgconfig::init");
      l5 ([&]{trace << "for " << rs.out_path ();});

      // Load pkgconfig.config.
      //
      bool conf (true);
      if (!cast_false<bool> (rs["pkgconfig.config.loaded"]))
      {
        if (!load_module (rs, rs, "pkgconfig.config", loc, optional, hints))
          conf = false;
      }
      else if (!cast_false<bool> (rs["pkgconfig.config.configured"]))
      {
        if (!optional)
          fail << "pkgconfig module could not be configured" <<
            info << "re-run with -V option for more information";

        conf = false;
      }

      // Register the target type and configure its default "installability".
      //
      // Note that we do it whether we found pkg-config or not since these are
      // used to produce .pc files which we do regardless.
      //
      rs.target_types.insert<pca> ();
      rs.target_types.insert<pcs> ();

      if (cast_false<bool> (rs["install.loaded"]))
        install::install_path<pc> (rs, dir_path ("pkgconfig"));

      return conf;
    }
  }
}
