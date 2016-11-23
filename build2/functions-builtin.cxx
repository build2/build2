// file      : build2/functions-builtin.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/function>
#include <build2/variable>

using namespace std;

namespace build2
{
  void
  builtin_functions ()
  {
    function_family f ("builtin");

    f["type"] = [](value* v) {return v->type != nullptr ? v->type->name : "";};

    f["null"]  = [](value* v) {return v->null;};
    f["empty"] = [](value v)  {return v.empty ();};

    f["identity"] = [](value* v) {return move (*v);};
  }
}
