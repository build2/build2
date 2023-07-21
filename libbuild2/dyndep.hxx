// file      : libbuild2/dyndep.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_DYNDEP_HXX
#define LIBBUILD2_DYNDEP_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/action.hxx>
#include <libbuild2/target.hxx>

#include <libbuild2/export.hxx>

// Additional functionality that is normally only useful for implementing
// rules with dynamic dependencies (usually prerequisites, but also target
// group members).
//
namespace build2
{
  class LIBBUILD2_SYMEXPORT dyndep_rule
  {
  public:
    // Update the target during the match phase. Return true if the target has
    // changed or, if the passed timestamp is not timestamp_unknown, it is
    // older than the target.
    //
    // Note that such a target must still be updated during the execute phase
    // in order to keep the dependency counts straight.
    //
    static bool
    update (tracer&, action, const target&, timestamp);

    // Update and add to the list of prerequisite targets a prerequisite file
    // target.
    //
    // Return the indication of whether it has changed or, if the passed
    // timestamp is not timestamp_unknown, is newer than this timestamp. If
    // the prerequisite target does not exists nor can be generated (no rule),
    // then issue diagnostics and fail if the fail argument is true and return
    // nullopt otherwise.
    //
    // If adhoc is true, then add it as ad hoc to prerequisite targets. At
    // first it may seem like such dynamic prerequisites should always be ad
    // hoc. But on the other hand, taking headers as an example, if the same
    // header is listed as a static prerequisite, it will most definitely not
    // going to be ad hoc. So we leave it to the caller to make this decision.
    // Similarly, the data argument is passed to the prerequisite_target ctor.
    //
    static optional<bool>
    inject_file (tracer&, const char* what,
                 action, target&,
                 const file& prerequiste,
                 timestamp,
                 bool fail,
                 bool adhoc = false,
                 uintptr_t data = 0);

    // As above but verify the file is matched with noop_recipe or was updated
    // during match and issue diagnostics and fail otherwise (regardless of
    // the fail flag). Pass 0 for pts_n if don't want the "was updated during
    // match" part.
    //
    // This version (together with verify_existing_file() below) is primarily
    // useful for handling dynamic dependencies that are produced as a
    // byproduct of recipe execution (and thus must have all the generated
    // prerequisites specified statically).
    //
    // Note that this function expects all the static prerequisites of the
    // target to already be matched and their number passed in pts_n.
    //
    static optional<bool>
    inject_existing_file (tracer&, const char* what,
                          action, target&, size_t pts_n,
                          const file& prerequiste,
                          timestamp,
                          bool fail,
                          bool adhoc = false,
                          uintptr_t data = 0);

    // Verify the file is matched with noop_recipe or was updated during match
    // and issue diagnostics and fail otherwise. If the file is not matched,
    // then fail if the target is not implied (that is, declared in a
    // buildfile). Pass 0 for pts_n if don't want the "was updated during
    // match" part.
    //
    // Note: can only be called in the execute phase.
    //
    static void
    verify_existing_file (tracer&, const char* what,
                          action, const target&, size_t pts_n,
                          const file& prerequiste);

    // Reverse-lookup target type(s) from file name/extension.
    //
    // If the list of base target types is specified, then only these types
    // and those derived from them are considered. Otherwise, any file-based
    // type is considered but not the file type itself.
    //
    // It's possible the extension-to-target type mapping is ambiguous (for
    // example, because both C and C++-language headers use the same .h
    // extension). So this function can return multiple target types.
    //
    static small_vector<const target_type*, 2>
    map_extension (const scope& base,
                   const string& name, const string& ext,
                   const target_type* const* bases);

    // Mapping of inclusion prefixes (e.g., foo in #include <foo/bar>) for
    // auto-generated files to inclusion search paths (e.g. -I) where they
    // will be generated.
    //
    // We are using a prefix map of directories (dir_path_map) instead of just
    // a map in order to also cover sub-paths (e.g., #include <foo/more/bar>
    // if we continue with the example). Specifically, we need to make sure we
    // don't treat foobar as a sub-directory of foo.
    //
    // The priority is used to decide who should override whom. Lesser values
    // are considered higher priority. Note that we allow multiple prefixless
    // mapping (where priority is used to determine the order). For details,
    // see append_prefix().
    //
    // Note that the keys should be normalized.
    //
    struct prefix_value
    {
      dir_path directory;
      size_t priority;
    };

    using prefix_map = dir_path_multimap<prefix_value>;

    // Add the specified absolute and normalized inclusion search path into
    // the prefix map of the specified target.
    //
    static void
    append_prefix (tracer&, prefix_map&, const target&, dir_path);

    // Mapping of src inclusion search paths to the corresponding out paths
    // for auto-generated files re-mapping. See cc::extract_headers() for
    // background.
    //
    // Note that we use path_map instead of dir_path_map to allow searching
    // using path (file path).
    //
    using srcout_map = path_map<dir_path>;

    class LIBBUILD2_SYMEXPORT srcout_builder
    {
    public:
      srcout_builder (context& ctx, srcout_map& map): ctx_ (ctx), map_ (map) {}

      // Process next -I path. Return true if an entry was added to the map,
      // in which case the passed path is moved from.
      //
      bool
      next (dir_path&&);

      // Skip the previously cached first half.
      //
      void
      skip ()
      {
        prev_ = nullptr;
      }

    private:
      context& ctx_;
      srcout_map& map_;

      // Previous -I's innermost scope if out_base plus the difference between
      // the scope path and the -I path (normally empty).
      //
      const scope* prev_ = nullptr;
      dir_path diff_;
    };

    // Find or insert a prerequisite file path as a target. If the path is
    // relative, then assume this is a non-existent generated file.
    //
    // Depending on the cache flag, the path is assumed to either have come
    // from the depdb cache or from the compiler run. If normalized is true,
    // then assume the absolute path is already normalized.
    //
    // Return the file target and an indication of whether it was remapped or
    // NULL if the file does not exist and cannot be generated. The passed by
    // reference file path is guaranteed to still be valid but might have been
    // adjusted (e.g., completed, normalized, remapped, etc). If the result is
    // not NULL, then it is the absolute and normalized path to the actual
    // file. If the result is NULL, then it can be used in diagnostics to
    // identify the origial file path.
    //
    // The map_extension function is used to reverse-map a file extension to
    // the target type. The fallback target type is used if it's NULL or
    // didn't return anything but only in situations where we are sure the
    // file is or should be there (see the implementation for details).
    //
    // The prefix map function is only called if this is a non-existent
    // generated file (so it can be initialized lazily). If it's NULL, then
    // generated files will not be supported. The srcout map is only consulted
    // if cache is false to re-map generated files (so its initialization can
    // be delayed until the call with cache=false).
    //
    using map_extension_func = small_vector<const target_type*, 2> (
      const scope& base, const string& name, const string& ext);

    using prefix_map_func = const prefix_map& (
      action, const scope& base, const target&);

    static pair<const file*, bool>
    enter_file (tracer&, const char* what,
                action, const scope& base, target&,
                path& prerequisite, bool cache, bool normalized,
                const function<map_extension_func>&,
                const target_type& fallback,
                const function<prefix_map_func>& = nullptr,
                const srcout_map& = {});

    // As above but do not insert the target if it doesn't already exist. This
    // function also returns NULL if the target exists but is dynamic (that
    // is, not real or implied), unless the dynamic argument is true.
    //
    static pair<const file*, bool>
    find_file (tracer&, const char* what,
               action, const scope& base, const target&,
               path& prerequisite, bool cache, bool normalized,
               bool dynamic,
               const function<map_extension_func>&,
               const target_type& fallback,
               const function<prefix_map_func>& = nullptr,
               const srcout_map& = {});

    // Find or insert a target file path as a target of the specified type,
    // make it a member of the specified (non-ad hoc) mtime target group and
    // set its path. Return the target and an indication of whether it was
    // made a member (can only be false if a filter is provided; see below).
    //
    // The file path must be absolute and normalized. Note that this function
    // assumes that this member can only be matched via this group. The group
    // type must have the target_type::flag::dyn_members flag.
    //
    // If specified, the group_filter function is called on the target before
    // making it a group member, skipping it if this function returns false.
    // Note that the filter is skipped if the target is newly inserted (the
    // filter is meant to be used to skip duplicates).
    //
    using group_filter_func = bool (mtime_target& g, const file&);

    static pair<const file&, bool>
    inject_group_member (action, const scope& base, mtime_target&,
                         path,
                         const target_type&,
                         const function<group_filter_func>& = nullptr);

    template <typename T>
    static pair<const T&, bool>
    inject_group_member (action a, const scope& bs, mtime_target& g,
                         path f,
                         const function<group_filter_func>& filter = nullptr)
    {
      auto p (inject_group_member (a, bs, g, move (f), T::static_type, filter));
      return pair<const T&, bool> (p.first.template as<T> (), p.second);
    }

    // As above but the target type is determined using the map_extension
    // function if specified, falling back to the fallback type if unable to
    // (the what argument is used for diagnostics during this process).
    //
    static pair<const file&, bool>
    inject_group_member (const char* what,
                         action, const scope& base, mtime_target& g,
                         path,
                         const function<map_extension_func>&,
                         const target_type& fallback,
                         const function<group_filter_func>& = nullptr);


    // Find or insert a target file path as a target, make it a member of the
    // specified ad hoc group unless it already is, and set its path. Return
    // the target and an indication of whether it was added as a member.
    //
    // The file path must be absolute and normalized. Note that this function
    // assumes that this target can only be known as a member of this group.
    //
    static pair<const file&, bool>
    inject_adhoc_group_member (action, const scope& base, target& g,
                               path,
                               const target_type&);

    // As above but the target type is determined using the map_extension
    // function if specified, falling back to the fallback type if unable to
    // (the what argument is used for diagnostics during this process).
    //
    static pair<const file&, bool>
    inject_adhoc_group_member (const char* what,
                               action, const scope& base, target& g,
                               path,
                               const function<map_extension_func>&,
                               const target_type& fallback);
  };
}

#endif // LIBBUILD2_DYNDEP_HXX
