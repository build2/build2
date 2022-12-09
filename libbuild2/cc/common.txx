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
      auto p (ctx.targets.insert_locked (
                T::static_type,
                move (dir),
                dir_path (out.effect_string ()).normalize (),
                name,
                move (ext),
                target_decl::implied,
                trace));

      if (exist && p.second)
        throw non_existent_library {p.first.template as<mtime_target> ()};

      r = &p.first.template as<T> ();
      return move (p.second);
    }
  }
}
