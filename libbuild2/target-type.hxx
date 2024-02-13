// file      : libbuild2/target-type.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TARGET_TYPE_HXX
#define LIBBUILD2_TARGET_TYPE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Target type.
  //
  // Note that we assume there is always a single instance of this class for
  // any target type. As a result, we can use address comparison to determine
  // if two target types are the same.
  //
  // If the extension derivation functions are NULL, then it means this target
  // type does not use extensions. Note that this is relied upon when deciding
  // whether to print the extension.
  //
  // If the fixed extension function is specified, then it means that this
  // target type has a fixed extension (including the no-extension case) and
  // this function should return such a fixed extension (which, if overriding
  // by the user is allowed, can point to the key's ext member; note that for
  // performance reasons we currently only verify the explicitly specified
  // extension on target insersion -- see target_key comparison for details).
  // It is called eraly, during the target insertion, in contrast to the
  // default extension function described below (you would specify one or the
  // other).
  //
  // Note that the fixed no-extension case that allows overriding by the user
  // is used to implement the "if extension is not specified by the user then
  // there is no extension" semantics of the file{} and similar target types.
  // For such cases the target_extension_none() function should be used (we
  // compare to it's address to detect target types with such semantics).
  // Similarly, for cases where the user must specify the extension explicitly
  // (e.g., man{}), use target_extension_must().
  //
  // The root scope argument to the fixed extension function may be NULL which
  // means the root scope is not known. A target type that relies on this must
  // be prepared to resolve the root scope itself and handle the cases where
  // the target is not (yet) in any project (this is currently only used to
  // handle the alternative build file/directory naming scheme and hopefully
  // it will stay that way).
  //
  // The default extension is used in two key (there are others) places:
  // search_existing_file() (called for a prerequisite with the last argument
  // true) and in target::derive_extension() (called for a target with the
  // last argument false); see their respective implementations for details.
  // The third argument is the default extension that is supplied (e.g., by a
  // rule) to derive_extension(), if any. The implementation can decide which
  // takes precedence, etc (see the exe{} target type for some interesting
  // logic). If the default extension function returns NULL, then it means the
  // default extension for this target could not be derived.
  //
  // If the pattern function is not NULL, then it is used to amend a pattern
  // or match (reverse is false) and then, if the amendment call returned
  // true, to reverse it in the resulting matches. The pattern function for a
  // non-directory target must first call target::split_name() if reverse is
  // false.
  //
  struct LIBBUILD2_SYMEXPORT target_type
  {
    const char* name;
    const target_type* base;

    target* (*factory) (context&,
                        const target_type&,
                        dir_path,
                        dir_path,
                        string);

    const char*      (*fixed_extension)   (const target_key&,
                                           const scope* root);

    optional<string> (*default_extension) (const target_key&,
                                           const scope& base,
                                           const char*,
                                           bool search);

    bool (*pattern) (const target_type&,
                     const scope& base,
                     string& name,
                     optional<string>& extension,
                     const location&,
                     bool reverse);

    // See to_stream(ostream,target_key) for details.
    //
    bool (*print) (ostream&, const target_key&, bool name_only);

    // Target type-specific prerequisite to target search.
    //
    // If passed target is NULL, then only search for an existing target (and
    // which can be performed during execute, not only match).
    //
    const target* (*search) (context&,
                             const target*,
                             const prerequisite_key&);

    // Target type flags.
    //
    // Note that the member_hint flag should only be used on groups with
    // link-up during load (see lib{}, for example). In particular, if the
    // group link-up only happens during match, then the hint would be looked
    // up before the group is known.
    //
    // Note: consider exposing as an attribute in define if adding a new flag.
    //
    enum class flag: uint64_t
    {
      none        = 0,
      group       = 0x01,         // A (non-adhoc) group.
      see_through = group | 0x02, // A group with "see through" semantics.
      member_hint = group | 0x04, // Untyped rule hint applies to members.
      dyn_members = group | 0x08  // A group with dynamic members.
    };

    flag flags;

    bool
    see_through () const;

    template <typename T>
    bool
    is_a () const {return is_a (T::static_type);}

    bool
    is_a (const target_type& tt) const
    {
      for (const target_type* b (this); b != nullptr; b = b->base)
        if (b == &tt)
          return true;

      return false;
    }

    bool
    is_a (const char*) const; // Defined in target.cxx

    target_type& operator= (target_type&&) = delete;
    target_type& operator= (const target_type&) = delete;
  };

  inline bool
  operator< (const target_type& x, const target_type& y) {return &x < &y;}

  inline bool
  operator== (const target_type& x, const target_type& y) {return &x == &y;}

  inline bool
  operator!= (const target_type& x, const target_type& y) {return &x != &y;}

  inline ostream&
  operator<< (ostream& os, const target_type& tt) {return os << tt.name;}

  inline target_type::flag
  operator&= (target_type::flag& x, target_type::flag y)
  {
    return x = static_cast<target_type::flag> (
      static_cast<uint64_t> (x) & static_cast<uint64_t> (y));
  }

  inline target_type::flag
  operator|= (target_type::flag& x, target_type::flag y)
  {
    return x = static_cast<target_type::flag> (
      static_cast<uint64_t> (x) | static_cast<uint64_t> (y));
  }

  inline target_type::flag
  operator& (target_type::flag x, target_type::flag y) {return x &= y;}

  inline target_type::flag
  operator| (target_type::flag x, target_type::flag y) {return x |= y;}

  inline bool target_type::
  see_through () const
  {
    return (flags & flag::see_through) == flag::see_through;
  }

  // Target type map.
  //
  class target_type_map
  {
  public:
    // Target type name to target type mapping.
    //
    const target_type*
    find (const string& n) const
    {
      auto i (type_map_.find (n));
      return i != type_map_.end () ? &i->second.get () : nullptr;
    }

    bool
    empty () const
    {
      return type_map_.empty ();
    }

    pair<reference_wrapper<const target_type>, bool>
    insert (const target_type& tt)
    {
      auto r (type_map_.emplace (tt.name, target_type_ref (tt)));
      return {r.second ? tt : r.first->second.get (), r.second};
    }

    template <typename T>
    const target_type&
    insert ()
    {
      return insert (T::static_type).first;
    }

    pair<reference_wrapper<const target_type>, bool>
    insert (const string& n, unique_ptr<target_type>&& tt)
    {
      target_type& rtt (*tt); // Save a non-const reference to the object.

      auto p (type_map_.emplace (n, target_type_ref (move (tt))));

      // Patch the alias name to use the map's key storage.
      //
      if (p.second)
        rtt.name = p.first->first.c_str ();

      return pair<reference_wrapper<const target_type>, bool> (
        p.first->second.get (), p.second);
    }

    // File name to target type mapping.
    //
    const target_type*
    find_file (const string& n) const
    {
      auto i (file_map_.find (n));
      return i != file_map_.end () ? &i->second.get () : nullptr;
    }

    void
    insert_file (const string& n, const target_type& tt)
    {
      file_map_.emplace (n, tt);
    }

  public:
    struct target_type_ref
    {
      // Like reference_wrapper except it sometimes deletes the target type.
      //
      explicit
      target_type_ref (const target_type& r): p_ (&r), d_ (false) {}

      explicit
      target_type_ref (unique_ptr<target_type>&& p)
          : p_ (p.release ()), d_ (true) {}

      target_type_ref (target_type_ref&& r) noexcept
          : p_ (r.p_), d_ (r.d_) {r.p_ = nullptr;}

      ~target_type_ref () {if (p_ != nullptr && d_) delete p_;}

      explicit operator const target_type& () const {return *p_;}
      const target_type& get () const {return *p_;}

    private:
      const target_type* p_;
      bool d_;
    };

    using type_map = map<string, target_type_ref>;
    using file_map = map<string, reference_wrapper<const target_type>>;

    using type_iterator = type_map::const_iterator;

    type_iterator type_begin () const {return type_map_.begin ();}
    type_iterator type_end ()   const {return type_map_.end ();}

  private:
    type_map type_map_;
    file_map file_map_;
  };
}

#endif // LIBBUILD2_TARGET_TYPE_HXX
