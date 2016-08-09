// file      : build2/cc/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cc/module>

#include <iomanip> // left, setw()

#include <butl/triplet>

#include <build2/scope>
#include <build2/context>
#include <build2/diagnostics>

#include <build2/bin/target>

#include <build2/config/utility>
#include <build2/install/utility>

#include <build2/cc/guess>

using namespace std;
using namespace butl;

namespace build2
{
  namespace cc
  {
    void config_module::
    init (scope& r,
          scope& b,
          const location& loc,
          bool first,
          const variable_map&)
    {
      tracer trace (x, "config_init");

      // Configure.
      //
      string pattern; // Toolchain pattern.

      if (first)
      {
        const variable& config_c_coptions (var_pool["config.cc.coptions"]);

        // config.x
        //
        auto p (config::required (r, config_x, path (x_default)));

        // Figure out which compiler we are dealing with, its target, etc.
        //
        const path& xc (cast<path> (p.first));
        compiler_info ci (
          guess (x_lang,
                 xc,
                 cast_null<strings> (r[config_c_coptions]),
                 cast_null<strings> (r[config_x_coptions])));

        // If this is a new value (e.g., we are configuring), then print the
        // report at verbosity level 2 and up (-v).
        //
        if (verb >= (p.second ? 2 : 3))
        {
          text << x << ' ' << project (r) << '@' << r.out_path () << '\n'
               << "  " << left << setw (11) << x << xc << '\n'
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

        r.assign (x_id) = ci.id.string ();
        r.assign (x_id_type) = move (ci.id.type);
        r.assign (x_id_variant) = move (ci.id.variant);

        r.assign (x_version) = move (ci.version.string);
        r.assign (x_version_major) = ci.version.major;
        r.assign (x_version_minor) = ci.version.minor;
        r.assign (x_version_patch) = ci.version.patch;
        r.assign (x_version_build) = move (ci.version.build);

        r.assign (x_signature) = move (ci.signature);
        r.assign (x_checksum) = move (ci.checksum);

        pattern = move (ci.pattern);

        // Split/canonicalize the target. First see if the user asked us to
        // use config.sub.
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

          // Enter as x.target.{cpu,vendor,system,version,class}.
          //
          r.assign (x_target) = move (canon);
          r.assign (x_target_cpu) = move (t.cpu);
          r.assign (x_target_vendor) = move (t.vendor);
          r.assign (x_target_system) = move (t.system);
          r.assign (x_target_version) = move (t.version);
          r.assign (x_target_class) = move (t.class_);
        }
        catch (const invalid_argument& e)
        {
          // This is where we suggest that the user specifies --config-sub to
          // help us out.
          //
          fail << "unable to parse " << x_lang << "compiler target '"
               << ci.target << "': " << e.what () <<
            info << "consider using the --config-sub option";
        }
      }

      // config.x.{p,c,l}options
      // config.x.libs
      //
      // These are optional. We also merge them into the corresponding
      // x.* variables.
      //
      // The merging part gets a bit tricky if this module has already
      // been loaded in one of the outer scopes. By doing the straight
      // append we would just be repeating the same options over and
      // over. So what we are going to do is only append to a value if
      // it came from this scope. Then the usage for merging becomes:
      //
      // x.coptions = <overridable options> # Note: '='.
      // using x
      // x.coptions += <overriding options> # Note: '+='.
      //
      b.assign (x_poptions) += cast_null<strings> (
        config::optional (r, config_x_poptions));

      b.assign (x_coptions) += cast_null<strings> (
        config::optional (r, config_x_coptions));

      b.assign (x_loptions) += cast_null<strings> (
        config::optional (r, config_x_loptions));

      b.assign (x_libs) += cast_null<strings> (
        config::optional (r, config_x_libs));

      // Load cc.config.
      //
      if (!cast_false<bool> (b["cc.config.loaded"]))
      {
        // Prepare configuration hints. They are only used on the first load
        // of cc.config so we only populate them on our first load.
        //
        variable_map h;
        if (first)
        {
          h.assign ("config.cc.id") = cast<string> (r[x_id]);
          h.assign ("config.cc.target") = cast<string> (r[x_target]);
          if (!pattern.empty ())
            h.assign ("config.cc.pattern") = move (pattern);
        }

        load_module ("cc.config", r, b, loc, false, h);
      }
      else if (first)
      {
        // If cc.config is already loaded, verify its configuration matched
        // ours since it could have been loaded by another c-family module.
        //
        auto check = [&r, &loc, this](const char* cv,
                                      const variable& xv,
                                      const char* w)
        {
          const string& c (cast<string> (r[cv]));
          const string& x (cast<string> (r[xv]));

          if (c != x)
            fail (loc) << "cc and " << x << " module " << w << " mismatch" <<
              info << cv << " is " << c <<
              info << xv.name << " is " << x;
        };

        // Note that we don't require that patterns match. Presumably, if the
        // toolchain id and target are the same, then where exactly the tools
        // (e.g., ar) come from doesn't really matter.
        //
        check ("cc.id",     x_id,     "toolchain id");
        check ("cc.target", x_target, "target");
      }
    }

    void module::
    init (scope& r,
          scope& b,
          const location& loc,
          bool,
          const variable_map&)
    {
      tracer trace (x, "init");

      // Load cc.core. Besides other things, this will load bin (core) plus
      // extra bin.* modules we may need.
      //
      if (!cast_false<bool> (b["cc.core.loaded"]))
        load_module ("cc.core", r, b, loc);

      // Register target types and configure their "installability".
      //
      {
        using namespace install;

        auto& t (b.target_types);

        t.insert (x_src);

        // Install headers into install.include.
        //
        for (const target_type* const* ht (x_hdr); *ht != nullptr; ++ht)
        {
          t.insert (**ht);
          install_path (**ht, b, dir_path ("include"));
        }
      }

      // Register rules.
      //
      {
        using namespace bin;

        auto& r (b.rules);

        // We register for configure so that we detect unresolved imports
        // during configuration rather that later, e.g., during update.
        //
        // @@ Should we check if install module was loaded (see bin)?
        //
        compile& cr (*this);
        link&    lr (*this);
        install& ir (*this);

        r.insert<obje> (perform_update_id,   x_compile, cr);
        r.insert<obje> (perform_clean_id,    x_compile, cr);
        r.insert<obje> (configure_update_id, x_compile, cr);

        r.insert<exe>  (perform_update_id,   x_link, lr);
        r.insert<exe>  (perform_clean_id,    x_link, lr);
        r.insert<exe>  (configure_update_id, x_link, lr);

        r.insert<exe>  (perform_install_id,  x_install, ir);

        // Only register static object/library rules if the bin.ar module is
        // loaded (by us or by the user).
        //
        if (cast_false<bool> (b["bin.ar.loaded"]))
        {
          r.insert<obja> (perform_update_id,   x_compile, cr);
          r.insert<obja> (perform_clean_id,    x_compile, cr);
          r.insert<obja> (configure_update_id, x_compile, cr);

          r.insert<liba> (perform_update_id,   x_link, lr);
          r.insert<liba> (perform_clean_id,    x_link, lr);
          r.insert<liba> (configure_update_id, x_link, lr);

          r.insert<liba> (perform_install_id,  x_install, ir);
        }

        r.insert<objs> (perform_update_id,   x_compile, cr);
        r.insert<objs> (perform_clean_id,    x_compile, cr);
        r.insert<objs> (configure_update_id, x_compile, cr);

        r.insert<libs> (perform_update_id,   x_link, lr);
        r.insert<libs> (perform_clean_id,    x_link, lr);
        r.insert<libs> (configure_update_id, x_link, lr);

        r.insert<libs> (perform_install_id,  x_install, ir);
      }
    }
  }
}
