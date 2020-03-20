// file      : libbuild2/types.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_TYPES_HXX
#define LIBBUILD2_TYPES_HXX

// Include unprocessed file during bootstrap. See <libbuild2/config.hxx.in>
// for details.
//
#ifdef BUILD2_BOOTSTRAP
#  include <libbuild2/config.hxx.in>
#else
#  include <libbuild2/config.hxx>
#endif

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

#include <libbutl/path.mxx>
#include <libbutl/path-map.mxx>
#include <libbutl/sha256.mxx>
#include <libbutl/process.mxx>
#include <libbutl/fdstream.mxx>
#include <libbutl/optional.mxx>
#include <libbutl/const-ptr.mxx>
#include <libbutl/timestamp.mxx>
#include <libbutl/vector-view.mxx>
#include <libbutl/small-vector.mxx>
#include <libbutl/project-name.mxx>
#include <libbutl/target-triplet.mxx>
#include <libbutl/semantic-version.mxx>
#include <libbutl/standard-version.mxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Commonly-used types.
  //
  using std::uint8_t;
  using std::uint16_t;
  using std::uint32_t;
  using std::uint64_t;
  using std::uintptr_t;

  using uint64s = std::vector<uint64_t>;

  using std::size_t;
  using std::nullptr_t;

  using std::pair;
  using std::tuple;
  using std::string;
  using std::function;
  using std::reference_wrapper;

  using strings = std::vector<string>;
  using cstrings = std::vector<const char*>;

  using std::hash;

  using std::initializer_list;

  using std::unique_ptr;
  using std::shared_ptr;
  using std::weak_ptr;

  using std::array;
  using std::vector;
  using butl::vector_view;  // <libbutl/vector-view.mxx>
  using butl::small_vector; // <libbutl/small-vector.mxx>

  using std::istream;
  using std::ostream;
  using std::endl;
  using std::streamsize; // C++'s ssize_t.

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

  // Like std::atomic except implicit conversion and assignment use relaxed
  // memory ordering.
  //
  template <typename T>
  struct relaxed_atomic: atomic<T>
  {
    using atomic<T>::atomic; // Delegate.
    relaxed_atomic (const relaxed_atomic& a) noexcept
        : atomic<T> (a.load (memory_order_relaxed)) {}

    operator T () const noexcept {return this->load (memory_order_relaxed);}

    T operator= (T v) noexcept {
      this->store (v, memory_order_relaxed); return v;}
    T operator= (const relaxed_atomic& a) noexcept {
      return *this = a.load (memory_order_relaxed);}
  };

  template <typename T>
  struct relaxed_atomic<T*>: atomic<T*>
  {
    using atomic<T*>::atomic; // Delegate.
    relaxed_atomic (const relaxed_atomic& a) noexcept
        : atomic<T*> (a.load (memory_order_relaxed)) {}

    operator T* () const noexcept {return this->load (memory_order_relaxed);}
    T& operator* () const noexcept {return *this->load (memory_order_relaxed);}
    T* operator-> () const noexcept {return this->load (memory_order_relaxed);}

    T* operator= (T* v) noexcept {
      this->store (v, memory_order_relaxed); return v;}
    T* operator= (const relaxed_atomic& a) noexcept {
      return *this = a.load (memory_order_relaxed);}
  };

  // VC 14 has issues.
  //
#if defined(_MSC_VER) && _MSC_VER <= 1900
  template <typename T, typename P>
  inline bool
  operator== (const relaxed_atomic<T*>& x, const P& y)
  {
    return static_cast<T*> (x) == y;
  }

  template <typename T, typename P>
  inline bool
  operator!= (const relaxed_atomic<T*>& x, const P& y)
  {
    return static_cast<T*> (x) != y;
  }
#endif

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

  // <libbutl/optional.mxx>
  //
  using butl::optional;
  using butl::nullopt;

  // <libbutl/const-ptr.mxx>
  //
  using butl::const_ptr;

  // <libbutl/path.mxx>
  // <libbutl/path-map.mxx>
  //
  using butl::path;
  using butl::path_name;
  using butl::path_name_view;
  using butl::path_name_value;
  using butl::dir_path;
  using butl::path_cast;
  using butl::basic_path;
  using butl::invalid_path;
  using butl::path_abnormality;

  using butl::path_map;
  using butl::dir_path_map;

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

  // <libbutl/timestamp.mxx>
  //
  using butl::system_clock;
  using butl::timestamp;
  using butl::duration;
  using butl::timestamp_unknown;
  using butl::timestamp_unknown_rep;
  using butl::timestamp_nonexistent;
  using butl::timestamp_unreal;
  using butl::to_string;
  using butl::operator<<;

  // <libbutl/sha256.mxx>
  //
  using butl::sha256;

  // <libbutl/process.mxx>
  using butl::process;
  using butl::process_env;
  using butl::process_path;
  using butl::process_error;

  // <libbutl/fdstream.mxx>
  //
  using butl::auto_fd;
  using butl::fdpipe;
  using butl::ifdstream;
  using butl::ofdstream;
  using butl::fdopen_mode;
  using butl::fdstream_mode;
  using butl::fdselect_state;
  using butl::fdselect_set;

  // <libbutl/target-triplet.mxx>
  //
  using butl::target_triplet;

  // <libbutl/semantic-version.mxx>
  //
  using butl::semantic_version;
  using butl::parse_semantic_version;

  // <libbutl/standard-version.mxx>
  //
  using butl::standard_version;
  using butl::standard_version_constraint;

  // <libbutl/project-name.mxx>
  //
  using butl::project_name;

  // Diagnostics location.
  //
  // Note that location maintains a shallow reference to path/path_name. Zero
  // lines or columns are not printed.
  //
  class location
  {
  public:
    path_name_view file;
    uint64_t       line;
    uint64_t       column;

    location (): line (0), column (0) {}

    explicit
    location (const path& f, uint64_t l = 0, uint64_t c = 0)
        : file (&f, nullptr /* name */), line (l), column (c) {}

    explicit
    location (path&&, uint64_t = 0, uint64_t = 0) = delete;

    explicit
    location (const path_name_view& f, uint64_t l = 0, uint64_t c = 0)
        : file (f), line (l), column (c) {}

    explicit
    location (path_name_view&&, uint64_t = 0, uint64_t = 0) = delete;

    bool
    empty () const {return file.null () || file.empty ();}

  protected:
    location (uint64_t l, uint64_t c): line (l), column (c) {}
  };

  // Similar (and implicit-convertible) to the above but stores a copy of the
  // path.
  //
  class location_value: public location
  {
  public:
    path_name_value file;

    location_value ();

    explicit
    location_value (const location&);

    location_value (location_value&&);
    location_value (const location_value&);
    location_value& operator= (location_value&&);
    location_value& operator= (const location_value&);
  };

  // See context.
  //
  enum class run_phase {load, match, execute};

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, run_phase); // utility.cxx

}

// In order to be found (via ADL) these have to be either in std:: or in
// butl::. The latter is a bad idea since libbutl includes the default
// implementation. They are defined in utility.cxx.
//
namespace std
{
  // Path printing potentially relative with trailing slash for directories.
  //
  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const ::butl::path&);

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const ::butl::path_name_view&);

  // Print as recall[@effect].
  //
  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const ::butl::process_path&);
}

// <libbuild2/name.hxx>
//
#include <libbuild2/name.hxx>

#include <libbuild2/types.ixx>

#endif // LIBBUILD2_TYPES_HXX
