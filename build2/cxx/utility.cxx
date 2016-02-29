// file      : build2/cxx/utility.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/cxx/utility>

#include <build2/bin/target>

using namespace std;

namespace build2
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

    void
    hash_lib_options (sha256& csum, target& l, const char* var)
    {
      using namespace bin;

      for (target* t: l.prerequisite_targets)
      {
        if (t->is_a<lib> () || t->is_a<liba> () || t->is_a<libso> ())
          hash_lib_options (csum, *t, var);
      }

      hash_options (csum, l, var);
    }
  }
}
