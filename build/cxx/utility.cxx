// file      : build/cxx/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/cxx/utility>

#include <build/bin/target>

using namespace std;

namespace build
{
  namespace cxx
  {
    void
    append_lib_options (cstrings& args, target& l, const char* var)
    {
      using namespace bin;

      for (target* t: l.prerequisite_targets)
      {
        if (t->is_a<lib> () || t->is_a<liba> () || t->is_a<libso> ())
          append_lib_options (args, *t, var);
      }

      append_options (args, l, var);
    }
  }
}
