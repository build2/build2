// file      : libbuild2/cc/pkgconfig.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBUILD2_CC_PKGCONFIG_HXX
#define LIBBUILD2_CC_PKGCONFIG_HXX

// In order not to complicate the bootstrap procedure with libpkg-config
// building, exclude functionality that involves reading of .pc files.
//
#ifndef BUILD2_BOOTSTRAP

#ifndef BUILD2_LIBPKGCONF
#  include <libpkg-config/pkg-config.h>
#else
#  include <libpkgconf/libpkgconf.h>
#endif

#include <libbuild2/types.hxx>
#include <libbuild2/utility.hxx>

namespace build2
{
  namespace cc
  {
    // Load package information from a .pc file. Filter out the -I/-L options
    // that refer to system directories. This makes sure all the system search
    // directories are "pushed" to the back which minimizes the chances of
    // picking up wrong (e.g., old installed version) header/library.
    //
    // Note that the prerequisite package .pc files search order is as
    // follows:
    //
    // - in the directory of the specified file
    // - in pc_dirs directories (in the specified order)
    //
    // Issue diagnostics and throw failed on any errors.
    //
    class pkgconfig
    {
    public:
      using path_type = build2::path;

      path_type path;

    public:
      pkgconfig (path_type,
                 const dir_paths& pc_dirs,
                 const dir_paths& sys_hdr_dirs,
                 const dir_paths& sys_lib_dirs);

      // Create an unloaded/empty object. Querying package information on such
      // an object is illegal.
      //
      pkgconfig () = default;
      ~pkgconfig ();

      // Movable-only type.
      //
      pkgconfig (pkgconfig&&) noexcept;
      pkgconfig& operator= (pkgconfig&&) noexcept;

      pkgconfig (const pkgconfig&) = delete;
      pkgconfig& operator= (const pkgconfig&) = delete;

      strings
      cflags (bool static_) const;

      strings
      libs (bool static_) const;

      optional<string>
      variable (const char*) const;

      optional<string>
      variable (const string& s) const {return variable (s.c_str ());}

    private:
      void
      free ();

#ifndef BUILD2_LIBPKGCONF
      pkg_config_client_t* client_ = nullptr;
      pkg_config_pkg_t* pkg_ = nullptr;
#else
      pkgconf_client_t* client_ = nullptr;
      pkgconf_pkg_t* pkg_ = nullptr;
#endif
    };

    inline pkgconfig::
    ~pkgconfig ()
    {
      if (client_ != nullptr) // Not empty.
        free ();
    }

    inline pkgconfig::
    pkgconfig (pkgconfig&& p) noexcept
        : path (move (p.path)),
          client_ (p.client_),
          pkg_ (p.pkg_)
    {
      p.client_ = nullptr;
      p.pkg_ = nullptr;
    }

    inline pkgconfig& pkgconfig::
    operator= (pkgconfig&& p) noexcept
    {
      if (this != &p)
      {
        if (client_ != nullptr) // Not empty.
          free ();

        path = move (p.path);
        client_ = p.client_;
        pkg_ = p.pkg_;

        p.client_ = nullptr;
        p.pkg_ = nullptr;
      }
      return *this;
    }
  }
}

#endif // BUILD2_BOOTSTRAP

#endif // LIBBUILD2_CC_PKGCONFIG_HXX
