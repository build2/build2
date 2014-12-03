// file      : build/bd.cxx
// copyright : Copyright (c) 2014-2015 Code Synthesis Tools CC
// license   : MIT; see accompanying LICENSE file

#include <vector>
#include <iostream>

#include <build/target>

using namespace std;

namespace build
{
  bool
  update (target& t)
  {
    const targets& ps (t.prerequisites ());

    for (target& p: ps)
      if (!update (p))
        return false;

    //@@ TODO: check for existance, compare timestamps.

    auto r (t.rule ());
    return r != 0 ? r (t, t.prerequisites ()) : true;
  }
}

using namespace build;

bool
cxx_compile_rule (target& t, const targets& p)
{
  //@@ TODO: actually execute

  cerr << "c++ " << t.name () << endl;
  return true;
}

bool
cxx_link_rule (target& t, const targets& p)
{
  cerr << "ld " << t.name () << endl;
  return true;
}

int
main (int argc, char* argv[])
{
  exe bd ("bd");
  obj bd_o ("bd.o");
  bd.prerequisite (bd_o);
  bd.rule (&cxx_link_rule);

  cxx bd_cxx ("bd.cxx");
  hxx target ("target");
  bd_o.prerequisite (bd_cxx);
  bd_o.prerequisite (target);
  bd_o.rule (&cxx_compile_rule);

  update (bd);
}
