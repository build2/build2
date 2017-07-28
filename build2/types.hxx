// file      : build2/types.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TYPES_HXX
#define BUILD2_TYPES_HXX

#include <array>
#include <tuple>
#include <vector>
#include <string>
#include <memory>           // unique_ptr, shared_ptr
#include <utility>          // pair, move()
#include <cstddef>          // size_t, nullptr_t
#include <cstdint>          // uint{8,16,32,64}_t, *_MIN, *_MAX
#include <istream>
#include <ostream>
#include <functional>       // hash, function, reference_wrapper
#include <initializer_list>

#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

#include <libbutl/ft/shared_mutex.hxx>
#if defined(__cpp_lib_shared_mutex) || defined(__cpp_lib_shared_timed_mutex)
#  include <shared_mutex>
#endif

#include <ios>           // ios_base::failure
#include <exception>     // exception
#include <stdexcept>     // logic_error, invalid_argument, runtime_error
#include <system_error>

#include <libbutl/path.hxx>
#include <libbutl/sha256.hxx>
#include <libbutl/process.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/optional.hxx>
#include <libbutl/const-ptr.hxx>
#include <libbutl/timestamp.hxx>
#include <libbutl/vector-view.hxx>
#include <libbutl/small-vector.hxx>
#include <libbutl/target-triplet.hxx>
#include <libbutl/standard-version.hxx>

namespace build2
{
  // Commonly-used types.
  //
  using std::uint8_t;
  using std::uint16_t;
  using std::uint32_t;
  using std::uint64_t;
  using std::uintptr_t;

  using std::size_t;
  using std::nullptr_t;

  using std::pair;
  using std::tuple;
  using std::string;
  using std::function;
  using std::reference_wrapper;

  using std::hash;

  using std::initializer_list;

  using std::unique_ptr;
  using std::shared_ptr;
  using std::weak_ptr;

  using std::array;
  using std::vector;
  using butl::vector_view;  // <libbutl/vector-view.hxx>
  using butl::small_vector; // <libbutl/small-vector.hxx>

  using strings = vector<string>;
  using cstrings = vector<const char*>;

  using std::istream;
  using std::ostream;

  // Concurrency.
  //
  using std::atomic;
  using std::memory_order;
  using std::memory_order_relaxed;
  using std::memory_order_consume;
  using std::memory_order_acquire;
  using std::memory_order_release;
  using std::memory_order_acq_rel;
  using std::memory_order_seq_cst;

  using atomic_count = atomic<size_t>; // Matches scheduler::atomic_count.

  using std::mutex;
  using mlock = std::unique_lock<mutex>;

  using std::condition_variable;

#if   defined(__cpp_lib_shared_mutex)
  using shared_mutex = std::shared_mutex;
  using ulock        = std::unique_lock<shared_mutex>;
  using slock        = std::shared_lock<shared_mutex>;
#elif defined(__cpp_lib_shared_timed_mutex)
  using shared_mutex = std::shared_timed_mutex;
  using ulock        = std::unique_lock<shared_mutex>;
  using slock        = std::shared_lock<shared_mutex>;
#else
  // Because we have this fallback, we need to be careful not to create
  // multiple shared locks in the same thread.
  //
  struct shared_mutex: mutex
  {
    using mutex::mutex;

    void lock_shared     () { lock ();     }
    void try_lock_shared () { try_lock (); }
    void unlock_shared   () { unlock ();   }
  };

  using ulock        = std::unique_lock<shared_mutex>;
  using slock        = ulock;
#endif

  using std::defer_lock;
  using std::adopt_lock;

  using std::thread;
  namespace this_thread = std::this_thread;

  // Exceptions.
  //
  // While <exception> is included, there is no using for std::exception --
  // use qualified.
  //
  using std::logic_error;
  using std::invalid_argument;
  using std::runtime_error;
  using std::system_error;
  using io_error = std::ios_base::failure;

  // <libbutl/optional.hxx>
  //
  using butl::optional;
  using butl::nullopt;

  // <libbutl/const-ptr.hxx>
  //
  using butl::const_ptr;

  // <libbutl/path.hxx>
  //
  using butl::path;
  using butl::dir_path;
  using butl::basic_path;
  using butl::invalid_path;
  using butl::path_cast;

  // Absolute directory path. Note that for now we don't do any checking that
  // the path is in fact absolute.
  //
  // The idea is to have a different type that we automatically complete when
  // a (variable) value of this type gets initialized from untyped names. See
  // value_type<abs_dir_path> for details.
  //
  // Note that currently we also normalize and actualize the path. And we
  // leave empty path as is.
  //
  struct abs_dir_path: dir_path
  {
    using dir_path::dir_path;

    explicit
    abs_dir_path (dir_path d): dir_path (std::move (d)) {}
    abs_dir_path () = default;
  };

  using paths = std::vector<path>;
  using dir_paths = std::vector<dir_path>;

  // <libbutl/timestamp.hxx>
  //
  using butl::system_clock;
  using butl::timestamp;
  using butl::duration;
  using butl::timestamp_unknown;
  using butl::timestamp_unknown_rep;
  using butl::timestamp_nonexistent;
  using butl::operator<<;

  // <libbutl/sha256.hxx>
  //
  using butl::sha256;

  // <libbutl/process.hxx>
  // <libbutl/fdstream.hxx>
  //
  using butl::process;
  using butl::process_path;
  using butl::process_error;

  using butl::auto_fd;
  using butl::ifdstream;
  using butl::ofdstream;

  // <libbutl/target-triplet.hxx>
  //
  using butl::target_triplet;

  // <libbutl/standard-version.hxx>
  //
  using butl::standard_version;
  using butl::standard_version_constraint;

  // See context.
  //
  enum class run_phase {load, match, execute};

  ostream&
  operator<< (ostream&, run_phase); // utility.cxx

  extern run_phase phase;
}

// In order to be found (via ADL) these have to be either in std:: or in
// butl::. The latter is a bad idea since libbutl includes the default
// implementation. They are defined in utility.cxx.
//
namespace std
{
  // Path printing with trailing slash for directories.
  //
  ostream&
  operator<< (ostream&, const ::butl::path&);

  // Print as recall[@effect].
  //
  ostream&
  operator<< (ostream&, const ::butl::process_path&);
}

// <build2/name.hxx>
//
#include <build2/name.hxx>

#endif // BUILD2_TYPES_HXX
