// file      : libbuild2/bin/def-rule.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/bin/def-rule.hxx>

#include <libbuild2/depdb.hxx>
#include <libbuild2/scope.hxx>
#include <libbuild2/target.hxx>
#include <libbuild2/algorithm.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/bin/target.hxx>
#include <libbuild2/bin/utility.hxx>

namespace build2
{
  namespace bin
  {
    // In C global uninitialized data becomes a "common symbol" (an equivalent
    // definition compiled as C++ results in a BSS symbol) which allows some
    // archaic merging of multiple such definitions during linking (see GNU ld
    // --warn-common for background). Note that this merging may happen with
    // other data symbol types, not just common.
    //
    struct symbols
    {
      set<string> d; // data
      set<string> r; // read-only data
      set<string> b; // uninitialized data (BSS)
      set<string> c; // common uninitialized data
      set<string> t; // text (code)
    };

    static void
    read_dumpbin (diag_buffer& dbuf, ifdstream& is, symbols& syms)
    {
      // Note: io_error is handled by the caller.

      // Lines that describe symbols look like:
      //
      // 0   1        2      3          4            5 6
      // IDX OFFSET   SECT   SYMTYPE    VISIBILITY     SYMNAME
      // ----------------------------------------------------------------------
      // 02E 00000130 SECTA  notype     External     | _standbyState
      // 02F 00000009 SECT9  notype     Static       | _LocalRecoveryInProgress
      // 064 00000020 SECTC  notype ()  Static       | _XLogCheckBuffer
      // 065 00000000 UNDEF  notype ()  External     | _BufferGetTag
      //
      // IDX is the symbol index and OFFSET is its offset.
      //
      // SECT[ION] is the name of the section where the symbol is defined. If
      // UNDEF, then it's a symbol to be resolved at link time from another
      // object file.
      //
      // SYMTYPE is always notype for C/C++ symbols as there's no typeinfo and
      // no way to get the symbol type from name (de)mangling. However, we
      // care if "notype" is suffixed by "()" or not. The presence of () means
      // the symbol is a function, the absence means it isn't.
      //
      // VISIBILITY indicates whether it's a compilation-unit local static
      // symbol ("Static"), or whether it's available for use from other
      // compilation units ("External"). Note that there are other values,
      // such as "WeakExternal", and "Label".
      //
      // SYMNAME is the symbol name.
      //
      // The first symbol in each section appears to specify the section type,
      // for example:
      //
      // 006 00000000 SECT3  notype    Static       | .rdata
      // B44 00000000 SECT4  notype    Static       | .rdata$r
      // AA2 00000000 SECT5  notype    Static       | .bss
      //
      // Note that an UNDEF data symbol with non-zero OFFSET is a "common
      // symbol", equivalent to the nm `C` type.
      //
      // We keep a map of read-only (.rdata, .xdata) and uninitialized (.bss)
      // sections to their types (R and B, respectively). If a section is not
      // found in this map, then it's assumed to be normal data (.data).
      //
      auto parse_line = [&syms,
                         secs = map<string, char> ()] (const string& l) mutable
      {
        size_t b (0), e (0), n;

        // IDX (note that it can be more than 3 characters).
        //
        if (next_word (l, b, e) == 0)
          return;

        // OFFSET (always 8 characters).
        //
        n = next_word (l, b, e);

        if (n != 8)
          return;

        string off (l, b, n);

        // SECT
        //
        n = next_word (l, b, e);

        if (n == 0)
          return;

        string sec (l, b, n);

        // TYPE
        //
        n = next_word (l, b, e);

        if (l.compare (b, n, "notype") != 0)
          return;

        bool dat;
        if (l[e] == ' ' && l[e + 1] == '(' && l[e + 2] == ')')
        {
          e += 3;
          dat = false;
        }
        else
          dat = true;

        // VISIBILITY
        //
        n = next_word (l, b, e);

        if (n == 0)
          return;

        string vis (l, b, n);

        // |
        //
        n = next_word (l, b, e);

        if (n != 1 || l[b] != '|')
          return;

        // SYMNAME
        //
        n = next_word (l, b, e);

        if (n == 0)
          return;

        string s (l, b, n);

        // See if this is the section type symbol.
        //
        if (dat &&
            off == "00000000" &&
            sec != "UNDEF"    &&
            vis == "Static"   &&
            s[0] == '.')
        {
          auto cmp = [&s] (const char* n, size_t l)
          {
            return s.compare (0, l, n) == 0 && (s[l] == '\0' || s[l] == '$');
          };

          if      (cmp (".rdata", 6) ||
                   cmp (".xdata", 6))    secs.emplace (move (sec), 'R');
          else if (cmp (".bss",   4))    secs.emplace (move (sec), 'B');

          return;
        }

        // We can only export extern symbols.
        //
        if (vis != "External")
          return;

        if (dat)
        {
          if (sec != "UNDEF")
          {
            auto i (secs.find (sec));
            switch (i == secs.end () ? 'D' : i->second)
            {
            case 'D': syms.d.insert (move (s)); break;
            case 'R': syms.r.insert (move (s)); break;
            case 'B': syms.b.insert (move (s)); break;
            }
          }
          else
          {
            if (off != "00000000")
              syms.c.insert (move (s));
          }
        }
        else
        {
          if (sec != "UNDEF")
            syms.t.insert (move (s));
        }
      };

      // Read until we reach EOF on all streams.
      //
      // Note that if dbuf is not opened, then we automatically get an
      // inactive nullfd entry.
      //
      fdselect_set fds {is.fd (), dbuf.is.fd ()};
      fdselect_state& ist (fds[0]);
      fdselect_state& dst (fds[1]);

      for (string l; ist.fd != nullfd || dst.fd != nullfd; )
      {
        if (ist.fd != nullfd && getline_non_blocking (is, l))
        {
          if (eof (is))
            ist.fd = nullfd;
          else
          {
            parse_line (l);
            l.clear ();
          }

          continue;
        }

        ifdselect (fds);

        if (dst.ready)
        {
          if (!dbuf.read ())
            dst.fd = nullfd;
        }
      }
    }

    static void
    read_posix_nm (diag_buffer& dbuf, ifdstream& is, symbols& syms)
    {
      // Note: io_error is handled by the caller.

      // Lines that describe symbols look like:
      //
      // <NAME> <TYPE> <VALUE> <SIZE>
      //
      // The types that we are interested in are T, D, R, and B.
      //
      auto parse_line = [&syms] (const string& l)
      {
        size_t b (0), e (0), n;

        // NAME
        //
        n = next_word (l, b, e);

        if (n == 0)
          return;

        string s (l, b, n);

        // TYPE
        //
        n = next_word (l, b, e);

        if (n != 1)
          return;

        switch (l[b])
        {
        case 'D': syms.d.insert (move (s)); break;
        case 'R': syms.r.insert (move (s)); break;
        case 'B': syms.b.insert (move (s)); break;
        case 'c':
        case 'C': syms.c.insert (move (s)); break;
        case 'T': syms.t.insert (move (s)); break;
        }
      };

      // Read until we reach EOF on all streams.
      //
      // Note that if dbuf is not opened, then we automatically get an
      // inactive nullfd entry.
      //
      fdselect_set fds {is.fd (), dbuf.is.fd ()};
      fdselect_state& ist (fds[0]);
      fdselect_state& dst (fds[1]);

      for (string l; ist.fd != nullfd || dst.fd != nullfd; )
      {
        if (ist.fd != nullfd && getline_non_blocking (is, l))
        {
          if (eof (is))
            ist.fd = nullfd;
          else
          {
            parse_line (l);
            l.clear ();
          }

          continue;
        }

        ifdselect (fds);

        if (dst.ready)
        {
          if (!dbuf.read ())
            dst.fd = nullfd;
        }
      }
    }

    static void
    write_win32_msvc (ostream& os, const symbols& syms, bool i386)
    {
      // Our goal here is to export the same types of symbols as what gets
      // exported by MSVC with __declspec(dllexport) (can be viewed with
      // dumpbin /EXPORTS).
      //
      // Some special C++ symbol patterns:
      //
      // Data symbols:
      //
      // ??_C* -- string literal                      (R,   not exported)
      // ??_7* -- vtable                              (R,   exported)
      // ??_R* -- rtti, can be prefixed with _CT/__CT (D/R, not exported)
      //
      // Text symbols:
      //
      // ??_G* -- scalar deleting destructor (not exported)
      // ??_E* -- vector deleting destructor (not exported)
      //
      // The following two symbols seem to be related to exception
      // throwing and most likely should not be exported.
      //
      // R _CTA3?AVinvalid_argument@std@@
      // R _TI3?AVinvalid_argument@std@@
      //
      // There are also what appears to be floating point literals:
      //
      // R __real@3f80000
      //
      // For some reason i386 object files have extern "C" symbols (both
      // data and text) prefixed with an underscore which must be stripped
      // in the .def file.
      //
      // Note that the extra prefix seems to be also added to special
      // symbols so something like _CT??... becomes __CT??... on i386.
      // However, for such symbols the underscore shall not be removed.
      // Which means an extern "C" _CT becomes __CT on i383 and hard to
      // distinguish from the special symbols. We deal with this by only
      // stripping the underscore if the symbols doesn't contain any
      // special characters (?@).
      //
      auto extern_c = [] (const string& s)
      {
        return s.find_first_of ("?@") == string::npos;
      };

      auto strip = [i386, &extern_c] (const string& s) -> const char*
      {
        const char* r (s.c_str ());

        if (i386 && s[0] == '_' && extern_c (s))
          r++;

        return r;
      };

      // Code.
      //
      for (const string& s: syms.t)
      {
        auto filter = [&strip] (const string& s) -> const char*
        {
          if (s.compare (0, 4, "??_G") == 0 ||
              s.compare (0, 4, "??_E") == 0)
            return nullptr;

          return strip (s);
        };

        if (const char* v = filter (s))
          os << "  " << v << '\n';
      }

      // Data.
      //
      // Note that it's not easy to import data without a dllimport
      // declaration.
      //
      {
        auto filter = [&strip] (const string& s) -> const char*
        {
          if (s.compare (0, 4, "??_R") == 0 ||
              s.compare (0, 4, "??_C") == 0)
            return nullptr;

          return strip (s);
        };

        for (const string& s: syms.d)
          if (const char* v = filter (s))
            os << "  " << v << " DATA\n";

        for (const string& s: syms.b)
          if (const char* v = filter (s))
            os << "  " << v << " DATA\n";

        // For common symbols, only write extern C.
        //
        for (const string& s: syms.c)
          if (extern_c (s))
            if (const char* v = filter (s))
              os << "  " << v << " DATA\n";

        // Read-only data contains an especially large number of various
        // special symbols. Instead of trying to filter them out case by case,
        // we will try to recognize C/C++ identifiers plus the special symbols
        // that we need to export (e.g., vtable).
        //
        // Note that it looks like rdata should not be declared DATA. It is
        // known to break ??_7 (vtable) exporting (see GH issue 315).
        //
        for (const string& s: syms.r)
        {
          if (extern_c (s)                 || // C
              (s[0] == '?' && s[1] != '?') || // C++
              s.compare (0, 4, "??_7") == 0)  // vtable
          {
            os << "  " << strip (s) << '\n';
          }
        }
      }
    }

    static void
    write_mingw32 (ostream& os, const symbols& syms, bool i386)
    {
      // Our goal here is to export the same types of symbols as what gets
      // exported by GCC with __declspec(dllexport) (can be viewed with
      // dumpbin /EXPORTS).
      //
      // Some special C++ symbol patterns (Itanium C++ ABI):
      //
      // Data symbols:
      //
      // _ZTVN* -- vtable          (R,   exported)
      // _ZTIN* -- typeinfo        (R,   exported)
      // _ZTSN* -- typeinfo name   (R, not exported)
      //
      // There are also some special R symbols which start with .refptr.
      // that are not exported.
      //
      // Normal symbols (both text and data) appear to start with _ZN.
      //
      // Note that we have the same extra underscore for i386 as in the
      // win32-msvc case above but here even for mangled symbols (e.g., __Z*).
      //
      auto skip = [i386] (const string& s) -> size_t
      {
        return i386 && s[0] == '_' ? 1 : 0;
      };

      // Code.
      //
      for (const string& s: syms.t)
      {
        auto filter = [&skip] (const string& s) -> const char*
        {
          return s.c_str () + skip (s);
        };

        if (const char* v = filter (s))
          os << "  " << v << '\n';
      }

      // Data.
      //
      {
        auto filter = [&skip] (const string& s) -> const char*
        {
          return s.c_str () + skip (s);
        };

        for (const string& s: syms.d)
          if (const char* v = filter (s))
            os << "  " << v << " DATA\n";

        for (const string& s: syms.b)
          if (const char* v = filter (s))
            os << "  " << v << " DATA\n";

        for (const string& s: syms.c)
          if (const char* v = filter (s))
            os << "  " << v << " DATA\n";

        // Read-only data contains an especially large number of various
        // special symbols. Instead of trying to filter them out case by case,
        // we will try to recognize C/C++ identifiers plus the special symbols
        // that we need to export (e.g., vtable and typeinfo).
        //
        // For the description of GNU binutils .def format, see:
        //
        // https://sourceware.org/binutils/docs/binutils/def-file-format.html
        //
        // @@ Maybe CONSTANT is more appropriate than DATA?
        //
        for (const string& s: syms.r)
        {
          if (s.find_first_of (".") != string::npos) // Special (.refptr.*)
            continue;

          size_t p (skip (s)), n (s.size () - p);

          if ((n < 2 || s[p] != '_' || s[p + 1] != 'Z') ||  // C
              (s[p + 2] == 'N'                        ) ||  // C++ (normal)
              (s[p + 2] == 'T' && (s[p + 3] == 'V' ||       // vtable
                                   s[p + 3] == 'I') &&      // typeinfo
               s[p + 4] == 'N'))
          {
            os << "  " << s.c_str () + p << " DATA\n";
          }
        }
      }
    }

    bool def_rule::
    match (action a, target& t) const
    {
      tracer trace ("bin::def_rule::match");

      // See if we have an object file or a utility library.
      //
      for (prerequisite_member p: reverse_group_prerequisite_members (a, t))
      {
        // If excluded or ad hoc, then don't factor it into our tests.
        //
        if (include (a, t, p) != include_type::normal)
          continue;

        if (p.is_a<obj> ()   || p.is_a<objs> () ||
            p.is_a<bmi> ()   || p.is_a<bmis> () ||
            p.is_a<libul> () || p.is_a<libus> ())
          return true;
      }

      l4 ([&]{trace << "no object or utility library prerequisite for target "
                    << t;});
      return false;
    }

    recipe def_rule::
    apply (action a, target& xt) const
    {
      def& t (xt.as<def> ());

      t.derive_path ();

      // Inject dependency on the output directory.
      //
      inject_fsdir (a, t);

      // Match prerequisites only picking object files and utility libraries.
      //
      match_prerequisite_members (
        a,
        t,
        [] (action a,
            const target& t,
            const prerequisite_member& p,
            include_type i) -> prerequisite_target
        {
          return
            i == include_type::adhoc ? nullptr :
            //
            // If this is a target group, then pick the appropriate member
            // (the same semantics as what we have in link-rule).
            //
            p.is_a<obj> ()   ? &search (t, objs::static_type, p.key ()) :
            p.is_a<bmi> ()   ? &search (t, bmis::static_type, p.key ()) :
            p.is_a<libul> () ? link_member (p.search (t).as<libul> (),
                                            a,
                                            linfo {otype::s, lorder::s}) :
            p.is_a<objs> () ||
            p.is_a<bmis> () ||
            p.is_a<libus> () ? &p.search (t) : nullptr;
        });

      switch (a)
      {
      case perform_update_id: return &perform_update;
      case perform_clean_id:  return &perform_clean_depdb; // Standard clean.
      default:                return noop_recipe;          // Configure update.
      }
    }

    target_state def_rule::
    perform_update (action a, const target& xt)
    {
      tracer trace ("bin::def_rule::perform_update");

      const def& t (xt.as<def> ());
      const path& tp (t.path ());

      context& ctx (t.ctx);

      const scope& bs (t.base_scope ());
      const scope& rs (*bs.root_scope ());

      // For link.exe we use its /DUMP option to access dumpbin.exe. Otherwise
      // (lld-link, MinGW), we use nm (llvm-nm, MinGW nm). For good measure
      // (e.g., the bin.def module is loaded without bin.ld), we also handle
      // the direct dumpbin.exe usage.
      //
      const string& lid (cast_empty<string> (rs["bin.ld.id"]));

      // Update prerequisites and determine if anything changed.
      //
      timestamp mt (t.load_mtime ());
      optional<target_state> ts (execute_prerequisites (a, t, mt));

      bool update (!ts);

      // We use depdb to track changes to the input set, etc.
      //
      depdb dd (tp + ".d");

      // First should come the rule name/version.
      //
      if (dd.expect (rule_id_) != nullptr)
        l4 ([&]{trace << "rule mismatch forcing update of " << t;});

      // Then the nm checksum.
      //
      if (dd.expect (lid == "msvc"
                     ? cast<string> (rs["bin.ld.checksum"])
                     : cast<string> (rs["bin.nm.checksum"])) != nullptr)
        l4 ([&]{trace << "linker mismatch forcing update of " << t;});

      // @@ TODO: track in depdb if making symbol filtering configurable.

      // Collect and hash the list of object files seeing through libus{}.
      //
      vector<reference_wrapper<const objs>> os;
      {
        sha256 cs;

        auto collect = [a, &rs, &os, &cs] (const file& t,
                                           const auto& collect) -> void
        {
          for (const target* pt: t.prerequisite_targets[a])
          {
            if (pt == nullptr)
              continue;

            const objs* o;
            if ((o = pt->is_a<objs> ()) != nullptr)
              ;
            else if (pt->is_a<hbmi> ())
              o = find_adhoc_member<objs> (*pt);
            //
            // Note that in prerequisite targets we will have the libux{}
            // members, not the group.
            //
            else if (const libus* l = pt->is_a<libus> ())
            {
              collect (*l, collect);
              continue;
            }
            else
              continue;

            hash_path (cs, o->path (), rs.out_path ());
            os.push_back (*o);
          }
        };

        collect (t, collect);

        if (dd.expect (cs.string ()) != nullptr)
          l4 ([&]{trace << "file set mismatch forcing update of " << t;});
      }

      // Update if any mismatch or depdb is newer that the output.
      //
      if (dd.writing () || dd.mtime > mt)
        update = true;

      dd.close ();

      // If nothing changed, then we are done.
      //
      if (!update)
        return *ts;

      const process_path& nm (lid == "msvc"
                              ? cast<process_path> (rs["bin.ld.path"])
                              : cast<process_path> (rs["bin.nm.path"]));

      cstrings args {nm.recall_string ()};

      string nid;
      if (lid == "msvc")
      {
        args.push_back ("/DUMP"); // Must come first.
        args.push_back ("/NOLOGO");
        args.push_back ("/SYMBOLS");
      }
      else
      {
        nid = cast<string> (rs["bin.nm.id"]);

        if (nid == "msvc")
        {
          args.push_back ("/NOLOGO");
          args.push_back ("/SYMBOLS");
        }
        else
        {
          // Note that llvm-nm's --no-weak is only available since LLVM 7.
          //
          args.push_back ("--extern-only");
          args.push_back ("--format=posix");
        }
      }

      args.push_back (nullptr); // Argument placeholder.
      args.push_back (nullptr);

      const char*& arg (*(args.end () - 2));

      // We could print the prerequisite if it's a single obj{}/libu{} (with
      // the latter being the common case). But it doesn't feel like that's
      // worth the variability and the associated possibility of confusion.
      //
      if (verb == 1)
        print_diag ("def", t);

      // Extract symbols from each object file.
      //
      symbols syms;
      for (const objs& o: os)
      {
        // Use a relative path for nicer diagnostics.
        //
        path rp (relative (o.path ()));
        arg = rp.string ().c_str ();

        if (verb >= 2)
          print_process (args);

        if (ctx.dry_run)
          continue;

        // Both dumpbin.exe and nm send their output to stdout. While nm sends
        // diagnostics to stderr, dumpbin sends it to stdout together with the
        // output. To keep things uniform we will buffer stderr in both cases.
        //
        process pr (
          run_start (nm,
                     args,
                     0                       /* stdin */,
                     -1                      /* stdout */,
                     diag_buffer::pipe (ctx) /* stderr */));

        // Note that while we read both streams until eof in the normal
        // circumstances, we cannot use fdstream_mode::skip for the exception
        // case on both of them: we may end up being blocked trying to read
        // one stream while the process may be blocked writing to the other.
        // So in case of an exception we only skip the diagnostics and close
        // stdout hard. The latter should happen first so the order of the
        // dbuf/is variables is important.
        //
        diag_buffer dbuf (ctx, args[0], pr, (fdstream_mode::non_blocking |
                                             fdstream_mode::skip));

        bool io (false);
        try
        {
          ifdstream is (move (pr.in_ofd),
                        fdstream_mode::non_blocking,
                        ifdstream::badbit);

          if (lid == "msvc" || nid == "msvc")
            read_dumpbin (dbuf, is, syms);
          else
            read_posix_nm (dbuf, is, syms);

          is.close ();
        }
        catch (const io_error&)
        {
          // Presumably the child process failed so let run_finish() deal with
          // that first.
          //
          io = true;
        }

        if (!run_finish_code (dbuf, args, pr, 1 /* verbosity */) || io)
          fail << "unable to extract symbols from " << arg;
      }

#if 0
      for (const string& s: syms.d) text << "D " << s;
      for (const string& s: syms.r) text << "R " << s;
      for (const string& s: syms.b) text << "B " << s;
      for (const string& s: syms.c) text << "C " << s;
      for (const string& s: syms.t) text << "T " << s;
#endif

      if (verb >= 3)
        text << "cat >" << tp;

      if (!ctx.dry_run)
      {
        const auto& tgt (cast<target_triplet> (rs["bin.target"]));

        bool i386 (tgt.cpu.size () == 4 &&
                   tgt.cpu[0] == 'i' && tgt.cpu[2] == '8' && tgt.cpu[3] == '6');

        auto_rmfile rm (tp);
        try
        {
          ofdstream os (tp);

          os << "; Auto-generated, do not edit.\n"
             << "EXPORTS\n";

          if (tgt.system == "mingw32")
            write_mingw32 (os, syms, i386);
          else
            write_win32_msvc (os, syms, i386);

          os.close ();
          rm.cancel ();
        }
        catch (const io_error& e)
        {
          fail << "unable to write to " << tp << ": " << e;
        }

        dd.check_mtime (tp);
      }

      t.mtime (system_clock::now ());
      return target_state::changed;
    }

    const string def_rule::rule_id_ {"bin.def 2"};
  }
}
