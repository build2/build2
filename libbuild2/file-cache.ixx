// file      : libbuild2/file-cache.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  // file_cache::entry
  //
  inline const path& file_cache::entry::
  path () const
  {
    return path_;
  }

  inline void file_cache::entry::
  pin ()
  {
    ++pin_;
  }

  inline void file_cache::entry::
  unpin ()
  {
    if (--pin_ == 0          &&
        !comp_path_.empty () &&
        (state_ == uncomp || state_ == decomp))
      preempt ();
  }

  inline file_cache::read file_cache::entry::
  open ()
  {
    assert (state_ != null && state_ != uninit);

    if (state_ == comp)
    {
      decompress ();
      state_ = decomp;
    }

    pin ();
    return read (*this);
  }

  inline file_cache::entry::
  operator bool () const
  {
    return state_ != null;
  }

  inline file_cache::entry::
  entry (path_type p, bool t, bool c)
      : temporary (t),
        state_ (uninit),
        path_ (move (p)),
        comp_path_ (c ? path_ + ".lz4" : path_type ()),
        pin_ (1)
  {
  }

  inline file_cache::entry::
  ~entry ()
  {
    if (state_ != null && temporary)
      remove ();
  }

  inline file_cache::entry::
  entry (entry&& e) noexcept
      : temporary (e.temporary),
        state_ (e.state_),
        path_ (move (e.path_)),
        comp_path_ (move (e.comp_path_)),
        pin_ (e.pin_)
  {
    e.state_ = null;
  }

  inline file_cache::entry& file_cache::entry::
  operator= (entry&& e) noexcept
  {
    if (this != &e)
    {
      assert (state_ == null);

      temporary = e.temporary;
      state_ = e.state_;
      path_ = move (e.path_);
      comp_path_ = move (e.comp_path_);
      pin_ = e.pin_;

      e.state_ = null;
    }
    return *this;
  }

  // file_cache::write
  //
  inline void file_cache::write::
  close ()
  {
    entry_->state_ = entry::uncomp;
  }

  inline file_cache::write::
  ~write ()
  {
    if (entry_ != nullptr)
      entry_->unpin ();
  }

  inline file_cache::write::
  write (write&& e) noexcept
      : entry_ (e.entry_)
  {
    e.entry_ = nullptr;
  }

  inline file_cache::write& file_cache::write::
  operator= (write&& e) noexcept
  {
    if (this != &e)
    {
      assert (entry_ == nullptr);
      swap (entry_, e.entry_);
    }
    return *this;
  }

  // file_cache::read
  //
  inline file_cache::read::
  ~read ()
  {
    if (entry_ != nullptr)
      entry_->unpin ();
  }

  inline file_cache::read::
  read (read&& e) noexcept
      : entry_ (e.entry_)
  {
    e.entry_ = nullptr;
  }

  inline file_cache::read& file_cache::read::
  operator= (read&& e) noexcept
  {
    if (this != &e)
    {
      assert (entry_ == nullptr);
      swap (entry_, e.entry_);
    }
    return *this;
  }

  // file_cache
  //
  inline file_cache::entry file_cache::
  create (path f, optional<bool>)
  {
    return entry (move (f), true /* temporary */, compress_);
  }

  inline file_cache::entry file_cache::
  create_existing (path f)
  {
    entry e (move (f), false /* temporary */, compress_);
    e.init_existing ();
    return e;
  }

  inline string file_cache::
  compressed_extension (const char* e)
  {
    return compress_
      ? (e != nullptr ? string (e) : string ()) + ".lz4"
      : string ();
  }

  inline void file_cache::
  init (bool compress)
  {
    compress_ = compress;
  }

  inline file_cache::
  file_cache (bool compress)
  {
    init (compress);
  }
}
