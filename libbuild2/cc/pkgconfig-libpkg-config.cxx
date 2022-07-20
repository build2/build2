// file      : libbuild2/cc/pkgconfig-libpkg-config.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_BOOTSTRAP

#include <libbuild2/cc/pkgconfig.hxx>

#include <new> // std::bad_alloc

#include <libbuild2/diagnostics.hxx>

namespace build2
{
  namespace cc
  {
    // The package dependency traversal depth limit.
    //
    static const int max_depth = 100;

    static void
    error_handler (unsigned int,
                   const char* file,
                   size_t line,
                   const char* msg,
                   const pkg_config_client_t*,
                   const void*)
    {
      if (file != nullptr)
      {
        path_name n (file);
        const location l (n, static_cast<uint64_t> (line));
        error (l) << msg;
      }
      else
        error << msg;
    }

    // Deleters.
    //
    struct fragments_deleter
    {
      void operator() (pkg_config_list_t* f) const
      {
        pkg_config_fragment_free (f);
      }
    };

    // Convert fragments to strings. Skip the -I/-L options that refer to
    // system directories.
    //
    static strings
    to_strings (const pkg_config_list_t& frags,
                char type,
                const pkg_config_list_t& sysdirs)
    {
      assert (type == 'I' || type == 'L');

      strings r;
      auto add = [&r] (const pkg_config_fragment_t* frag)
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
      const pkg_config_fragment_t* opt (nullptr);

      pkg_config_node_t *node;
      LIBPKG_CONFIG_FOREACH_LIST_ENTRY(frags.head, node)
      {
        auto frag (static_cast<const pkg_config_fragment_t*> (node->data));

        // Add the separated option and directory, unless the latest is a
        // system one.
        //
        if (opt != nullptr)
        {
          assert (frag->type == '\0'); // See pkg_config_fragment_add().

          if (!pkg_config_path_match_list (frag->data, &sysdirs))
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

          if (pkg_config_path_match_list (frag->data, &sysdirs))
            continue;
        }

        add (frag);
      }

      if (opt != nullptr) // Add the dangling option.
        add (opt);

      return r;
    }

    // Note that some libpkg-config functions can potentially return NULL,
    // failing to allocate the required memory block. However, we will not
    // check the returned value for NULL as the library doesn't do so, prior
    // to filling the allocated structures. So such a code complication on our
    // side would be useless. Also, for some functions the NULL result has a
    // special semantics, for example "not found". @@ TODO: can we fix this?
    // This is now somewhat addressed, see the eflags argument in
    // pkg_config_pkg_find().
    //
    pkgconfig::
    pkgconfig (path_type p,
               const dir_paths& pc_dirs,
               const dir_paths& sys_lib_dirs,
               const dir_paths& sys_hdr_dirs)
        : path (move (p))
    {
      auto add_dirs = [] (pkg_config_list_t& dir_list,
                          const dir_paths& dirs,
                          bool suppress_dups)
      {
        for (const auto& d: dirs)
          pkg_config_path_add (d.string ().c_str (), &dir_list, suppress_dups);
      };

      // Initialize the client handle.
      //
      // Note: omit initializing the filters from environment/defaults.
      //
      unique_ptr<pkg_config_client_t, void (*) (pkg_config_client_t*)> c (
        pkg_config_client_new (&error_handler,
                               nullptr /* handler_data */,
                               false   /* init_filters */),
        [] (pkg_config_client_t* c) {pkg_config_client_free (c);});

      if (c == nullptr)
        throw std::bad_alloc ();

      add_dirs (c->filter_libdirs, sys_lib_dirs, false /* suppress_dups */);
      add_dirs (c->filter_includedirs, sys_hdr_dirs, false /* suppress_dups */);

      // Note that the loaded file directory is added to the (for now empty)
      // .pc file search list. Also note that loading of the dependency
      // packages is delayed until the flags retrieval, and their file
      // directories are not added to the search list.
      //
      // @@ Hm, is there a way to force this resolution? But we may not
      //    need this (e.g., only loading from variables).
      //
      unsigned int e;
      pkg_ = pkg_config_pkg_find (c.get (), path.string ().c_str (), &e);

      if (pkg_ == nullptr)
      {
        if (e == LIBPKG_CONFIG_ERRF_OK)
          fail << "package '" << path << "' not found";
        else
          // Diagnostics should have already been issued except for allocation
          // errors.
          //
          fail << "unable to load package '" << path << "'";
      }

      // Add the .pc file search directories.
      //
      assert (c->dir_list.length == 1); // Package file directory (see above).
      add_dirs (c->dir_list, pc_dirs, true /* suppress_dups */);

      client_ = c.release ();
    }

    void pkgconfig::
    free ()
    {
      assert (client_ != nullptr && pkg_ != nullptr);

      pkg_config_pkg_unref (client_, pkg_);
      pkg_config_client_free (client_);
    }

    strings pkgconfig::
    cflags (bool stat) const
    {
      assert (client_ != nullptr); // Must not be empty.

      pkg_config_client_set_flags (
        client_,
        // Walk through the private package dependencies (Requires.private)
        // besides the public ones while collecting the flags. Note that we do
        // this for both static and shared linking. @@ Hm, I wonder why...?
        //
        LIBPKG_CONFIG_PKG_PKGF_SEARCH_PRIVATE |

        // Collect flags from Cflags.private besides those from Cflags for the
        // static linking.
        //
        (stat
         ? LIBPKG_CONFIG_PKG_PKGF_ADD_PRIVATE_FRAGMENTS
         : 0));

      pkg_config_list_t f = LIBPKG_CONFIG_LIST_INITIALIZER; // Empty list.
      int e (pkg_config_pkg_cflags (client_, pkg_, &f, max_depth));

      if (e != LIBPKG_CONFIG_ERRF_OK)
        throw failed (); // Assume the diagnostics is issued.

      unique_ptr<pkg_config_list_t, fragments_deleter> fd (&f);
      return to_strings (f, 'I', client_->filter_includedirs);
    }

    strings pkgconfig::
    libs (bool stat) const
    {
      assert (client_ != nullptr); // Must not be empty.

      pkg_config_client_set_flags (
        client_,
        // Additionally collect flags from the private dependency packages
        // (see above) and from the Libs.private value for the static linking.
        //
        (stat
         ? LIBPKG_CONFIG_PKG_PKGF_SEARCH_PRIVATE |
           LIBPKG_CONFIG_PKG_PKGF_ADD_PRIVATE_FRAGMENTS
         : 0));

      pkg_config_list_t f = LIBPKG_CONFIG_LIST_INITIALIZER; // Empty list.
      int e (pkg_config_pkg_libs (client_, pkg_, &f, max_depth));

      if (e != LIBPKG_CONFIG_ERRF_OK)
        throw failed (); // Assume the diagnostics is issued.

      unique_ptr<pkg_config_list_t, fragments_deleter> fd (&f);
      return to_strings (f, 'L', client_->filter_libdirs);
    }

    optional<string> pkgconfig::
    variable (const char* name) const
    {
      assert (client_ != nullptr); // Must not be empty.

      const char* r (pkg_config_tuple_find (client_, &pkg_->vars, name));
      return r != nullptr ? optional<string> (r) : nullopt;
    }
  }
}

#endif // BUILD2_BOOTSTRAP
