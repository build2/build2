// file      : libbuild2/cc/compiledb.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_COMPILEDB_HXX
#define LIBBUILD2_CC_COMPILEDB_HXX

#include <unordered_map>

#ifndef BUILD2_BOOTSTRAP
#  include <libbutl/json/serializer.hxx>
#endif

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/target.hxx>
#include <libbuild2/action.hxx>
#include <libbuild2/context.hxx>

namespace build2
{
  namespace cc
  {
    using compiledb_name_filter = vector<pair<optional<string>, bool>>;
    using compiledb_type_filter = vector<pair<optional<string>, string>>;

    class compiledb
    {
    public:
      // Match callback where we confirm an entry in the database and also
      // signal whether it has changes (based on change tracking in depdb).
      // Return true to force compilation of this target and thus make sure
      // the below execute() is called (unless something before that failed).
      //
      // Besides noticing changes, this callback is also necessary to notice
      // and delete entries that should no longer be in the database (e.g., a
      // source file was removed from the project).
      //
      // Note that output is either obj*{}, bmi*{}, of hbmi*{}.
      //
      static bool
      match (const scope& bs,
             const file& output, const path& output_path,
             const file& input,
             bool changed);

      // Execute callback where we insert or update an entry in the database.
      //
      // The {relo, abso}, and {relm, absm} pairs are used to "untranslate"
      // relative paths to absolute. Specifically, any argument that has rel?
      // as a prefix has this prefix replaced with the corresponding abs?.
      // Note that this means we won't be able to handle old MSVC and
      // clang-cl, which don't support the `/F?: <path>` form, only
      // `/F?<path>`. Oh, well. Note also that either relo or relm (but not
      // both) could be empty if unused.
      //
      // Note that we assume the source file is always absolute and is the
      // last argument.
      //
      // Why do we want absolute paths? That's a good question. Our initial
      // plan was to compare command lines in order to detect when we need to
      // update the database. And if those changed with every change of CWD,
      // that would be of little use. But then we realized we could do better
      // by using depdb to detect changes. So now we actually don't have a
      // need to get rid of the relative paths in the command line. But seeing
      // that we already have it, let's keep it for now in case it makes a
      // different to some broken/legacy consumers. Note also that C++ module
      // name-to-BMI mapping is not untranslated (see append_module_options()).
      //
      static void
      execute (const scope& bs,
               const file& output, const path& output_path,
               const file& input, const path& input_path,
               const process_path& cpath, const cstrings& args,
               const path& relo, const path& abso,
               const path& relm, const path& absm);

    public:
      using path_type = build2::path;

      string    name;
      path_type path;

      // The path is expected to be absolute and normalized or empty if the
      // name is `-` (stdout).
      //
      compiledb (string n, path_type p)
          : name (move (n)), path (move (p))
      {
      }

      virtual void
      pre (context&) = 0;

      virtual bool
      match (const file& output, const path_type& output_path,
             bool changed) = 0;

      virtual void
      execute (const file& output, const path_type& output_path,
               const file& input, const path_type& input_path,
               const process_path& cpath, const cstrings& args,
               const path_type& relo, const path_type& abso,
               const path_type& relm, const path_type& absm) = 0;

      virtual void
      post (context&, const action_targets&, bool failed) = 0;

      virtual
      ~compiledb ();
    };

    using compiledb_set = vector<unique_ptr<compiledb>>;

    // Populated by core_config_init() during serial load.
    //
    extern compiledb_set compiledbs;

    // Context operation callbacks.
    //
    void
    compiledb_pre (context&, action, const action_targets&);

    void
    compiledb_post (context&, action, const action_targets&, bool failed);

#ifndef BUILD2_BOOTSTRAP

    // Implementation that writes to stdout.
    //
    // Note that this implementation forces compilation of all the targets for
    // which it is called to make sure their entries are in the database. So
    // typically used in the dry run mode.
    //
    class compiledb_stdout: public compiledb
    {
    public:
      // The path is expected to be empty.
      //
      explicit
      compiledb_stdout (string name);

      virtual void
      pre (context&) override;

      virtual bool
      match (const file& output, const path_type& output_path,
             bool changed) override;

      virtual void
      execute (const file& output, const path_type& output_path,
               const file& input, const path_type& input_path,
               const process_path& cpath, const cstrings& args,
               const path_type& relo, const path_type& abso,
               const path_type& relm, const path_type& absm) override;

      virtual void
      post (context&, const action_targets&, bool failed) override;

    private:
      mutex mutex_;
      enum class state {init, empty, full, failed} state_;
      size_t nesting_;
      json_stream_serializer js_;
    };

    // Implementation that maintains a file.
    //
    class compiledb_file: public compiledb
    {
    public:
      compiledb_file (string name, path_type path);

      virtual void
      pre (context&) override;

      virtual bool
      match (const file& output, const path_type& output_path,
             bool changed) override;

      virtual void
      execute (const file& output, const path_type& output_path,
               const file& input, const path_type& input_path,
               const process_path& cpath, const cstrings& args,
               const path_type& relo, const path_type& abso,
               const path_type& relm, const path_type& absm) override;

      virtual void
      post (context&, const action_targets&, bool failed) override;

    private:
      mutex mutex_;
      enum class state {closed, open, failed} state_;
      size_t nesting_;

      // We want to optimize the performance for the incremental update case
      // where only a few files will be recompiled and most of the time there
      // will be no change in the command line, which means we won't need to
      // rewrite the file.
      //
      // As a result, our in-memory representation is a hashmap (we could have
      // thousands of entries) of absolute and normalized output file paths
      // (stored as strings for lookup efficiency) to their serialized JSON
      // text lines plus the status: absent, present, changed, or missing
      // (entry should be there but is not). This way we don't waste
      // (completely) parsing (and re-serializing) each line knowing that we
      // won't need to touch most of them.
      //
      // In fact, we could have gone even further and used a sorted vector
      // since insertions will be rare in this case. But we will need to
      // lookup every entry on each update, so it's unclear this is a win.
      //
      enum class entry_status {absent, present, changed, missing};

      struct entry
      {
        entry_status status;
        string json;
      };

      using map_type = std::unordered_map<string, entry>;
      map_type db_;

      // Number/presence of various entries in the database (used to determine
      // whether we need to update the file without iterating over all the
      // entries).
      //
      size_t absent_; // Number of absent entries.
      bool changed_;  // Presence of changed or missing entries.
    };

#endif // BUILD2_BOOTSTRAP
  }
}

#endif // LIBBUILD2_CC_COMPILEDB_HXX
