// file      : build2/test/script/builtin.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef BUILD2_TEST_SCRIPT_BUILTIN_HXX
#define BUILD2_TEST_SCRIPT_BUILTIN_HXX

#include <map>

#include <build2/types.hxx>
#include <build2/utility.hxx>

namespace build2
{
  namespace test
  {
    namespace script
    {
      class scope;

      // A process/thread-like object representing a running builtin.
      //
      // For now, instead of allocating the result storage dynamically, we
      // expect it to be provided by the caller.
      //
      class builtin
      {
      public:
        uint8_t
        wait () {if (t_.joinable ()) t_.join (); return r_;}

        ~builtin () {wait ();}

      public:
        builtin (uint8_t& r, thread&& t = thread ()): r_ (r), t_ (move (t)) {}

        builtin (builtin&&) = default;
        builtin& operator= (builtin&&) = default;

      private:
        uint8_t& r_;
        thread t_;
      };

      // Start builtin command. Throw system_error on failure.
      //
      // Note that unlike argc/argv, our args don't include the program name.
      //
      using builtin_func = builtin (scope&,
                                    uint8_t& result,
                                    const strings& args,
                                    auto_fd in, auto_fd out, auto_fd err);

      class builtin_map: public std::map<string, builtin_func*>
      {
      public:
        using base = std::map<string, builtin_func*>;
        using base::base;

        // Return NULL if not a builtin.
        //
        builtin_func*
        find (const string& n) const
        {
          auto i (base::find (n));
          return i != end () ? i->second : nullptr;
        }
      };

      extern const builtin_map builtins;
    }
  }
}

#endif // BUILD2_TEST_SCRIPT_BUILTIN_HXX
