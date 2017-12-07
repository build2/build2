// file      : build2/target-type.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TARGET_TYPE_HXX
#define BUILD2_TARGET_TYPE_HXX

#include <map>

#include <build2/types.hxx>
#include <build2/utility.hxx>

namespace build2
{
  class scope;
  class target;
  class target_key;
  class prerequisite_key;

  // Target type.
  //
  // Note that we assume there is always a single instance of this class for
  // any target type. As a result, we can use address comparison to determine
  // if two target types are the same.
  //
  // If the extension derivation functions are NULL, then it means this target
  // type does not use extensions. Note that this is relied upon when deciding
  // whether to print the extension; if the target does use extensions but the
  // defaults couldn't (and its ok), couldn't (and its not ok), or shouldn't
  // (logically) be obtained, then use target_extension_{null,fail,assert}(),
  // respectively. The fixed extension function should return the fixed
  // extension (which can point to the key's ext member if the explicit
  // extension specificaton is allowed). If the default extension function
  // returns NULL, then it means the default extension for this target could
  // not be derived.
  //
  // The extension is used in two places: search_existing_file() (called for a
  // prerequisite with the last argument true) and in target::derive_path()
  // (called for a target with the last argument false); see their
  // implementations for details.
  //
  // If the pattern function is not NULL, then it is used to amend a pattern
  // or match (reverse is false) and then, if the amendment call returned
  // true, to reverse it in the resulting matches.
  //
  struct target_type
  {
    const char* name;
    const target_type* base;

    target* (*factory) (const target_type&, dir_path, dir_path, string);

    const char*      (*fixed_extension)   (const target_key&);
    optional<string> (*default_extension) (const target_key&,
                                           const scope&,
                                           bool search);

    bool (*pattern) (const target_type&, const scope&, string&, bool reverse);

    void (*print) (ostream&, const target_key&);

    const target* (*search) (const target&, const prerequisite_key&);

    bool see_through; // A group with the default "see through" semantics.

    template <typename T>
    bool
    is_a () const {return is_a (T::static_type);}

    bool
    is_a (const target_type& tt) const
    {
      return this == &tt || (base != nullptr && is_a_base (tt));
    }

    bool
    is_a_base (const target_type&) const; // Defined in target.cxx
  };

  inline bool
  operator< (const target_type& x, const target_type& y) {return &x < &y;}

  inline bool
  operator== (const target_type& x, const target_type& y) {return &x == &y;}

  inline bool
  operator!= (const target_type& x, const target_type& y) {return &x != &y;}

  inline ostream&
  operator<< (ostream& os, const target_type& tt) {return os << tt.name;}

  // Target type map.
  //
  struct target_type_ref
  {
    // Like reference_wrapper except it sometimes deletes the target type.
    //
    explicit
    target_type_ref (const target_type& r): p_ (&r), d_ (false) {}

    explicit
    target_type_ref (unique_ptr<target_type>&& p)
        : p_ (p.release ()), d_ (true) {}

    target_type_ref (target_type_ref&& r)
        : p_ (r.p_), d_ (r.d_) {r.p_ = nullptr;}

    ~target_type_ref () {if (p_ != nullptr && d_) delete p_;}

    explicit operator const target_type& () const {return *p_;}
    const target_type& get () const {return *p_;}

  private:
    const target_type* p_;
    bool d_;
  };

  using target_type_map_base = std::map<string, target_type_ref>;

  class target_type_map: public target_type_map_base
  {
  public:
    const target_type&
    insert (const target_type& tt)
    {
      emplace (tt.name, target_type_ref (tt));
      return tt;
    }

    template <typename T>
    const target_type&
    insert () {return insert (T::static_type);}
  };
}

#endif // BUILD2_TARGET_TYPE_HXX
