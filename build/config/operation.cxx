// file      : build/config/operation.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/config/operation>

#include <fstream>

#include <build/scope>
#include <build/context>
#include <build/filesystem>
#include <build/diagnostics>

using namespace std;

namespace build
{
  namespace config
  {
    static const path build_dir ("build");
    static const path bootstrap_dir ("build/bootstrap");

    static const path config_file ("build/config.build");
    static const path src_root_file ("build/bootstrap/src-root.build");

    // configure
    //
    static operation_id
    configure_operation_pre (operation_id o)
    {
      // Don't translate default to update. In our case unspecified
      // means configure everything.
      //
      return o;
    }

    static void
    save_src_root (const path& out_root, const path& src_root)
    {
      path f (out_root / src_root_file);

      if (verb >= 1)
        text << "config::save_src_root " << f.string ();
      else
        text << "save " << f;

      try
      {
        ofstream ofs (f.string ());
        if (!ofs.is_open ())
          fail << "unable to open " << f;

        ofs.exceptions (ofstream::failbit | ofstream::badbit);

        //@@ TODO: quote path
        //
        ofs << "# Created automatically by the config module." << endl
            << "#" << endl
            << "src_root = " << src_root.string () << '/' << endl;
      }
      catch (const ios_base::failure&)
      {
        fail << "failed to write to " << f;
      }
    }

    static void
    save_config (const path& out_root)
    {
      path f (out_root / config_file);

      if (verb >= 1)
        text << "config::save_config " << f.string ();
      else
        text << "save " << f;

      try
      {
        ofstream ofs (f.string ());
        if (!ofs.is_open ())
          fail << "unable to open " << f;

        ofs.exceptions (ofstream::failbit | ofstream::badbit);

        // Save all the variables in the config. namespace that are set
        // on the project's root scope.
        //
        scope& r (scopes.find (out_root));

        /*
        r.variables["config"] = "default interactive";
        r.variables["config.cxx"] = "g++-4.9";
        r.variables["config.cxx.options"] = "-O3 -g";
        */

        for (auto p (r.variables.find_namespace ("config"));
             p.first != p.second;
             ++p.first)
        {
          const variable& var (p.first->first);
          const value& val (*p.first->second);

          //@@ TODO: assuming list
          //
          const list_value& lv (dynamic_cast<const list_value&> (val));

          ofs << var.name << " = " << lv.data << endl;
          text << var.name << " = " << lv.data;
        }
      }
      catch (const ios_base::failure&)
      {
        fail << "failed to write to " << f;
      }
    }

    static void
    configure_execute (action a, const action_targets& ts)
    {
      tracer trace ("configure_execute");

      for (void* v: ts)
      {
        target& t (*static_cast<target*> (v));
        scope& s (scopes.find (t.dir));

        const path& out_root (s["out_root"].as<const path&> ());
        const path& src_root (s["src_root"].as<const path&> ());

        // Make sure the directories exist.
        //
        if (out_root != src_root)
        {
          mkdir (out_root);
          mkdir (out_root / build_dir);
        }

        mkdir (out_root / bootstrap_dir);

        // We distinguish between a complete configure and operation-
        // specific.
        //
        if (a.operation () == default_id)
        {
          level4 ([&]{trace << "completely configuring " << out_root;});

          // Save src-root.build unless out_root is the same as src.
          //
          if (out_root != src_root)
            save_src_root (out_root, src_root);

          // Save config.build.
          //
          save_config (out_root);
        }
        else
        {
        }
      }
    }

    meta_operation_info configure {
      "configure",
      nullptr, // meta-operation pre
      &configure_operation_pre,
      &load,   // normal load
      &match,  // normal match
      &configure_execute,
      nullptr, // operation post
      nullptr  // meta-operation post
    };

    // disfigure
    //
    static operation_id
    disfigure_operation_pre (operation_id o)
    {
      // Don't translate default to update. In our case unspecified
      // means disfigure everything.
      //
      return o;
    }

    static void
    disfigure_load (const path& bf,
                    scope&,
                    const path&,
                    const path&,
                    const location&)
    {
      tracer trace ("disfigure_load");
      level5 ([&]{trace << "skipping " << bf;});
    }

    static void
    disfigure_match (action a,
                     scope& root,
                     const target_key& tk,
                     const location& l,
                     action_targets& ts)
    {
      tracer trace ("disfigure_match");
      level5 ([&]{trace << "collecting " << root.path ();});
      ts.push_back (&root);
    }

    static void
    disfigure_execute (action a, const action_targets& ts)
    {
      tracer trace ("disfigure_execute");

      for (void* v: ts)
      {
        scope& root (*static_cast<scope*> (v));
        const path& out_root (root.path ());
        const path& src_root (root.src_path ());

        // We distinguish between a complete disfigure and operation-
        // specific.
        //
        if (a.operation () == default_id)
        {
          level4 ([&]{trace << "completely disfiguring " << out_root;});

          rmfile (out_root / config_file);
          rmfile (out_root / src_root_file);

          // Clean up the directories.
          //
          rmdir (out_root / bootstrap_dir);

          if (out_root != src_root)
          {
            rmdir (out_root / build_dir);

            if (rmdir (out_root) == rmdir_status::not_empty)
              warn << "directory " << out_root.string () << " is "
                   << (out_root == work
                       ? "current working directory"
                       : "not empty") << ", not removing";
          }
        }
        else
        {
        }
      }
    }

    static void
    disfigure_meta_operation_post ()
    {
      tracer trace ("disfigure_meta_operation_post");

      // Reset the dependency state since anything that could have been
      // loaded earlier using a previous configuration is now invalid.
      //
      level5 ([&]{trace << "resetting dependency state";});
      reset ();
    }

    meta_operation_info disfigure {
      "disfigure",
      nullptr, // meta-operation pre
      &disfigure_operation_pre,
      &disfigure_load,
      &disfigure_match,
      &disfigure_execute,
      nullptr, // operation post
      &disfigure_meta_operation_post
    };
  }
}
