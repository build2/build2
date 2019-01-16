// file      : build2/in/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/in/target.hxx>

using namespace std;

namespace build2
{
  namespace in
  {
    static const target*
    in_search (const target& xt, const prerequisite_key& cpk)
    {
      // If we have no extension then derive it from our target. Then delegate
      // to file_search().
      //
      prerequisite_key pk (cpk);
      optional<string>& e (pk.tk.ext);

      if (!e)
      {
        if (const file* t = xt.is_a<file> ())
        {
          const string& te (t->derive_extension ());
          e = te + (te.empty () ? "" : ".") + "in";
        }
        else
          fail << "prerequisite " << pk << " for a non-file target " << xt;
      }

      return file_search (xt, pk);
    }

    static bool
    in_pattern (const target_type&,
                const scope&,
                string&,
                optional<string>&,
                const location& l,
                bool)
    {
      fail (l) << "pattern in in{} prerequisite" << endf;
    }

    extern const char in_ext_def[] = ""; // No extension by default.

    const target_type in::static_type
    {
      "in",
      &file::static_type,
      &target_factory<in>,
      &target_extension_fix<in_ext_def>,
      nullptr, /* default_extension */   // Taken care of by search.
      &in_pattern,
      &target_print_1_ext_verb,          // Same as file.
      &in_search,
      false
    };
  }
}
