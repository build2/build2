// file      : libbuild2/filesystem.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  template <typename T>
  fs_status<butl::rmfile_status>
  rmfile (context&, const path&, const T&, uint16_t);

  template <typename T>
  inline fs_status<rmfile_status>
  rmfile (const path& f, const T& t, uint16_t v)
  {
    return rmfile (t.ctx, f, t, v);
  }

  inline fs_status<rmfile_status>
  rmfile (context& ctx, const path& f, uint16_t v)
  {
    return rmfile (ctx, f, f, v);
  }

  template <typename T>
  fs_status<rmdir_status>
  rmdir (context&, const dir_path&, const T&, uint16_t);

  template <typename T>
  inline fs_status<rmdir_status>
  rmdir (const dir_path& d, const T& t, uint16_t v)
  {
    return rmdir (t.ctx, d, t, v);
  }

  inline fs_status<rmdir_status>
  rmdir (context& ctx, const dir_path& d, uint16_t v)
  {
    return rmdir (ctx, d, d, v);
  }
}
