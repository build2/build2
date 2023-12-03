// file      : libbuild2/in/target.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/in/target.hxx>

using namespace std;

namespace build2
{
  namespace in
  {
    static const target*
    in_search (context& ctx, const target* xt, const prerequisite_key& cpk)
    {
      // If we have no extension then derive it from our target. Then delegate
      // to file_search().
      //
      prerequisite_key pk (cpk);
      optional<string>& e (pk.tk.ext);

      if (!e && xt != nullptr)
      {
        // Why is the extension, say, .h.in and not .in (with .h being in the
        // name)? While this is mostly academic (in this case things will work
        // the same either way), conceptually, it is a header template rather
        // than some file template. In other words, we are adding the second
        // level classification.
        //
        // See also the low verbosity tidying up code in the rule.
        //
        if (const file* t = xt->is_a<file> ())
        {
          const string& te (t->derive_extension ());
          e = te + (te.empty () ? "" : ".") + "in";
        }
        else
          fail << "prerequisite " << pk << " for a non-file target " << *xt;
      }

      return file_search (ctx, xt, pk);
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

    const target_type in::static_type
    {
      "in",
      &file::static_type,
      &target_factory<in>,
      &target_extension_none,
      nullptr, /* default_extension */   // Taken care of by search.
      &in_pattern,
      &target_print_1_ext_verb,          // Same as file (but see rule).
      &in_search,
      target_type::flag::none
    };
  }
}
