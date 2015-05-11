// file      : build/cxx/module.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/module>

#include <istream>
#include <ext/stdio_filebuf.h>

#include <build/path>
#include <build/scope>
#include <build/process>
#include <build/diagnostics>

#include <build/bin/module>

#include <build/config/utility>

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

      // Initialize the bin module.
      //
      bin::init (root, base, l);

      //@@ TODO: need to register target types, rules here instead of main().

      const dir_path& out_root (root.path ());
      level4 ([&]{trace << "for " << out_root;});

      // Configure.
      //

      // config.cxx
      //
      {
        auto r (config::required (root, "config.cxx", "g++"));

        // If we actually set a new value, test it by trying to execute.
        //
        if (r.second)
        {
          const string& cxx (r.first);
          const char* args[] = {cxx.c_str (), "-dumpversion", nullptr};

          if (verb)
            print_process (args);
          else
            text << "test " << cxx;

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
              fail << "unexpected output from " << cxx;
          }
          catch (const process_error& e)
          {
            error << "unable to execute " << cxx << ": " << e.what ();

            if (e.child ())
              exit (1);

            throw failed ();
          }

          //text << "toolchain version " << ver;
        }
      }

      // config.cxx.{p,c,l}options
      // config.cxx.libs
      //
      // These are optional. We also merge them into the corresponding
      // cxx.* variables.
      //
      if (auto* v = config::optional<list_value> (root, "config.cxx.poptions"))
        root.append ("cxx.poptions") += *v;

      if (auto* v = config::optional<list_value> (root, "config.cxx.coptions"))
        root.append ("cxx.coptions") += *v;

      if (auto* v = config::optional<list_value> (root, "config.cxx.loptions"))
        root.append ("cxx.loptions") += *v;

      if (auto* v = config::optional<list_value> (root, "config.cxx.libs"))
        root.append ("cxx.libs") += *v;
    }
  }
}
