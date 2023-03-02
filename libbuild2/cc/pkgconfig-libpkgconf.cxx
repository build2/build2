// file      : libbuild2/cc/pkgconfig-libpkgconf.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_BOOTSTRAP

#include <libbuild2/cc/pkgconfig.hxx>

#include <libbuild2/diagnostics.hxx>

// Note that the libpkgconf library did not used to provide the version macro
// that we could use to compile the code conditionally against different API
// versions. Thus, we need to sense the pkgconf_client_new() function
// signature ourselves to call it properly.
//
namespace details
{
  void*
  pkgconf_cross_personality_default (); // Never called.
}

using namespace details;

template <typename H>
static inline pkgconf_client_t*
call_pkgconf_client_new (pkgconf_client_t* (*f) (H, void*),
                         H error_handler,
                         void* error_handler_data)
{
  return f (error_handler, error_handler_data);
}

template <typename H, typename P>
static inline pkgconf_client_t*
call_pkgconf_client_new (pkgconf_client_t* (*f) (H, void*, P),
                         H error_handler,
                         void* error_handler_data)
{
  return f (error_handler,
            error_handler_data,
            ::pkgconf_cross_personality_default ());
}

namespace build2
{
  namespace cc
  {
    // The libpkgconf library is not thread-safe, even on the pkgconf_client_t
    // level (see issue #128 for details). While it seems that the obvious
    // thread-safety issues are fixed, the default personality initialization,
    // which is still not thread-safe. So let's keep the mutex for now not to
    // introduce potential issues.
    //
    static mutex pkgconf_mutex;

    // The package dependency traversal depth limit.
    //
    static const int pkgconf_max_depth = 100;

    // Normally the error_handler() callback can be called multiple times to
    // report a single error (once per message line), to produce a multi-line
    // message like this:
    //
    //   Package foo was not found in the pkg-config search path.\n
    //   Perhaps you should add the directory containing `foo.pc'\n
    //   to the PKG_CONFIG_PATH environment variable\n
    //   Package 'foo', required by 'bar', not found\n
    //
    // For the above example callback will be called 4 times. To suppress all
    // the junk we will use PKGCONF_PKG_PKGF_SIMPLIFY_ERRORS to get just:
    //
    //   Package 'foo', required by 'bar', not found\n
    //
    // Also disable merging options like -framework into a single fragment, if
    // possible.
    //
    static const int pkgconf_flags =
      PKGCONF_PKG_PKGF_SIMPLIFY_ERRORS
      | PKGCONF_PKG_PKGF_SKIP_PROVIDES
#ifdef PKGCONF_PKG_PKGF_DONT_MERGE_SPECIAL_FRAGMENTS
      | PKGCONF_PKG_PKGF_DONT_MERGE_SPECIAL_FRAGMENTS
#endif
      ;

#if defined(LIBPKGCONF_VERSION) && LIBPKGCONF_VERSION >= 10900
    static bool
    pkgconf_error_handler (const char* msg,
                           const pkgconf_client_t*,
                           void*)
#else
    static bool
    pkgconf_error_handler (const char* msg,
                           const pkgconf_client_t*,
                           const void*)
#endif
    {
      error << runtime_error (msg); // Sanitize the message (trailing dot).
      return true;
    }

    // Deleters. Note that they are thread-safe.
    //
    struct fragments_deleter
    {
      void operator() (pkgconf_list_t* f) const {pkgconf_fragment_free (f);}
    };

    // Convert fragments to strings. Skip the -I/-L options that refer to system
    // directories.
    //
    static strings
    to_strings (const pkgconf_list_t& frags,
                char type,
                const pkgconf_list_t& sysdirs)
    {
      assert (type == 'I' || type == 'L');

      strings r;

      auto add = [&r] (const pkgconf_fragment_t* frag)
      {
        string s;
        if (frag->type != '\0')
        {
          s += '-';
          s += frag->type;
        }

        s += frag->data;
        r.push_back (move (s));
      };

      // Option that is separated from its value, for example:
      //
      // -I /usr/lib
      //
      const pkgconf_fragment_t* opt (nullptr);

      pkgconf_node_t *node;
      PKGCONF_FOREACH_LIST_ENTRY(frags.head, node)
      {
        auto frag (static_cast<const pkgconf_fragment_t*> (node->data));

        // Add the separated option and directory, unless the latest is a
        // system one.
        //
        if (opt != nullptr)
        {
          // Note that we should restore the directory path that was
          // (mis)interpreted as an option, for example:
          //
          // -I -Ifoo
          //
          // In the above example option '-I' is followed by directory
          // '-Ifoo', which is represented by libpkgconf library as fragment
          // 'foo' with type 'I'.
          //
          if (!pkgconf_path_match_list (
                frag->type == '\0'
                ? frag->data
                : (string ({'-', frag->type}) + frag->data).c_str (),
                &sysdirs))
          {
            add (opt);
            add (frag);
          }

          opt = nullptr;
          continue;
        }

        // Skip the -I/-L option if it refers to a system directory.
        //
        if (frag->type == type)
        {
          // The option is separated from a value, that will (presumably)
          // follow.
          //
          if (*frag->data == '\0')
          {
            opt = frag;
            continue;
          }

          if (pkgconf_path_match_list (frag->data, &sysdirs))
            continue;
        }

        add (frag);
      }

      if (opt != nullptr) // Add the dangling option.
        add (opt);

      return r;
    }

    // Note that some libpkgconf functions can potentially return NULL,
    // failing to allocate the required memory block. However, we will not
    // check the returned value for NULL as the library doesn't do so, prior
    // to filling the allocated structures. So such a code complication on our
    // side would be useless. Also, for some functions the NULL result has a
    // special semantics, for example "not found".
    //
    pkgconfig::
    pkgconfig (path_type p,
               const dir_paths& pc_dirs,
               const dir_paths& sys_lib_dirs,
               const dir_paths& sys_hdr_dirs)
        : path (move (p))
    {
      auto add_dirs = [] (pkgconf_list_t& dir_list,
                          const dir_paths& dirs,
                          bool suppress_dups,
                          bool cleanup = false)
      {
        if (cleanup)
        {
          pkgconf_path_free (&dir_list);
          dir_list = PKGCONF_LIST_INITIALIZER;
        }

        for (const auto& d: dirs)
          pkgconf_path_add (d.string ().c_str (), &dir_list, suppress_dups);
      };

      mlock l (pkgconf_mutex);

      // Initialize the client handle.
      //
      unique_ptr<pkgconf_client_t, void (*) (pkgconf_client_t*)> c (
        call_pkgconf_client_new (&pkgconf_client_new,
                                 pkgconf_error_handler,
                                 nullptr /* handler_data */),
        [] (pkgconf_client_t* c) {pkgconf_client_free (c);});

      pkgconf_client_set_flags (c.get (), pkgconf_flags);

      // Note that the system header and library directory lists are
      // automatically pre-filled by the pkgconf_client_new() call (see
      // above). We will re-create these lists from scratch.
      //
      add_dirs (c->filter_libdirs,
                sys_lib_dirs,
                false /* suppress_dups */,
                true  /* cleanup */);

      add_dirs (c->filter_includedirs,
                sys_hdr_dirs,
                false /* suppress_dups */,
                true  /* cleanup */);

      // Note that the loaded file directory is added to the (yet empty)
      // search list. Also note that loading of the prerequisite packages is
      // delayed until flags retrieval, and their file directories are not
      // added to the search list.
      //
      pkg_ = pkgconf_pkg_find (c.get (), path.string ().c_str ());

      if (pkg_ == nullptr)
        fail << "package '" << path << "' not found or invalid";

      // Add the .pc file search directories.
      //
      assert (c->dir_list.length == 1); // Package file directory (see above).
      add_dirs (c->dir_list, pc_dirs, true /* suppress_dups */);

      client_ = c.release ();
    }

    void pkgconfig::
    free ()
    {
      assert (pkg_ != nullptr);

      mlock l (pkgconf_mutex);
      pkgconf_pkg_unref (client_, pkg_);
      pkgconf_client_free (client_);
    }

    strings pkgconfig::
    cflags (bool stat) const
    {
      assert (client_ != nullptr); // Must not be empty.

      mlock l (pkgconf_mutex);

      pkgconf_client_set_flags (
        client_,
        pkgconf_flags |

        // Walk through the private package dependencies (Requires.private)
        // besides the public ones while collecting the flags. Note that we do
        // this for both static and shared linking.
        //
        PKGCONF_PKG_PKGF_SEARCH_PRIVATE |

        // Collect flags from Cflags.private besides those from Cflags for the
        // static linking.
        //
        (stat
         ? PKGCONF_PKG_PKGF_MERGE_PRIVATE_FRAGMENTS
         : 0));

      pkgconf_list_t f = PKGCONF_LIST_INITIALIZER; // Aggregate initialization.
      int e (pkgconf_pkg_cflags (client_, pkg_, &f, pkgconf_max_depth));

      if (e != PKGCONF_PKG_ERRF_OK)
        throw failed (); // Assume the diagnostics is issued.

      unique_ptr<pkgconf_list_t, fragments_deleter> fd (&f); // Auto-deleter.
      return to_strings (f, 'I', client_->filter_includedirs);
    }

    strings pkgconfig::
    libs (bool stat) const
    {
      assert (client_ != nullptr); // Must not be empty.

      mlock l (pkgconf_mutex);

      pkgconf_client_set_flags (
        client_,
        pkgconf_flags |

        // Additionally collect flags from the private dependency packages
        // (see above) and from the Libs.private value for the static linking.
        //
        (stat
         ? PKGCONF_PKG_PKGF_SEARCH_PRIVATE |
         PKGCONF_PKG_PKGF_MERGE_PRIVATE_FRAGMENTS
         : 0));

      pkgconf_list_t f = PKGCONF_LIST_INITIALIZER; // Aggregate initialization.
      int e (pkgconf_pkg_libs (client_, pkg_, &f, pkgconf_max_depth));

      if (e != PKGCONF_PKG_ERRF_OK)
        throw failed (); // Assume the diagnostics is issued.

      unique_ptr<pkgconf_list_t, fragments_deleter> fd (&f); // Auto-deleter.
      return to_strings (f, 'L', client_->filter_libdirs);
    }

    optional<string> pkgconfig::
    variable (const char* name) const
    {
      assert (client_ != nullptr); // Must not be empty.

      mlock l (pkgconf_mutex);
      const char* r (pkgconf_tuple_find (client_, &pkg_->vars, name));
      return r != nullptr ? optional<string> (r) : nullopt;
    }
  }
}

#endif // BUILD2_BOOTSTRAP
