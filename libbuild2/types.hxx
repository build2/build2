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

#include <map>
#include <set>
#include <list>
#include <array>
#include <tuple>
#include <regex>
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

#include <atomic>

#ifndef LIBBUTL_MINGW_STDTHREAD
#  include <mutex>
#  include <thread>
#  include <condition_variable>

#  include <libbutl/ft/shared_mutex.hxx>
#  if defined(__cpp_lib_shared_mutex) || defined(__cpp_lib_shared_timed_mutex)
#    include <shared_mutex>
#  endif
#else
#  include <libbutl/mingw-mutex.hxx>
#  include <libbutl/mingw-thread.hxx>
#  include <libbutl/mingw-condition_variable.hxx>
#  include <libbutl/mingw-shared_mutex.hxx>
#endif

#include <ios>           // ios_base::failure
#include <exception>     // exception
#include <stdexcept>     // logic_error, invalid_argument, runtime_error
#include <system_error>

#include <libbutl/path.hxx>
#include <libbutl/path-map.hxx>
#include <libbutl/regex.hxx>
#include <libbutl/sha256.hxx>
#include <libbutl/process.hxx>
#include <libbutl/fdstream.hxx>
#include <libbutl/optional.hxx>
#include <libbutl/const-ptr.hxx>
#include <libbutl/timestamp.hxx>
#include <libbutl/vector-view.hxx>
#include <libbutl/small-vector.hxx>
#include <libbutl/project-name.hxx>
#include <libbutl/target-triplet.hxx>
#include <libbutl/semantic-version.hxx>
#include <libbutl/standard-version.hxx>
#include <libbutl/move-only-function.hxx>

#include <libbuild2/export.hxx>

namespace build2
{
  // Commonly-used types.
  //
  using std::uint8_t;
  using std::uint16_t;
  using std::uint32_t;
  using std::int64_t;
  using std::uint64_t;
  using std::uintptr_t;

  using int64s  = std::vector<int64_t>;
  using uint64s = std::vector<uint64_t>;

  using std::size_t;
  using std::nullptr_t;

  using std::pair;
  using std::tuple;
  using std::string;
  using std::reference_wrapper;

  using std::function;
  using butl::move_only_function;
  using butl::move_only_function_ex;

  using strings = std::vector<string>;
  using cstrings = std::vector<const char*>;

  using std::hash;

  using std::initializer_list;

  using std::unique_ptr;
  using std::shared_ptr;
  using std::weak_ptr;

  using std::map;
  using std::multimap;
  using std::set;
  using std::multiset;
  using std::array;
  using std::vector;
  using std::list;
  using butl::vector_view;  // <libbutl/vector-view.hxx>
  using butl::small_vector; // <libbutl/small-vector.hxx>

  using std::istream;
  using std::ostream;
  using std::endl;
  using std::streamsize; // C++'s ssize_t.

  // Regex.
  //
  // Note that <libbutl/regex.hxx> includes an ostream insertion operator for
  // regex_error which prints cleaned up message, if any.
  //
  using std::regex;
  using std::regex_error;
  using regex_match_results = std::match_results<string::const_iterator>;

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

#ifndef LIBBUTL_MINGW_STDTHREAD
  using std::mutex;
  using mlock = std::unique_lock<mutex>;

  using std::condition_variable;

  using std::defer_lock;
  using std::adopt_lock;

  using std::thread;
  namespace this_thread = std::this_thread;

#  if   defined(__cpp_lib_shared_mutex)
  using shared_mutex = std::shared_mutex;
  using ulock        = std::unique_lock<shared_mutex>;
  using slock        = std::shared_lock<shared_mutex>;
#  elif defined(__cpp_lib_shared_timed_mutex)
  using shared_mutex = std::shared_timed_mutex;
  using ulock        = std::unique_lock<shared_mutex>;
  using slock        = std::shared_lock<shared_mutex>;
#  else
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
#  endif
#else // LIBBUTL_MINGW_STDTHREAD
  using mingw_stdthread::mutex;
  using mlock = mingw_stdthread::unique_lock<mutex>;

  using mingw_stdthread::condition_variable;

  using mingw_stdthread::defer_lock;
  using mingw_stdthread::adopt_lock;

  using mingw_stdthread::thread;
  namespace this_thread = mingw_stdthread::this_thread;

  using shared_mutex = mingw_stdthread::shared_mutex;
  using ulock        = mingw_stdthread::unique_lock<shared_mutex>;
  using slock        = mingw_stdthread::shared_lock<shared_mutex>;
#endif

  // Global, MT-safe information cache. Normally used for caching information
  // (versions, target triplets, search paths, etc) extracted from other
  // programs (compilers, etc).
  //
  // The key is normally a hash of all the inputs that can affect the output.
  //
  // Note that insertion is racy and it's possible the cache entry already
  // exists, in which case we ignore our value assuming it is the same.
  //
  template <typename T, typename K = string>
  class global_cache
  {
  public:
    const T*
    find (const K& k) const
    {
      mlock l (mutex_);
      auto i (cache_.find (k));
      return i != cache_.end () ? &i->second : nullptr;
    }

    const T&
    insert (K k, T v)
    {
      mlock l (mutex_);
      return cache_.insert (std::make_pair (std::move (k),
                                            std::move (v))).first->second;
    }

  private:
    map<K, T> cache_;
    mutable mutex mutex_;
  };

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
  // <libbutl/path-map.hxx>
  //
  using butl::path;
  using path_traits = path::traits_type;
  using butl::path_name;
  using butl::path_name_view;
  using butl::path_name_value;
  using butl::dir_path;
  using butl::dir_name_view;
  using butl::path_cast;
  using butl::basic_path;
  using butl::invalid_path;
  using butl::path_abnormality;

  using butl::path_map;
  using butl::dir_path_map;
  using butl::path_multimap;
  using butl::dir_path_multimap;

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

  // Path printing potentially relative with trailing slash for directories.
  //
  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const path&); // utility.cxx

  inline ostream&
  operator<< (ostream& os, const dir_path& d) // For overload resolution.
  {
    return build2::operator<< (os, static_cast<const path&> (d));
  }

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const path_name_view&); // utility.cxx

  // <libbutl/timestamp.hxx>
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

  // <libbutl/sha256.hxx>
  //
  using butl::sha256;

  // <libbutl/process.hxx>
  //
  using butl::process;
  using butl::process_env;
  using butl::process_exit;
  using butl::process_path;
  using butl::process_error;

  // Extended process_path with additional information.
  //
  // See also {import,export}.metadata.
  //
  // Note that the environment checksum is calculated in the (potentially
  // hermetic) project environment which makes instances of process_path_ex
  // project-specific. (We could potentially store the original list of
  // environment variables and calculate the checksum on the fly in the
  // current context thus making it project-independent. But this will
  // complicate things without, currently, much real benefit since all our
  // use-cases fit well with the project-specific restriction. We could
  // probably even support both variants if desirable.)
  //
  struct process_path_ex: process_path
  {
    optional<string> name;         // Stable name for diagnostics.
    optional<string> checksum;     // Executable checksum for change tracking.
    optional<string> env_checksum; // Environment checksum for change tracking.

    using process_path::process_path;

    process_path_ex (const process_path& p,
                     string n,
                     optional<string> c = {},
                     optional<string> ec = {})
        : process_path (p, false /* init */),
          name (std::move (n)),
          checksum (std::move (c)),
          env_checksum (std::move (ec)) {}

    process_path_ex (process_path&& p,
                     string n,
                     optional<string> c = {},
                     optional<string> ec = {})
        : process_path (std::move (p)),
          name (std::move (n)),
          checksum (std::move (c)),
          env_checksum (std::move (ec)) {}

    process_path_ex () = default;
  };

  // Print as recall[@effect].
  //
  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, const process_path&); // utility.cxx

  // <libbutl/fdstream.hxx>
  //
  using butl::nullfd;
  using butl::auto_fd;
  using butl::fdpipe;
  using butl::ifdstream;
  using butl::ofdstream;
  using butl::fdopen_mode;
  using butl::fdstream_mode;
  using butl::fdselect_state;
  using butl::fdselect_set;

  // <libbutl/target-triplet.hxx>
  //
  using butl::target_triplet;

  // <libbutl/semantic-version.hxx>
  //
  using butl::semantic_version;
  using butl::parse_semantic_version;

  // <libbutl/standard-version.hxx>
  //
  using butl::standard_version;
  using butl::standard_version_constraint;

  // <libbutl/project-name.hxx>
  //
  using butl::project_name;

  // Diagnostics location.
  //
  // Note that location maintains a shallow reference to path/path_name (use
  // location_value if you need the deep copy semantics). Zero lines or
  // columns are not printed.
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

  // Print in the <file>:<line>:<column> form with 0 lines/columns not
  // printed. Nothing is printed for an empty location.
  //
  ostream&
  operator<< (ostream&, const location&);

  // Similar (and implicit-convertible) to the above but stores a copy of the
  // path.
  //
  class location_value: public location
  {
  public:
    path_name_value file;

    location_value ();

    location_value (const location&);

    location_value (location_value&&) noexcept;
    location_value (const location_value&);
    location_value& operator= (location_value&&) noexcept;
    location_value& operator= (const location_value&);
  };

  // See context.
  //
  enum class run_phase {load, match, execute};

  LIBBUILD2_SYMEXPORT ostream&
  operator<< (ostream&, run_phase); // utility.cxx
}

// <libbuild2/name.hxx>
//
#include <libbuild2/name.hxx>

#include <libbuild2/types.ixx>

#endif // LIBBUILD2_TYPES_HXX
