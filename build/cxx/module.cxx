// file      : build/cxx/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/module>

#include <istream>
#include <ext/stdio_filebuf.h>

#include <build/path>
#include <build/scope>
#include <build/process>
#include <build/diagnostics>

using namespace std;

namespace build
{
  namespace cxx
  {
    void
    init (scope& root, scope& base, const location& l)
    {
      //@@ TODO: avoid multiple inits (generally, for modules).
      //

      tracer trace ("cxx::init");

      //@@ Should it be this way?
      //
      if (&root != &base)
        fail (l) << "cxx module must be initialized in project root scope";

      //@@ TODO: need to register target types, rules here instead of main().

      const path& out_root (root.path ());
      level4 ([&]{trace << "for " << out_root << '/';});

      // Configure.
      //

      // config.cxx
      //
      for (bool f (true); f; f = false)
      {
        auto val (root["config.cxx"]);

        string v;

        if (val)
        {
          if (val.scope != global_scope)
            break; // A value from config.build.

          v = val.as<const string&> ();
        }
        else
          v = "g++"; // Default.

        // Test it by trying to execute.
        //
        const char* args[] = {v.c_str (), "-dumpversion", nullptr};

        if (verb >= 1)
          print_process (args);
        else
          text << "test " << v;

        string ver;
        try
        {
          process pr (args, false, false, true);

          __gnu_cxx::stdio_filebuf<char> fb (pr.in_ofd, ios_base::in);
          istream is (&fb);

          bool r (getline (is, ver));

          if (!pr.wait ())
            throw failed ();

          if (!r)
            fail << "unexpected output from " << v;
        }
        catch (const process_error& e)
        {
          error << "unable to execute " << v << ": " << e.what ();

          if (e.child ())
            exit (1);

          throw failed ();
        }

        //text << "toolchain version " << ver;

        // Set on the project root.
        //
        root.variables["config.cxx"] = move (v);
      }

      // config.cxx.{p,c,l}options
      // config.cxx.libs
      //
      // These are optional so all we need to do is "import" them
      // into the root scope if they were specified on the command
      // line and set them to empty if unspecified (the last part
      // is important for the "configured as undefined" vs
      // "unconfigured" logic).
      //
      if (auto val = root["config.cxx.poptions"])
      {
        if (val.scope == global_scope)
          root.variables["config.cxx.poptions"] = val;
      }
      else
        root.variables["config.cxx.poptions"]; // Undefined.

      if (auto val = root["config.cxx.coptions"])
      {
        if (val.scope == global_scope)
          root.variables["config.cxx.coptions"] = val;
      }
      else
        root.variables["config.cxx.coptions"]; // Undefined.

      if (auto val = root["config.cxx.loptions"])
      {
        if (val.scope == global_scope)
          root.variables["config.cxx.loptions"] = val;
      }
      else
        root.variables["config.cxx.loptions"]; // Undefined.

      if (auto val = root["config.cxx.libs"])
      {
        if (val.scope == global_scope)
          root.variables["config.cxx.libs"] = val;
      }
      else
        root.variables["config.cxx.libs"]; // Undefined.
    }
  }
}
