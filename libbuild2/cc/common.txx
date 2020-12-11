// file      : libbuild2/cc/common.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  namespace cc
  {
    // Insert a target "tagging" it with the specified process path and
    // verifying that it already exists if requested. Return the lock.
    //
    template <typename T>
    ulock common::
    insert_library (context& ctx,
                    T*& r,
                    string name,
                    dir_path dir,
                    const process_path& out,
                    optional<string> ext,
                    bool exist,
                    tracer& trace)
    {
      auto p (ctx.targets.insert_locked (T::static_type,
                                         move (dir),
                                         path_cast<dir_path> (out.effect),
                                         name,
                                         move (ext),
                                         target_decl::implied,
                                         trace));

      assert (!exist || !p.second);
      r = &p.first.template as<T> ();
      return move (p.second);
    }
  }
}
