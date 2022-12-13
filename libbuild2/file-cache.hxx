// file      : libbuild2/file-cache.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_FILE_CACHE_HXX
#define LIBBUILD2_FILE_CACHE_HXX

#include <libbuild2/types.hxx>
#include <libbuild2/forward.hxx>
#include <libbuild2/utility.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // We sometimes have intermediate build results that must be stored and
  // accessed as files (for example, partially-preprocessed C/C++ translation
  // units; those .i/.ii files). These files can be quite large which can lead
  // to excessive disk usage (for example, the .ii files can be several MB
  // each and can end up dominating object file sizes in a build with debug
  // information). These files are also often temporary which means writing
  // them to disk is really a waste.
  //
  // The file cache attempts to address this by still presenting a file-like
  // entry (which can be a real file or a named pipe) but potentially storing
  // the file contents in memory and/or compressed.
  //
  // Each cache entry is identified by the filesystem entry path that will be
  // written to or read from. The file cache reserves a filesystem entry path
  // that is derived by adding a compression extension to the main entry path
  // (for example, .ii.lz4). When cleaning intermediate build results that are
  // managed by the cache, the rule must clean such a reserved path in
  // addition to the main entry path (see compressed_extension() below).
  //
  // While the cache is MT-safe (that is, we can insert multiple entries
  // concurrently), each entry is expected to be accessed serially by a single
  // thread. Furthermore, each entry can either be written to or read from at
  // any give time and it can only be read from by a single reader at a time.
  // In other words, there meant to be a single cache entry for any given path
  // and it is not meant to be shared.
  //
  // The underlying filesystem entry can be either temporary or permanent. A
  // temporary entry only exists during the build, normally between the match
  // and execute phases. A permanent entry exists across builds. Note,
  // however, that a permanent entry is often removed in cases of an error and
  // sometimes a temporary entry is left behind for diagnostics. It is also
  // possible that the distinction only becomes known some time after the
  // entry has been created. As a result, all entries by default start as
  // temporary and can later be made permanent if desired.
  //
  // A cache entry can be pinned or unpinned. A cache entry is created pinned.
  // A cache entry being written to or read from remains pinned.
  //
  // An unpinned entry can be preempted. Preempting a cache entry can mean any
  // of the following:
  //
  //   - An in-memory content is compressed (but stays in memory).
  //
  //   - An in-memory content (compressed or not) is flushed to disk (with or
  //     without compression).
  //
  //   - An uncompressed on-disk content is compressed.
  //
  // Naturally, any of the above degrees of preemption make accessing the
  // contents of a cache entry slower. Note also that pinned/unpinned and
  // temporary/permanent are independent and a temporary entry does not need
  // to be unpinned to be removed.
  //
  // After creation, a cache entry must be initialized by either writing new
  // contents to the filesystem entry or by using an existing (permanent)
  // filesystem entry. Once initialized, an entry can be opened for reading,
  // potentially multiple times.
  //
  // Note also that a noop implementation of this caching semantics (that is,
  // one that simply saves the file on disk) is file_cache::entry that is just
  // auto_rmfile.

  // The synchronous LZ4 on-disk compression file cache implementation.
  //
  // If the cache entry is no longer pinned, this implementation compresses
  // the content and removes the uncompressed file all as part of the call
  // that caused the entry to become unpinned.
  //
  // In order to deal with interruptions during compression, when recreating
  // the cache entry state from the filesystem state, this implementation
  // treats the presence of the uncompressed file as an indication that the
  // compressed file, if any, is invalid.
  //
  class file_cache
  {
  public:
    // If compression is disabled, then this implementation becomes equivalent
    // to the noop implementation.
    //
    explicit
    file_cache (bool compress);

    file_cache () = default; // Create uninitialized instance.

    void
    init (bool compress);

    class entry;

    // A cache entry write handle. During the lifetime of this object the
    // filesystem entry can be opened for writing and written to.
    //
    // A successful write must be terminated with an explicit call to close()
    // (similar semantics to ofdstream). A write handle that is destroyed
    // without a close() call is treated as an unsuccessful write and the
    // initialization can be attempted again.
    //
    class write
    {
    public:
      void
      close ();

      write (): entry_ (nullptr) {}

      // Move-to-NULL-only type.
      //
      write (write&&) noexcept;
      write (const write&) = delete;
      write& operator= (write&&) noexcept;
      write& operator= (const write&) = delete;

      ~write ();

    private:
      friend class entry;

      explicit
      write (entry& e): entry_ (&e) {}

      entry* entry_;
    };

    // A cache entry read handle. During the lifetime of this object the
    // filesystem entry can be opened for reading and read from.
    //
    class read
    {
    public:
      read (): entry_ (nullptr) {}

      // Move-to-NULL-only type.
      //
      read (read&&) noexcept;
      read (const read&) = delete;
      read& operator= (read&&) noexcept;
      read& operator= (const read&) = delete;

      ~read ();

    private:
      friend class entry;

      explicit
      read (entry& e): entry_ (&e) {}

      entry* entry_;
    };

    // A cache entry handle. When it is destroyed, a temporary entry is
    // automatically removed from the filesystem.
    //
    class LIBBUILD2_SYMEXPORT entry
    {
    public:
      using path_type = build2::path;

      bool temporary = true;

      // The returned reference is valid and stable for the lifetime of the
      // entry handle.
      //
      const path_type&
      path () const;

      // Initialization.
      //
      write
      init_new ();

      void
      init_existing ();

      // Reading.
      //
      read
      open ();

      // Pinning.
      //
      // Note that every call to pin() should have a matching unpin().
      //
      void
      pin ();

      void
      unpin ();

      // NULL handle.
      //
      entry () = default;

      explicit operator bool () const;

      // Move-to-NULL-only type.
      //
      entry (entry&&) noexcept;
      entry (const entry&) = delete;
      entry& operator= (entry&&) noexcept;
      entry& operator= (const entry&) = delete;

      ~entry ();

    private:
      friend class file_cache;

      entry (path_type, bool, bool);

      void
      preempt ();

      bool
      compress ();

      void
      decompress ();

      void
      remove ();

      enum state {null, uninit, uncomp, comp, decomp};

      state     state_ = null;
      path_type path_;           // Uncompressed path.
      path_type comp_path_;      // Compressed path (empty if disabled).
      size_t    pin_ = 0;        // Pin count.
    };

    // Create a cache entry corresponding to the specified filesystem path.
    // The path must be absolute and normalized. The temporary argument may be
    // used to hint whether the entry is likely to be temporary or permanent.
    //
    entry
    create (path, optional<bool> temporary);

    // A shortcut for creating and initializing an existing permanent entry.
    //
    // Note that this function creates a permanent entry right away and if
    // init_existing() fails, no filesystem cleanup of any kind will be
    // performed.
    //
    entry
    create_existing (path);

    // Return the compressed filesystem entry extension (with the leading dot)
    // or empty string if no compression is used by this cache implementation.
    //
    // If the passed extension is not NULL, then it is included as a first-
    // level extension into the returned value (useful to form extensions for
    // clean_extra()).
    //
    string
    compressed_extension (const char* ext = nullptr);

  private:
    bool compress_;
  };
}

#include <libbuild2/file-cache.ixx>

#endif // LIBBUILD2_FILE_CACHE_HXX
