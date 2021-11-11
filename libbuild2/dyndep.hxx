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
// rules with dynamic dependencies.
//
namespace build2
{
  class LIBBUILD2_SYMEXPORT dyndep_rule
  {
  public:
    // Update the target during the match phase. Return true if it has changed
    // or if the passed timestamp is not timestamp_unknown and is older than
    // the target.
    //
    static bool
    update (tracer&, action, const target&, timestamp);

    // Update and add to the list of prerequisite targets a prerequisite file
    // target.
    //
    // Return the indication of whether it has changed or, if the passed
    // timestamp is not timestamp_unknown, is older than this timestamp. If
    // the prerequisite target does not exists nor can be generated (no rule),
    // then issue diagnostics and fail if the fail argument is true and return
    // nullopt otherwise.
    //
    static optional<bool>
    inject_file (tracer&, const char* what,
                 action, target&,
                 const file& prerequiste,
                 timestamp,
                 bool fail);

    // Reverse-lookup target type(s) from file name/extension.
    //
    // If the list of base target types is specified, then only these types
    // and those derived from them are considered. Otherwise, any file-based
    // type is considered but not the file type itself.
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

    // Enter a prerequisite file as a target. If the path is relative, then
    // assume this a non-existent generated file.
    //
    // Depending on the cache flag, the path is assumed to either have come
    // from the depdb cache or from the compiler run. In the former case
    // assume the path is already normalized unless the normalize flag is
    // true.
    //
    // Return the file target and an indication of whether it was remapped or
    // NULL if the file does not exist and cannot be generated. In the latter
    // case the passed file path is guaranteed to still be valid but might
    // have been adjusted (e.g., normalized, etc).
    //
    // The map_extension function is used to reverse-map a file extension to
    // the target type. The fallback target type is used if it's NULL or
    // didn't return anything but only in situations where we are sure the
    // file is (or should be there; see the implementation for details).
    //
    // The prefix map function is only called if this is a non-existent
    // generated file (so it can be initialized lazily). If it's NULL, then
    // generated files will not be supported. The srcout map is only consulted
    // if cache is false (so its initialization can be delayed until the call
    // with cache=false).
    //
    using map_extension_func = small_vector<const target_type*, 2> (
      const scope& base, const string& name, const string& ext);

    using prefix_map_func = const prefix_map& (
      action, const scope& base, const target&);

    static pair<const file*, bool>
    enter_file (tracer&, const char* what,
                action, const scope& base, target&,
                path&& prerequisite, bool cache, bool norm,
                const function<map_extension_func>&,
                const target_type& fallback,
                const function<prefix_map_func>&,
                const srcout_map&);
  };
}

#endif // LIBBUILD2_DYNDEP_HXX
