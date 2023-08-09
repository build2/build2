// file      : libbuild2/functions-target-triplet.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  target_triplet_functions (function_map& m)
  {
    function_family f (m, "target_triplet");

    // $string(<target-triplet>)
    //
    // Return the canonical (that is, without the `unknown` vendor component)
    // target triplet string.
    //

    // Note that we must handle NULL values (relied upon by the parser
    // to provide conversion semantics consistent with untyped values).
    //
    f["string"] += [](target_triplet* t)
    {
      return t != nullptr ? t->string () : string ();
    };

    // $representation(<target-triplet>)
    //
    // Return the complete target triplet string that always contains the
    // vendor component.
    //
    f["representation"] += [](target_triplet t)
    {
      return t.representation ();
    };

    // Target triplet-specific overloads from builtins.
    //
    function_family b (m, "builtin");

    // Note that while we should normally handle NULL values (relied upon by
    // the parser to provide concatenation semantics consistent with untyped
    // values), the result will unlikely be what the user expected. So for now
    // we keep it a bit tighter.
    //
    b[".concat"] += [](target_triplet l, string sr) {return l.string () + sr;};
    b[".concat"] += [](string sl, target_triplet r) {return sl + r.string ();};

    b[".concat"] += [](target_triplet l, names ur)
    {
      return l.string () + convert<string> (move (ur));
    };

    b[".concat"] += [](names ul, target_triplet r)
    {
      return convert<string> (move (ul)) + r.string ();
    };
  }
}
