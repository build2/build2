// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

#ifndef LIBBUILD2_SCRIPT_BUILTIN_OPTIONS_HXX
#define LIBBUILD2_SCRIPT_BUILTIN_OPTIONS_HXX

// Begin prologue.
//
//
// End prologue.

#include <vector>
#include <iosfwd>
#include <string>
#include <cstddef>
#include <exception>

#ifndef CLI_POTENTIALLY_UNUSED
#  if defined(_MSC_VER) || defined(__xlC__)
#    define CLI_POTENTIALLY_UNUSED(x) (void*)&x
#  else
#    define CLI_POTENTIALLY_UNUSED(x) (void)x
#  endif
#endif

namespace build2
{
  namespace script
  {
    namespace cli
    {
      class unknown_mode
      {
        public:
        enum value
        {
          skip,
          stop,
          fail
        };

        unknown_mode (value);

        operator value () const 
        {
          return v_;
        }

        private:
        value v_;
      };

      // Exceptions.
      //

      class exception: public std::exception
      {
        public:
        virtual void
        print (::std::ostream&) const = 0;
      };

      ::std::ostream&
      operator<< (::std::ostream&, const exception&);

      class unknown_option: public exception
      {
        public:
        virtual
        ~unknown_option () throw ();

        unknown_option (const std::string& option);

        const std::string&
        option () const;

        virtual void
        print (::std::ostream&) const;

        virtual const char*
        what () const throw ();

        private:
        std::string option_;
      };

      class unknown_argument: public exception
      {
        public:
        virtual
        ~unknown_argument () throw ();

        unknown_argument (const std::string& argument);

        const std::string&
        argument () const;

        virtual void
        print (::std::ostream&) const;

        virtual const char*
        what () const throw ();

        private:
        std::string argument_;
      };

      class missing_value: public exception
      {
        public:
        virtual
        ~missing_value () throw ();

        missing_value (const std::string& option);

        const std::string&
        option () const;

        virtual void
        print (::std::ostream&) const;

        virtual const char*
        what () const throw ();

        private:
        std::string option_;
      };

      class invalid_value: public exception
      {
        public:
        virtual
        ~invalid_value () throw ();

        invalid_value (const std::string& option,
                       const std::string& value,
                       const std::string& message = std::string ());

        const std::string&
        option () const;

        const std::string&
        value () const;

        const std::string&
        message () const;

        virtual void
        print (::std::ostream&) const;

        virtual const char*
        what () const throw ();

        private:
        std::string option_;
        std::string value_;
        std::string message_;
      };

      class eos_reached: public exception
      {
        public:
        virtual void
        print (::std::ostream&) const;

        virtual const char*
        what () const throw ();
      };

      // Command line argument scanner interface.
      //
      // The values returned by next() are guaranteed to be valid
      // for the two previous arguments up until a call to a third
      // peek() or next().
      //
      // The position() function returns a monotonically-increasing
      // number which, if stored, can later be used to determine the
      // relative position of the argument returned by the following
      // call to next(). Note that if multiple scanners are used to
      // extract arguments from multiple sources, then the end
      // position of the previous scanner should be used as the
      // start position of the next.
      //
      class scanner
      {
        public:
        virtual
        ~scanner ();

        virtual bool
        more () = 0;

        virtual const char*
        peek () = 0;

        virtual const char*
        next () = 0;

        virtual void
        skip () = 0;

        virtual std::size_t
        position () = 0;
      };

      class argv_scanner: public scanner
      {
        public:
        argv_scanner (int& argc,
                      char** argv,
                      bool erase = false,
                      std::size_t start_position = 0);

        argv_scanner (int start,
                      int& argc,
                      char** argv,
                      bool erase = false,
                      std::size_t start_position = 0);

        int
        end () const;

        virtual bool
        more ();

        virtual const char*
        peek ();

        virtual const char*
        next ();

        virtual void
        skip ();

        virtual std::size_t
        position ();

        protected:
        std::size_t start_position_;
        int i_;
        int& argc_;
        char** argv_;
        bool erase_;
      };

      class vector_scanner: public scanner
      {
        public:
        vector_scanner (const std::vector<std::string>&,
                        std::size_t start = 0,
                        std::size_t start_position = 0);

        std::size_t
        end () const;

        void
        reset (std::size_t start = 0, std::size_t start_position = 0);

        virtual bool
        more ();

        virtual const char*
        peek ();

        virtual const char*
        next ();

        virtual void
        skip ();

        virtual std::size_t
        position ();

        private:
        std::size_t start_position_;
        const std::vector<std::string>& v_;
        std::size_t i_;
      };

      template <typename X>
      struct parser;
    }
  }
}

#include <libbuild2/types.hxx>

namespace build2
{
  namespace script
  {
    class set_options
    {
      public:
      set_options ();

      set_options (int& argc,
                   char** argv,
                   bool erase = false,
                   ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                   ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      set_options (int start,
                   int& argc,
                   char** argv,
                   bool erase = false,
                   ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                   ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      set_options (int& argc,
                   char** argv,
                   int& end,
                   bool erase = false,
                   ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                   ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      set_options (int start,
                   int& argc,
                   char** argv,
                   int& end,
                   bool erase = false,
                   ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                   ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      set_options (::build2::script::cli::scanner&,
                   ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                   ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      // Option accessors and modifiers.
      //
      const bool&
      exact () const;

      bool&
      exact ();

      void
      exact (const bool&);

      const bool&
      newline () const;

      bool&
      newline ();

      void
      newline (const bool&);

      const bool&
      whitespace () const;

      bool&
      whitespace ();

      void
      whitespace (const bool&);

      // Implementation details.
      //
      protected:
      bool
      _parse (const char*, ::build2::script::cli::scanner&);

      private:
      bool
      _parse (::build2::script::cli::scanner&,
              ::build2::script::cli::unknown_mode option,
              ::build2::script::cli::unknown_mode argument);

      public:
      bool exact_;
      bool newline_;
      bool whitespace_;
    };

    class timeout_options
    {
      public:
      timeout_options ();

      timeout_options (int& argc,
                       char** argv,
                       bool erase = false,
                       ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                       ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      timeout_options (int start,
                       int& argc,
                       char** argv,
                       bool erase = false,
                       ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                       ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      timeout_options (int& argc,
                       char** argv,
                       int& end,
                       bool erase = false,
                       ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                       ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      timeout_options (int start,
                       int& argc,
                       char** argv,
                       int& end,
                       bool erase = false,
                       ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                       ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      timeout_options (::build2::script::cli::scanner&,
                       ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                       ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      // Option accessors and modifiers.
      //
      const bool&
      success () const;

      bool&
      success ();

      void
      success (const bool&);

      // Implementation details.
      //
      protected:
      bool
      _parse (const char*, ::build2::script::cli::scanner&);

      private:
      bool
      _parse (::build2::script::cli::scanner&,
              ::build2::script::cli::unknown_mode option,
              ::build2::script::cli::unknown_mode argument);

      public:
      bool success_;
    };

    class export_options
    {
      public:
      export_options ();

      export_options (int& argc,
                      char** argv,
                      bool erase = false,
                      ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                      ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      export_options (int start,
                      int& argc,
                      char** argv,
                      bool erase = false,
                      ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                      ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      export_options (int& argc,
                      char** argv,
                      int& end,
                      bool erase = false,
                      ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                      ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      export_options (int start,
                      int& argc,
                      char** argv,
                      int& end,
                      bool erase = false,
                      ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                      ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      export_options (::build2::script::cli::scanner&,
                      ::build2::script::cli::unknown_mode option = ::build2::script::cli::unknown_mode::fail,
                      ::build2::script::cli::unknown_mode argument = ::build2::script::cli::unknown_mode::stop);

      // Option accessors and modifiers.
      //
      const vector<string>&
      unset () const;

      vector<string>&
      unset ();

      void
      unset (const vector<string>&);

      bool
      unset_specified () const;

      void
      unset_specified (bool);

      const vector<string>&
      clear () const;

      vector<string>&
      clear ();

      void
      clear (const vector<string>&);

      bool
      clear_specified () const;

      void
      clear_specified (bool);

      // Implementation details.
      //
      protected:
      bool
      _parse (const char*, ::build2::script::cli::scanner&);

      private:
      bool
      _parse (::build2::script::cli::scanner&,
              ::build2::script::cli::unknown_mode option,
              ::build2::script::cli::unknown_mode argument);

      public:
      vector<string> unset_;
      bool unset_specified_;
      vector<string> clear_;
      bool clear_specified_;
    };
  }
}

#include <libbuild2/script/builtin-options.ixx>

// Begin epilogue.
//
//
// End epilogue.

#endif // LIBBUILD2_SCRIPT_BUILTIN_OPTIONS_HXX
