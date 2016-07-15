// file      : build2/cxx/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/module>

#include <butl/triplet>

#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/config/utility>
#include <build2/install/utility>

#include <build2/bin/target>

#include <build2/cxx/link>
#include <build2/cxx/guess>
#include <build2/cxx/target>
#include <build2/cxx/compile>
#include <build2/cxx/install>
#include <build2/cxx/utility>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cxx
  {
    bool
    init (scope& r,
          scope& b,
          const location& loc,
          unique_ptr<module_base>&,
          bool first,
          bool,
          const variable_map& config_hints)
    {
      tracer trace ("cxx::init");
      l5 ([&]{trace << "for " << b.out_path ();});

      // Enter module variables.
      //
      if (first)
      {
        auto& v (var_pool);

        // Note: some overridable, some not.
        //
        v.insert<path>    ("config.cxx",          true);
        v.insert<strings> ("config.cxx.poptions", true);
        v.insert<strings> ("config.cxx.coptions", true);
        v.insert<strings> ("config.cxx.loptions", true);
        v.insert<strings> ("config.cxx.libs",     true);

        v.insert<strings> ("cxx.poptions");
        v.insert<strings> ("cxx.coptions");
        v.insert<strings> ("cxx.loptions");
        v.insert<strings> ("cxx.libs");

        v.insert<strings> ("cxx.export.poptions");
        v.insert<strings> ("cxx.export.coptions");
        v.insert<strings> ("cxx.export.loptions");
        v.insert<strings> ("cxx.export.libs");

        v.insert<string> ("cxx.std",              true);
      }

      // Configure.
      //

      assert (config_hints.empty ()); // We don't known any hints.

      // config.cxx.{p,c,l}options
      // config.cxx.libs
      //
      // These are optional. We also merge them into the corresponding
      // cxx.* variables.
      //
      // The merging part gets a bit tricky if this module has already
      // been loaded in one of the outer scopes. By doing the straight
      // append we would just be repeating the same options over and
      // over. So what we are going to do is only append to a value if
      // it came from this scope. Then the usage for merging becomes:
      //
      // cxx.coptions = <overridable options> # Note: '='.
      // using cxx
      // cxx.coptions += <overriding options> # Note: '+='.
      //
      b.assign ("cxx.poptions") += cast_null<strings> (
        config::optional (r, "config.cxx.poptions"));

      b.assign ("cxx.coptions") += cast_null<strings> (
        config::optional (r, "config.cxx.coptions"));

      b.assign ("cxx.loptions") += cast_null<strings> (
        config::optional (r, "config.cxx.loptions"));

      b.assign ("cxx.libs") += cast_null<strings> (
        config::optional (r, "config.cxx.libs"));

      // Configuration hints for the bin module. They will only be used on the
      // first loading of the bin module (for this project) so we only
      // populate them on our first loading.
      //
      variable_map bin_hints;

      // config.cxx
      //
      if (first)
      {
        auto p (config::required (r, "config.cxx", path ("g++")));

        // Figure out which compiler we are dealing with, its target, etc.
        //
        const path& cxx (cast<path> (p.first));
        compiler_info ci (guess (cxx, cast_null<strings> (r["cxx.coptions"])));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          //@@ Print project out root or name? Don't print if unnamed?

          text << "cxx\n"
               << "  exe        " << cxx << '\n'
               << "  id         " << ci.id << '\n'
               << "  version    " << ci.version.string << '\n'
               << "  major      " << ci.version.major << '\n'
               << "  minor      " << ci.version.minor << '\n'
               << "  patch      " << ci.version.patch << '\n'
               << "  build      " << ci.version.build << '\n'
               << "  signature  " << ci.signature << '\n'
               << "  checksum   " << ci.checksum << '\n'
               << "  target     " << ci.target;
        }

        r.assign<string> ("cxx.id") = ci.id.string ();
        r.assign<string> ("cxx.id.type") = move (ci.id.type);
        r.assign<string> ("cxx.id.variant") = move (ci.id.variant);

        r.assign<string> ("cxx.version") = move (ci.version.string);
        r.assign<uint64_t> ("cxx.version.major") = ci.version.major;
        r.assign<uint64_t> ("cxx.version.minor") = ci.version.minor;
        r.assign<uint64_t> ("cxx.version.patch") = ci.version.patch;
        r.assign<string> ("cxx.version.build") = move (ci.version.build);

        r.assign<string> ("cxx.signature") = move (ci.signature);
        r.assign<string> ("cxx.checksum") = move (ci.checksum);

        // While we still have the original, compiler-reported target, see if
        // we can derive a binutils program pattern.
        //
        // BTW, for GCC we also get gcc-{ar,ranlib} which add support for the
        // LTO plugin though it seems more recent GNU binutils (2.25) are able
        // to load the plugin when needed automatically. So it doesn't seem we
        // should bother trying to support this on our end (the way we could
        // do it is by passing config.bin.{ar,ranlib} as hints).
        //
        string pattern;

        if (cast<string> (r["cxx.id"]) == "msvc")
        {
          // If the compiler name is/starts with 'cl' (e.g., cl.exe, cl-14),
          // then replace it with '*' and use it as a pattern for lib, link,
          // etc.
          //
          if (cxx.size () > 2)
          {
            const string& l (cxx.leaf ().string ());
            size_t n (l.size ());

            if (n >= 2 &&
                (l[0] == 'c' || l[0] == 'C') &&
                (l[1] == 'l' || l[1] == 'L') &&
                (n == 2 || l[2] == '.' || l[2] == '-'))
            {
              path p (cxx.directory ());
              p /= "*";
              p += l.c_str () + 2;
              pattern = move (p).string ();
            }
          }
        }
        else
        {
          // When cross-compiling the whole toolchain is normally prefixed
          // with the target triplet, e.g., x86_64-w64-mingw32-{g++,ar,ld}.
          //
          const string& t (ci.target);
          size_t n (t.size ());

          if (cxx.size () > n + 1)
          {
            const string& l (cxx.leaf ().string ());

            if (l.size () > n + 1 && l.compare (0, n, t) == 0 && l[n] == '-')
            {
              path p (cxx.directory ());
              p /= t;
              p += "-*";
              pattern = move (p).string ();
            }
          }
        }

        if (!pattern.empty ())
          bin_hints.assign ("config.bin.pattern") = move (pattern);

        // Split/canonicalize the target.
        //

        // Did the user ask us to use config.sub?
        //
        if (ops.config_sub_specified ())
        {
          ci.target = run<string> (ops.config_sub (),
                                   ci.target.c_str (),
                                   [] (string& l) {return move (l);});
          l5 ([&]{trace << "config.sub target: '" << ci.target << "'";});
        }

        try
        {
          string canon;
          triplet t (ci.target, canon);

          l5 ([&]{trace << "canonical target: '" << canon << "'; "
                        << "class: " << t.class_;});

          // Pass the target we extracted from the C++ compiler as a config
          // hint to the bin module.
          //
          bin_hints.assign ("config.bin.target") = canon;

          // Enter as cxx.target.{cpu,vendor,system,version,class}.
          //
          r.assign<string> ("cxx.target") = move (canon);
          r.assign<string> ("cxx.target.cpu") = move (t.cpu);
          r.assign<string> ("cxx.target.vendor") = move (t.vendor);
          r.assign<string> ("cxx.target.system") = move (t.system);
          r.assign<string> ("cxx.target.version") = move (t.version);
          r.assign<string> ("cxx.target.class") = move (t.class_);
        }
        catch (const invalid_argument& e)
        {
          // This is where we suggest that the user specifies --config-sub to
          // help us out.
          //
          fail << "unable to parse compiler target '" << ci.target << "': "
               << e.what () <<
            info << "consider using the --config-sub option";
        }
      }

      const string& cid (cast<string> (r["cxx.id"]));
      const string& tsys (cast<string> (r["cxx.target.system"]));
      const string& tclass (cast<string> (r["cxx.target.class"]));

      // Initialize the bin module. Only do this if it hasn't already been
      // loaded so that we don't overwrite user's bin.* settings.
      //
      if (!cast_false<bool> (b["bin.loaded"]))
        load_module ("bin", r, b, loc, false, bin_hints);

      // Verify bin's target matches ours.
      //
      {
        const string& bt (cast<string> (r["bin.target"]));
        const string& ct (cast<string> (r["cxx.target"]));

        if (bt != ct)
          fail (loc) << "bin and cxx module target platform mismatch" <<
            info << "bin.target is " << bt <<
            info << "cxx.target is " << ct;
      }

      // In the VC world you link things directly with link.exe.
      //
      if (cid == "msvc")
      {
        if (!cast_false<bool> (b["bin.ld.loaded"]))
          load_module ("bin.ld", r, b, loc, false, bin_hints);
      }

      // If our target is MinGW, then we will need the resource compiler
      // (windres) in order to embed the manifest.
      //
      if (tsys == "mingw32")
      {
        if (!cast_false<bool> (b["bin.rc.loaded"]))
          load_module ("bin.rc", r, b, loc, false, bin_hints);
      }

      // Register target types.
      //
      {
        auto& t (b.target_types);

        t.insert<h> ();
        t.insert<c> ();

        t.insert<cxx> ();
        t.insert<hxx> ();
        t.insert<ixx> ();
        t.insert<txx> ();
      }

      // Register rules.
      //
      {
        using namespace bin;

        auto& r (b.rules);

        r.insert<obja> (perform_update_id, "cxx.compile", compile::instance);

        r.insert<obja> (perform_update_id, "cxx.compile", compile::instance);
        r.insert<obja> (perform_clean_id, "cxx.compile", compile::instance);

        r.insert<objso> (perform_update_id, "cxx.compile", compile::instance);
        r.insert<objso> (perform_clean_id, "cxx.compile", compile::instance);

        r.insert<exe> (perform_update_id, "cxx.link", link::instance);
        r.insert<exe> (perform_clean_id, "cxx.link", link::instance);

        r.insert<liba> (perform_update_id, "cxx.link", link::instance);
        r.insert<liba> (perform_clean_id, "cxx.link", link::instance);

        r.insert<libso> (perform_update_id, "cxx.link", link::instance);
        r.insert<libso> (perform_clean_id, "cxx.link", link::instance);

        // Register for configure so that we detect unresolved imports
        // during configuration rather that later, e.g., during update.
        //
        r.insert<obja> (configure_update_id, "cxx.compile", compile::instance);
        r.insert<objso> (configure_update_id, "cxx.compile", compile::instance);

        r.insert<exe> (configure_update_id, "cxx.link", link::instance);
        r.insert<liba> (configure_update_id, "cxx.link", link::instance);
        r.insert<libso> (configure_update_id, "cxx.link", link::instance);

        //@@ Should we check if install module was loaded (see bin)?
        //
        r.insert<exe> (perform_install_id, "cxx.install", install::instance);
        r.insert<liba> (perform_install_id, "cxx.install", install::instance);
        r.insert<libso> (perform_install_id, "cxx.install", install::instance);
      }

      // Configure "installability" of our target types.
      //
      using namespace install;

      install_path<hxx> (b, dir_path ("include")); // Into install.include.
      install_path<ixx> (b, dir_path ("include"));
      install_path<txx> (b, dir_path ("include"));
      install_path<h>   (b, dir_path ("include"));

      // Create additional target types for certain target platforms.
      //
      if (tclass == "windows")
      {
        const target_type& dll (b.derive_target_type<file> ("dll").first);
        install_path (dll, b, dir_path ("bin"));

        if (cid == "msvc")
        {
          const target_type& pdb (b.derive_target_type<file> ("pdb").first);
          install_path (pdb, b, dir_path ("bin"));
          install_mode (pdb, b, "644");
        }
      }

      return true;
    }
  }
}
