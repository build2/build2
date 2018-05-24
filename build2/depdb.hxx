// file      : build2/depdb.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_DEPDB_HXX
#define BUILD2_DEPDB_HXX

#include <fstream>
#include <cstring> // strlen()

#include <build2/types.hxx>
#include <build2/utility.hxx>

namespace build2
{
  // Auxiliary dependency database (those .d files). Uses io_error and
  // system_error exceptions to signal errors except for openning (see
  // below).
  //
  // This is a strange beast: a line-oriented, streaming database that can, at
  // some point, be switched from reading to (over)writing. The idea is to
  // store auxiliary/ad-hoc dependency information in the "invalidation"
  // order. That is, if an earlier line is out of date, then all the
  // subsequent ones are out of date as well.
  //
  // As an example, consider a dependency database for foo.o which is built
  // from foo.cxx by the cxx.compile rule. The first line could be the rule
  // name itself (perhaps with the version). If a different rule is now
  // building foo.o, then any dep info that was saved by cxx.compile is
  // probably useless. Next we can have the command line options that were
  // used to build foo.o. Then could come the source file name followed by the
  // extracted header dependencies. If the compile options or the source file
  // name have changed, then the header dependencies are likely to have
  // changed as well.
  //
  // As an example, here is what our foo.o.d could look like (the first line
  // is the database format version and the last '\0' character is the end
  // marker):
  //
  // 1
  // cxx.compile 1
  // g++-4.8 -I/tmp/foo -O3
  // /tmp/foo/foo.cxx
  // /tmp/foo/foo.hxx
  // /usr/include/string.h
  // /usr/include/stdlib.h
  // /tmp/foo/bar.hxx
  // ^@
  //
  // A race is possible between updating the database and the target. For
  // example, we may detect a line mismatch that renders the target out-of-
  // date (say, compile options in the above example). We update the database
  // but before getting a chance to update the target, we get interrupted. On
  // a subsequent re-run, because the database has been updated, we will miss
  // the "target requires update" condition.
  //
  // If we assume that an update of the database also means an update of the
  // target, then this "interrupted update" situation can be easily detected
  // by comparing the database and target modification timestamps.
  //
  class depdb
  {
  public:
    // Open the database for reading. Note that if the file does not exist,
    // has wrong format version, or is corrupt, then the database will be
    // immediately switched to writing.
    //
    // If the database cannot be opened, issue diagnostics and throw failed.
    // This commonly happens when the user tries to stash the target in a
    // non-existent subdirectory but forgets to add the corresponding fsdir{}
    // prerequisite. Handling this as io_error in every rule that uses depdb
    // would be burdensome thus we issue the diagnostics here.
    //
    depdb (const path&);

    // Return the modification time of the database. This value only makes
    // sense while reading (in the write mode it will be timestamp_unknown).
    //
    timestamp
    mtime () const {return mtime_;}

    // Update the database modification time in close() even if otherwise
    // no modifications are necessary (i.e., the database is in the read
    // mode and is at eof).
    //
    void
    touch () {touch_ = true;}

    // Close the database. If this function is not called, then the database
    // may be left in the old/currupt state. Note that in the read mode this
    // function will "chop off" lines that haven't been read.
    //
    void
    close ();

    // Read the next line. If the result is not NULL, then it is a pointer to
    // the next line in the database (which you are free to move from). If you
    // then call write(), this line will be overwritten.
    //
    // If the result is NULL, then it means no next line is unavailable. This
    // can be due to several reasons:
    //
    // - eof reached (you can detect this by calling more() before read())
    // - database is already in the write mode
    // - the next line (and the rest of the database are corrupt)
    //
    string*
    read () {return state_ == state::write ? nullptr : read_ ();}

    // Return true if the database is in the read mode and there is at least
    // one more line available. Note that there is no guarantee that the line
    // is not corrupt. In other words, read() can still return NULL, it just
    // won't be because of eof.
    //
    bool
    more () const {return state_ == state::read;}

    bool
    reading () const {return state_ != state::write;}

    bool
    writing () const {return state_ == state::write;}

    bool
    touched () const {return touch_;}

    // Skip to the end of the database and return true if it is valid.
    // Otherwise, return false, in which case the database must be
    // overwritten. Note that this function expects the database to be in the
    // read state.
    //
    bool
    skip ();

    // Write the next line. If nl is false then don't write the newline yet.
    // Note that this switches the database into the write mode and no further
    // reading will be possible.
    //
    void
    write (const string& l, bool nl = true) {write (l.c_str (), l.size (), nl);}

    void
    write (const path& p, bool nl = true) {write (p.string (), nl);}

    void
    write (const char* s, bool nl = true) {write (s, std::strlen (s), nl);}

    void
    write (const char*, size_t, bool nl = true);

    void
    write (char, bool nl = true);

    // Mark the previously read line as to be overwritte.
    //
    void
    write () {if (state_ != state::write) change ();}

    // Read the next line and compare it to the expected value. If it matches,
    // return NULL. Otherwise, overwrite it and return the old value (which
    // could also be NULL). This strange-sounding result semantics is used to
    // detect the "there is a value but it does not match" case for tracing:
    //
    // if (string* o = d.expect (...))
    //   l4 ([&]{trace << "X mismatch forcing update of " << t;});
    //
    string*
    expect (const string& v)
    {
      string* l (read ());
      if (l == nullptr || *l != v)
      {
        write (v);
        return l;
      }

      return nullptr;
    }

    string*
    expect (const path& v)
    {
      string* l (read ());
      if (l == nullptr || path::traits::compare (*l, v.string ()) != 0)
      {
        write (v);
        return l;
      }

      return nullptr;
    }

    string*
    expect (const char* v)
    {
      string* l (read ());
      if (l == nullptr || *l != v)
      {
        write (v);
        return l;
      }

      return nullptr;
    }

  private:
    void
    change (bool flush = true);

    string*
    read_ ();

  private:
    timestamp mtime_;
    std::fstream fs_;

    std::fstream::pos_type pos_; // Start of the last returned line.
    string line_;

    enum class state {read, read_eof, write} state_;
    bool touch_;
  };
}

#endif // BUILD2_DEPDB_HXX
