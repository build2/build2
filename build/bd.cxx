// file      : build/bd.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <time.h> // tzset()

#include <vector>
#include <cstdlib>      // exit
#include <cassert>
#include <iostream>
#include <system_error>

#include <build/process>
#include <build/timestamp>
#include <build/target>

using namespace std;

namespace build
{
  bool
  update (target& t)
  {
    auto tts (path_timestamp (t.name ()));
    cout << t.name () << ": " << tts << endl;

    bool u (tts == timestamp_nonexistent);
    for (target& p: t.prerequisites ())
    {
      if (!update (p))
        return false;

      if (!u)
      {
        auto tps (path_timestamp (p.name ()));

        if (tts <= tps) // Note: not just less.
        {
          cout << t.name () << " vs " << p.name () << ": " << (tps - tts)
               << " ahead" << endl;
          u = true;
        }
      }
    }

    if (!u) // Nothing to do.
      return true;

    try
    {
      auto r (t.rule ());
      return r != 0 ? r (t) : true;
    }
    catch (const process_error& e)
    {
      // Take care of failed children. In a multi-threaded program that
      // fork()'ed but did not exec(), it is unwise to try to do any kind
      // of cleanup (like unwinding the stack and running destructors).
      //
      assert (e.child ());
      exit (1);
    }
  }
}

using namespace build;

bool
cxx_compile_rule (target& t)
{
  const targets& ps (t.prerequisites ());

  //@@ TODO: assuming .cxx is first.
  //
  const target& p0 (ps[0]);
  const char* args[] {
    "g++-4.9",
      "-std=c++11",
      "-I..",
      "-c",
      "-o", t.name ().c_str (),
      p0.name ().c_str (),
      nullptr};

  cerr << "c++ " << t.name () << endl;

  try
  {
    process pr (args);
    return pr.wait ();
  }
  catch (const process_error& e)
  {
    cerr << "error: unable to execute '" << args[0] << "': " <<
      e.what () << endl;

    if (e.child ())
      throw; // Let our caller terminate us quickly without causing a scene.

    return false;
  }
}

bool
cxx_link_rule (target& t)
{
  const targets& ps (t.prerequisites ());

  cerr << "ld " << t.name () << endl;
  return true;
}

int
main (int argc, char* argv[])
{
  // Initialize time conversion data that is used by localtime_r().
  //
  tzset ();

  exe bd ("bd");
  obj bd_o ("bd.o");
  bd.prerequisite (bd_o);
  bd.rule (&cxx_link_rule);

  cxx bd_cxx ("bd.cxx");
  hxx target ("target");
  bd_o.prerequisite (bd_cxx);
  bd_o.prerequisite (target);
  bd_o.rule (&cxx_compile_rule);

  if (!update (bd))
  {
    cerr << "unable to update '" << bd.name () << "'" << endl;
    return 1;
  }
}
