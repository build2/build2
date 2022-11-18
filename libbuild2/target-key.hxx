// file      : libbuild2/target-key.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TARGET_KEY_HXX
#define LIBBUILD2_TARGET_KEY_HXX

#include <cstring> // strcmp()

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target-type.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Light-weight (by being shallow-pointing) target key.
  //
  class LIBBUILD2_SYMEXPORT target_key
  {
  public:
    const target_type*       type;
    const dir_path*          dir; // Can be relative if part of prerequisite_key.
    const dir_path*          out; // Can be relative if part of prerequisite_key.
    const string*            name;
    mutable optional<string> ext; // Absent - unspecified, empty - none.

    template <typename T>
    bool is_a () const {return type->is_a<T> ();}
    bool is_a (const target_type& tt) const {return type->is_a (tt);}

    // Return an "effective" name, for example, for pattern matching, that
    // includes the extension where appropriate.
    //
    const string&
    effective_name (string& storage, bool force_ext = false) const;

    // Append/return the target name or a pair of names if out-qualified.
    //
    // See also target::as_name() for the returned name stability guarantees.
    //
    void
    as_name (names&) const;

    names
    as_name () const
    {
      names r;
      as_name (r);
      return r;
    }
  };

  inline bool
  operator== (const target_key& x, const target_key& y)
  {
    if (x.type  != y.type ||
        *x.dir  != *y.dir ||
        *x.out  != *y.out ||
        *x.name != *y.name)
      return false;

    // Unless fixed, unspecified and specified extensions are assumed equal.
    //
    const target_type& tt (*x.type);

    if (tt.fixed_extension == nullptr)
      return !x.ext || !y.ext || *x.ext == *y.ext;
    else
    {
      // Note that for performance reasons here we use the specified extension
      // without calling fixed_extension() to verify it matches.
      //
      const char* xe (x.ext
                      ? x.ext->c_str ()
                      : tt.fixed_extension (x, nullptr /* root scope */));

      const char* ye (y.ext
                      ? y.ext->c_str ()
                      : tt.fixed_extension (y, nullptr /* root scope */));

      return strcmp (xe, ye) == 0;
    }
  }

  inline bool
  operator!= (const target_key& x, const target_key& y) {return !(x == y);}

  // If the target type has a custom print function, call that. Otherwise,
  // call to_stream(). Both are defined in target.cxx.
  //
  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const target_key&);

  // If name_only is true, then only print the target name (and extension, if
  // necessary), without the directory or type.
  //
  // Return true if the result is regular, that is, in the
  // <dir>/<type>{<name>}@<out>/ form with the individual components
  // corresponding directly to the target_key members (that is, without moving
  // parts around as would be the case for directories). This information is
  // used when trying to print several targets in a combined form (for
  // example, {hxx cxx}{foo}) in print_diag().
  //
  LIBBUILD2_SYMEXPORT bool
  to_stream (ostream&,
             const target_key&,
             optional<stream_verbosity> = nullopt,
             bool name_only = false);
}

namespace std
{
  // Note that we ignore the extension when calculating the hash because of
  // its special "unspecified" logic (see operator== above).
  //
  template <>
  struct hash<build2::target_key>
  {
    using argument_type = build2::target_key;
    using result_type = size_t;

    size_t
    operator() (const build2::target_key& k) const noexcept
    {
      return build2::combine_hash (
        hash<const build2::target_type*> () (k.type),
        hash<build2::dir_path> () (*k.dir),
        hash<build2::dir_path> () (*k.out),
        hash<string> () (*k.name));
    }
  };
}

#endif // LIBBUILD2_TARGET_KEY_HXX
