// file      : libbuild2/test/script/runner.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/test/script/runner.hxx>

#include <ios> // streamsize

#include <libbutl/regex.mxx>
#include <libbutl/builtin.mxx>
#include <libbutl/fdstream.mxx> // fdopen_mode, fdnull(), fddup()

#include <libbuild2/variable.hxx>
#include <libbuild2/filesystem.hxx>
#include <libbuild2/diagnostics.hxx>

#include <libbuild2/test/common.hxx>

#include <libbuild2/test/script/regex.hxx>
#include <libbuild2/test/script/parser.hxx>
#include <libbuild2/test/script/builtin-options.hxx>

using namespace std;
using namespace butl;

namespace build2
{
  namespace test
  {
    namespace script
    {
      // Normalize a path. Also make the relative path absolute using the
      // scope's working directory unless it is already absolute.
      //
      static path
      normalize (path p, const scope& sp, const location& l)
      {
        path r (p.absolute () ? move (p) : sp.wd_path / move (p));

        try
        {
          r.normalize ();
        }
        catch (const invalid_path& e)
        {
          fail (l) << "invalid file path " << e.path;
        }

        return r;
      }

      // Check if a path is not empty, the referenced file exists and is not
      // empty.
      //
      static bool
      non_empty (const path& p, const location& ll)
      {
        if (p.empty () || !exists (p))
          return false;

        try
        {
          ifdstream is (p);
          return is.peek () != ifdstream::traits_type::eof ();
        }
        catch (const io_error& e)
        {
          // While there can be no fault of the test command being currently
          // executed let's add the location anyway to ease the
          // troubleshooting. And let's stick to that principle down the road.
          //
          fail (ll) << "unable to read " << p << ": " << e << endf;
        }
      }

      // If the file exists, not empty and not larger than 4KB print it to the
      // diag record. The file content goes from the new line and is not
      // indented.
      //
      static void
      print_file (diag_record& d, const path& p, const location& ll)
      {
        if (exists (p))
        {
          try
          {
            ifdstream is (p, ifdstream::badbit);

            if (is.peek () != ifdstream::traits_type::eof ())
            {
              char buf[4096 + 1]; // Extra byte is for terminating '\0'.

              // Note that the string is always '\0'-terminated with a maximum
              // sizeof (buf) - 1 bytes read.
              //
              is.getline (buf, sizeof (buf), '\0');

              // Print if the file fits 4KB-size buffer. Note that if it
              // doesn't the failbit is set.
              //
              if (is.eof ())
              {
                // Suppress the trailing newline character as the diag record
                // adds it's own one when flush.
                //
                streamsize n (is.gcount ());
                assert (n > 0);

                // Note that if the file contains '\0' it will also be counted
                // by gcount(). But even in the worst case we will stay in the
                // buffer boundaries (and so not crash).
                //
                if (buf[n - 1] == '\n')
                  buf[n - 1] = '\0';

                d << '\n' << buf;
              }
            }
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to read " << p << ": " << e;
          }
        }
      }

      // Print first 10 directory sub-entries to the diag record. The directory
      // must exist.
      //
      static void
      print_dir (diag_record& d, const dir_path& p, const location& ll)
      {
        try
        {
          size_t n (0);
          for (const dir_entry& de: dir_iterator (p,
                                                  false /* ignore_dangling */))
          {
            if (n++ < 10)
              d << '\n' << (de.ltype () == entry_type::directory
                            ? path_cast<dir_path> (de.path ())
                            : de.path ());
          }

          if (n > 10)
            d << "\nand " << n - 10 << " more file(s)";
        }
        catch (const system_error& e)
        {
          fail (ll) << "unable to iterate over " << p << ": " << e;
        }
      }

      // Save a string to the file. Fail if exception is thrown by underlying
      // operations.
      //
      static void
      save (const path& p, const string& s, const location& ll)
      {
        try
        {
          ofdstream os (p);
          os << s;
          os.close ();
        }
        catch (const io_error& e)
        {
          fail (ll) << "unable to write " << p << ": " << e;
        }
      }

      // Return the value of the test.target variable.
      //
      static inline const target_triplet&
      test_target (const script& s)
      {
        // @@ Would be nice to use cached value from test::common_data.
        //
        if (auto r = cast_null<target_triplet> (s.test_target["test.target"]))
          return *r;

        // We set it to default value in init() so it can only be NULL if the
        // user resets it.
        //
        fail << "invalid test.target value" << endf;
      }

      // Transform string according to here-* redirect modifiers from the {/}
      // set.
      //
      static string
      transform (const string& s,
                 bool regex,
                 const string& modifiers,
                 const script& scr)
      {
        if (modifiers.find ('/') == string::npos)
          return s;

        // For targets other than Windows leave the string intact.
        //
        if (test_target (scr).class_ != "windows")
          return s;

        // Convert forward slashes to Windows path separators (escape for
        // regex).
        //
        string r;
        for (size_t p (0);;)
        {
          size_t sp (s.find ('/', p));

          if (sp != string::npos)
          {
            r.append (s, p, sp - p);
            r.append (regex ? "\\\\" : "\\");
            p = sp + 1;
          }
          else
          {
            r.append (s, p, sp);
            break;
          }
        }

        return r;
      }

      // Check if the test command output matches the expected result (redirect
      // value). Noop for redirect types other than none, here_*.
      //
      static bool
      check_output (const path& pr,
                    const path& op,
                    const path& ip,
                    const redirect& rd,
                    const location& ll,
                    scope& sp,
                    bool diag,
                    const char* what)
      {
        auto input_info = [&ip, &ll] (diag_record& d)
        {
          if (non_empty (ip, ll))
            d << info << "stdin: " << ip;
        };

        auto output_info = [&what, &ll] (diag_record& d,
                                         const path& p,
                                         const char* prefix = "",
                                         const char* suffix = "")
        {
          if (non_empty (p, ll))
            d << info << prefix << what << suffix << ": " << p;
          else
            d << info << prefix << what << suffix << " is empty";
        };

        if (rd.type == redirect_type::none)
        {
          // Check that there is no output produced.
          //
          assert (!op.empty ());

          if (!non_empty (op, ll))
            return true;

          if (diag)
          {
            diag_record d (error (ll));
            d << pr << " unexpectedly writes to " << what <<
              info << what << ": " << op;

            input_info (d);

            // Print cached output.
            //
            print_file (d, op, ll);
          }

          // Fall through (to return false).
          //
        }
        else if (rd.type == redirect_type::here_str_literal ||
                 rd.type == redirect_type::here_doc_literal ||
                 (rd.type == redirect_type::file &&
                  rd.file.mode == redirect_fmode::compare))
        {
          // The expected output is provided as a file or as a string. Save the
          // string to a file in the later case.
          //
          assert (!op.empty ());

          path eop;

          if (rd.type == redirect_type::file)
            eop = normalize (rd.file.path, sp, ll);
          else
          {
            eop = path (op + ".orig");
            save (eop, transform (rd.str, false, rd.modifiers, sp.root), ll);
            sp.clean_special (eop);
          }

          // Use the diff utility for comparison.
          //
          path dp ("diff");
          process_path pp (run_search (dp, true));

          cstrings args {pp.recall_string (), "-u"};

          // Ignore Windows newline fluff if that's what we are running on.
          //
          if (test_target (sp.root).class_ == "windows")
            args.push_back ("--strip-trailing-cr");

          args.push_back (eop.string ().c_str ());
          args.push_back (op.string ().c_str ());
          args.push_back (nullptr);

          if (verb >= 2)
            print_process (args);

          try
          {
            // Save diff's stdout to a file for troubleshooting and for the
            // optional (if not too large) printing (at the end of
            // diagnostics).
            //
            path ep (op + ".diff");
            auto_fd efd;

            try
            {
              efd = fdopen (ep, fdopen_mode::out | fdopen_mode::create);
              sp.clean_special (ep);
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to write " << ep << ": " << e;
            }

            // Diff utility prints the differences to stdout. But for the
            // user it is a part of the test failure diagnostics so let's
            // redirect stdout to stderr.
            //
            process p (pp, args.data (), 0, 2, efd.get ());
            efd.reset ();

            if (p.wait ())
              return true;

            assert (p.exit);
            const process_exit& pe (*p.exit);

            // Note that both POSIX and GNU diff report error by exiting with
            // the code > 1.
            //
            if (!pe.normal () || pe.code () > 1)
            {
              diag_record d (fail (ll));
              print_process (d, args);
              d << " " << pe;
            }

            // Output doesn't match the expected result.
            //
            if (diag)
            {
              diag_record d (error (ll));
              d << pr << " " << what << " doesn't match expected";

              output_info (d, op);
              output_info (d, eop, "expected ");
              output_info (d, ep, "", " diff");
              input_info  (d);

              print_file (d, ep, ll);
            }

            // Fall through (to return false).
            //
          }
          catch (const process_error& e)
          {
            error (ll) << "unable to execute " << pp << ": " << e;

            if (e.child)
              exit (1);

            throw failed ();
          }
        }
        else if (rd.type == redirect_type::here_str_regex ||
                 rd.type == redirect_type::here_doc_regex)
        {
          // The overall plan is:
          //
          // 1. Create regex line string. While creating it's line characters
          //    transform regex lines according to the redirect modifiers.
          //
          // 2. Create line regex using the line string. If creation fails
          //    then save the (transformed) regex redirect to a file for
          //    troubleshooting.
          //
          // 3. Parse the output into the literal line string.
          //
          // 4. Match the output line string with the line regex.
          //
          // 5. If match fails save the (transformed) regex redirect to a file
          //    for troubleshooting.
          //
          using namespace regex;

          assert (!op.empty ());

          // Create regex line string.
          //
          line_pool pool;
          line_string rls;
          const regex_lines rl (rd.regex);

          // Parse regex flags.
          //
          // When add support for new flags don't forget to update
          // parse_regex().
          //
          auto parse_flags = [] (const string& f) -> char_flags
          {
            char_flags r (char_flags::none);

            for (char c: f)
            {
              switch (c)
              {
              case 'd': r |= char_flags::idot;  break;
              case 'i': r |= char_flags::icase; break;
              default: assert (false); // Error so should have been checked.
              }
            }

            return r;
          };

          // Return original regex line with the transformation applied.
          //
          auto line = [&rl, &rd, &sp] (const regex_line& l) -> string
          {
            string r;
            if (l.regex)                  // Regex (possibly empty),
            {
              r += rl.intro;
              r += transform (l.value, true, rd.modifiers, sp.root);
              r += rl.intro;
              r += l.flags;
            }
            else if (!l.special.empty ()) // Special literal.
              r += rl.intro;
            else                          // Textual literal.
              r += transform (l.value, false, rd.modifiers, sp.root);

            r += l.special;
            return r;
          };

          // Return regex line location.
          //
          // Note that we rely on the fact that the command and regex lines
          // are always belong to the same testscript file.
          //
          auto loc = [&ll] (uint64_t line, uint64_t column) -> location
          {
            location r (ll);
            r.line = line;
            r.column = column;
            return r;
          };

          // Save the regex to file for troubleshooting, return the file path
          // it have been saved to.
          //
          // Note that we save the regex on line regex creation failure or if
          // the program output doesn't match.
          //
          auto save_regex = [&op, &rl, &rd, &ll, &line] () -> path
          {
            path rp (op + ".regex");

            // Encode here-document regex global flags if present as a file
            // name suffix. For example if icase and idot flags are specified
            // the name will look like:
            //
            // test/1/stdout.regex-di
            //
            if (rd.type == redirect_type::here_doc_regex && !rl.flags.empty ())
              rp += '-' + rl.flags;

            // Note that if would be more efficient to directly write chunks
            // to file rather than to compose a string first. Hower we don't
            // bother (about performance) for the sake of the code as we
            // already failed.
            //
            string s;
            for (auto b (rl.lines.cbegin ()), i (b), e (rl.lines.cend ());
                 i != e; ++i)
            {
              if (i != b) s += '\n';
              s += line (*i);
            }

            save (rp, s, ll);
            return rp;
          };

          // Finally create regex line string.
          //
          // Note that diagnostics doesn't refer to the program path as it is
          // irrelevant to failures at this stage.
          //
          char_flags gf (parse_flags (rl.flags)); // Regex global flags.

          for (const auto& l: rl.lines)
          {
            if (l.regex) // Regex (with optional special characters).
            {
              line_char c;

              // Empty regex is a special case repesenting the blank line.
              //
              if (l.value.empty ())
                c = line_char ("", pool);
              else
              {
                try
                {
                  string s (transform (l.value, true, rd.modifiers, sp.root));

                  c = line_char (
                    char_regex (s, gf | parse_flags (l.flags)), pool);
                }
                catch (const regex_error& e)
                {
                  // Print regex_error description if meaningful.
                  //
                  diag_record d (fail (loc (l.line, l.column)));

                  if (rd.type == redirect_type::here_str_regex)
                    d << "invalid " << what << " regex redirect" << e <<
                      info << "regex: '" << line (l) << "'";
                  else
                    d << "invalid char-regex in " << what << " regex redirect"
                      << e <<
                      info << "regex line: '" << line (l) << "'";

                  d << endf;
                }
              }

              rls += c; // Append blank literal or regex line char.
            }
            else if (!l.special.empty ()) // Special literal.
            {
              // Literal can not be followed by special characters in the same
              // line.
              //
              assert (l.value.empty ());
            }
            else // Textual literal.
            {
              // Append literal line char.
              //
              rls += line_char (
                transform (l.value, false, rd.modifiers, sp.root), pool);
            }

            for (char c: l.special)
            {
              if (line_char::syntax (c))
                rls += line_char (c); // Append special line char.
              else
                fail (loc (l.line, l.column))
                  << "invalid syntax character '" << c << "' in " << what
                  << " regex redirect" <<
                  info << "regex line: '" << line (l) << "'";
            }
          }

          // Create line regex.
          //
          line_regex regex;

          try
          {
            regex = line_regex (move (rls), move (pool));
          }
          catch (const regex_error& e)
          {
            // Note that line regex creation can not fail for here-string
            // redirect as it doesn't have syntax line chars. That in
            // particular means that end_line and end_column are meaningful.
            //
            assert (rd.type == redirect_type::here_doc_regex);

            diag_record d (fail (loc (rd.end_line, rd.end_column)));

            // Print regex_error description if meaningful.
            //
            d << "invalid " << what << " regex redirect" << e;

            output_info (d, save_regex (), "", " regex");
          }

          // Parse the output into the literal line string.
          //
          line_string ls;

          try
          {
            // Do not throw when eofbit is set (end of stream reached), and
            // when failbit is set (getline() failed to extract any character).
            //
            // Note that newlines are treated as line-chars separators. That
            // in particular means that the trailing newline produces a blank
            // line-char (empty literal). Empty output produces the zero-length
            // line-string.
            //
            // Also note that we strip the trailing CR characters (otherwise
            // can mismatch when cross-test).
            //
            ifdstream is (op, ifdstream::badbit);
            is.peek (); // Sets eofbit for an empty stream.

            while (!is.eof ())
            {
              string s;
              getline (is, s);

              // It is safer to strip CRs in cycle, as msvcrt unexplainably
              // adds too much trailing junk to the system_error descriptions,
              // and so it can appear in programs output. For example:
              //
              // ...: Invalid data.\r\r\n
              //
              // Note that our custom operator<<(ostream&, const exception&)
              // removes this junk.
              //
              while (!s.empty () && s.back () == '\r')
                s.pop_back ();

              ls += line_char (move (s), regex.pool);
            }
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to read " << op << ": " << e;
          }

          // Match the output with the regex.
          //
          if (regex_match (ls, regex)) // Doesn't throw.
            return true;

          // Output doesn't match the regex. We save the regex to file for
          // troubleshooting regardless of whether we print the diagnostics or
          // not.
          //
          path rp (save_regex ());

          if (diag)
          {
            diag_record d (error (ll));
            d << pr << " " << what << " doesn't match regex";

            output_info (d, op);
            output_info (d, rp, "", " regex");
            input_info  (d);

            // Print cached output.
            //
            print_file (d, op, ll);
          }

          // Fall through (to return false).
          //
        }
        else // Noop.
          return true;

        return false;
      }

      bool default_runner::
      test (scope& s) const
      {
        return common_.test (s.root.test_target, s.id_path);
      }

      void default_runner::
      enter (scope& sp, const location&)
      {
        context& ctx (sp.root.target_scope.ctx);

        auto df = make_diag_frame (
          [&sp](const diag_record& dr)
          {
            // Let's not depend on how the path representation can be improved
            // for readability on printing.
            //
            dr << info << "test id: " << sp.id_path.posix_string ();
          });

        // Scope working directory shall be empty (the script working
        // directory is cleaned up by the test rule prior the script
        // execution).
        //
        // Create the root working directory containing the .buildignore file
        // to make sure that it is ignored by name patterns (see buildignore
        // description for details).
        //
        // @@ Shouldn't we add an optional location parameter to mkdir() and
        // alike utility functions so the failure message can contain
        // location info?
        //
        fs_status<mkdir_status> r (
          sp.parent == nullptr
          ? mkdir_buildignore (
            ctx,
            sp.wd_path,
            sp.root.target_scope.root_scope ()->root_extra->buildignore_file,
            2)
          : mkdir (sp.wd_path, 2));

        if (r == mkdir_status::already_exists)
          fail << "working directory " << sp.wd_path << " already exists" <<
            info << "are tests stomping on each other's feet?";

        // We don't change the current directory here but indicate that the
        // scope test commands will be executed in that directory.
        //
        if (verb >= 2)
          text << "cd " << sp.wd_path;

        sp.clean ({cleanup_type::always, sp.wd_path}, true);
      }

      void default_runner::
      leave (scope& sp, const location& ll)
      {
        context& ctx (sp.root.target_scope.ctx);

        auto df = make_diag_frame (
          [&sp](const diag_record& dr)
          {
            // Let's not depend on how the path representation can be improved
            // for readability on printing.
            //
            dr << info << "test id: " << sp.id_path.posix_string ();
          });

        // Perform registered cleanups if requested.
        //
        if (common_.after == output_after::clean)
        {
          // Note that we operate with normalized paths here.
          //
          // Remove special files. The order is not important as we don't
          // expect directories here.
          //
          for (const auto& p: sp.special_cleanups)
          {
            // Remove the file if exists. Fail otherwise.
            //
            if (rmfile (ctx, p, 3) == rmfile_status::not_exist)
              fail (ll) << "registered for cleanup special file " << p
                        << " does not exist";
          }

          // Remove files and directories in the order opposite to the order of
          // cleanup registration.
          //
          for (const auto& c: reverse_iterate (sp.cleanups))
          {
            cleanup_type t (c.type);

            // Skip whenever the path exists or not.
            //
            if (t == cleanup_type::never)
              continue;

            const path& cp (c.path);

            // Wildcard with the last component being '***' (without trailing
            // separator) matches all files and sub-directories recursively as
            // well as the start directories itself. So we will recursively
            // remove the directories that match the parent (for the original
            // path) directory wildcard.
            //
            bool recursive (cp.leaf ().representation () == "***");
            const path& p (!recursive ? cp : cp.directory ());

            // Remove files or directories using wildcard.
            //
            if (p.string ().find_first_of ("?*") != string::npos)
            {
              bool removed (false);

              auto rm = [&cp, recursive, &removed, &sp, &ll, &ctx]
                (path&& pe, const string&, bool interm)
              {
                if (!interm)
                {
                  // While removing the entry we can get not_exist due to
                  // racing conditions, but that's ok if somebody did our job.
                  // Note that we still set the removed flag to true in this
                  // case.
                  //
                  removed = true; // Will be meaningless on failure.

                  if (pe.to_directory ())
                  {
                    dir_path d (path_cast<dir_path> (pe));

                    if (!recursive)
                    {
                      rmdir_status r (rmdir (ctx, d, 3));

                      if (r != rmdir_status::not_empty)
                        return true;

                      diag_record dr (fail (ll));
                      dr << "registered for cleanup directory " << d
                         << " is not empty";

                      print_dir (dr, d, ll);
                      dr << info << "wildcard: '" << cp << "'";
                    }
                    else
                    {
                      // Don't remove the working directory (it will be removed
                      // by the dedicated cleanup).
                      //
                      // Cast to uint16_t to avoid ambiguity with
                      // libbutl::rmdir_r().
                      //
                      rmdir_status r (rmdir_r (ctx, d, d != sp.wd_path, 3));

                      if (r != rmdir_status::not_empty)
                        return true;

                      // The directory is unlikely to be current but let's keep
                      // for completeness.
                      //
                      fail (ll) << "registered for cleanup wildcard " << cp
                                << " matches the current directory";
                    }
                  }
                  else
                    rmfile (ctx, pe, 3);
                }

                return true;
              };

              // Note that here we rely on the fact that recursive iterating
              // goes depth-first (which make sense for the cleanup).
              //
              try
              {
                // Doesn't follow symlinks.
                //
                path_search (p,
                             rm,
                             dir_path () /* start */,
                             path_match_flags::none);
              }
              catch (const system_error& e)
              {
                fail (ll) << "unable to cleanup wildcard " << cp << ": " << e;
              }

              // Removal of no filesystem entries is not an error for 'maybe'
              // cleanup type.
              //
              if (removed || t == cleanup_type::maybe)
                continue;

              fail (ll) << "registered for cleanup wildcard " << cp
                        << " doesn't match any "
                        << (recursive
                            ? "path"
                            : p.to_directory ()
                              ? "directory"
                              : "file");
            }

            // Remove the directory if exists and empty. Fail otherwise.
            // Removal of non-existing directory is not an error for 'maybe'
            // cleanup type.
            //
            if (p.to_directory ())
            {
              dir_path d (path_cast<dir_path> (p));
              bool wd (d == sp.wd_path);

              // Trace the scope working directory removal with the verbosity
              // level 2 (that was used for its creation). For other
              // directories use level 3 (as for other cleanups).
              //
              int v (wd ? 2 : 3);

              // Don't remove the working directory for the recursive cleanup
              // (it will be removed by the dedicated one).
              //
              // Note that the root working directory contains the
              // .buildignore file (see above).
              //
              // @@ If 'd' is a file then will fail with a diagnostics having
              //    no location info. Probably need to add an optional location
              //    parameter to rmdir() function. The same problem exists for
              //    a file cleanup when try to rmfile() directory instead of
              //    file.
              //
              rmdir_status r (
                recursive
                ? rmdir_r (ctx, d, !wd, static_cast <uint16_t> (v))
                : (wd && sp.parent == nullptr
                   ? rmdir_buildignore (
                       ctx,
                       d,
                       sp.root.target_scope.root_scope ()->root_extra->
                         buildignore_file,
                       v)
                   : rmdir (ctx, d, v)));

              if (r == rmdir_status::success ||
                  (r == rmdir_status::not_exist && t == cleanup_type::maybe))
                continue;

              diag_record dr (fail (ll));
              dr << "registered for cleanup directory " << d
                 << (r == rmdir_status::not_exist
                     ? " does not exist"
                     : !recursive
                       ? " is not empty"
                       : " is current");

              if (r == rmdir_status::not_empty)
                print_dir (dr, d, ll);
            }

            // Remove the file if exists. Fail otherwise. Removal of
            // non-existing file is not an error for 'maybe' cleanup type.
            //
            if (rmfile (ctx, p, 3) == rmfile_status::not_exist &&
                t == cleanup_type::always)
              fail (ll) << "registered for cleanup file " << p
                        << " does not exist";
          }
        }

        // Return to the parent scope directory or to the out_base one for the
        // script scope.
        //
        if (verb >= 2)
          text << "cd " << (sp.parent != nullptr
                            ? sp.parent->wd_path
                            : sp.wd_path.directory ());
      }

      // The exit pseudo-builtin: exit the current scope successfully, or
      // print the diagnostics and exit the current scope and all the outer
      // scopes unsuccessfully. Always throw exit_scope exception.
      //
      // exit [<diagnostics>]
      //
      [[noreturn]] static void
      exit_builtin (const strings& args, const location& ll)
      {
        auto i (args.begin ());
        auto e (args.end ());

        // Process arguments.
        //
        // If no argument is specified, then exit successfully. Otherwise,
        // print the diagnostics and exit unsuccessfully.
        //
        if (i == e)
          throw exit_scope (true);

        const string& s (*i++);

        if (i != e)
          fail (ll) << "unexpected argument '" << *i << "'";

        error (ll) << s;
        throw exit_scope (false);
      }

      // The set pseudo-builtin: set variable from the stdin input.
      //
      // set [-e|--exact] [(-n|--newline)|(-w|--whitespace)] [<attr>] <var>
      //
      static void
      set_builtin (scope& sp,
                   const strings& args,
                   auto_fd in,
                   const location& ll)
      {
        try
        {
          // Do not throw when eofbit is set (end of stream reached), and
          // when failbit is set (read operation failed to extract any
          // character).
          //
          ifdstream cin  (move (in), ifdstream::badbit);

          // Parse arguments.
          //
          cli::vector_scanner scan (args);
          set_options ops (scan);

          if (ops.whitespace () && ops.newline ())
            fail (ll) << "both -n|--newline and -w|--whitespace specified";

          if (!scan.more ())
            fail (ll) << "missing variable name";

          string a (scan.next ()); // Either attributes or variable name.
          const string* ats (!scan.more () ? nullptr : &a);
          const string& vname (!scan.more () ? a : scan.next ());

          if (scan.more ())
            fail (ll) << "unexpected argument '" << scan.next () << "'";

          if (ats != nullptr && ats->empty ())
            fail (ll) << "empty variable attributes";

          if (vname.empty ())
            fail (ll) << "empty variable name";

          // Read the input.
          //
          cin.peek (); // Sets eofbit for an empty stream.

          names ns;
          while (!cin.eof ())
          {
            // Read next element that depends on the whitespace mode being
            // enabled or not. For the later case it also make sense to strip
            // the trailing CRs that can appear while cross-testing Windows
            // target or as a part of msvcrt junk production (see above).
            //
            string s;
            if (ops.whitespace ())
              cin >> s;
            else
            {
              getline (cin, s);

              while (!s.empty () && s.back () == '\r')
                s.pop_back ();
            }

            // If failbit is set then we read nothing into the string as eof is
            // reached. That in particular means that the stream has trailing
            // whitespaces (possibly including newlines) if the whitespace mode
            // is enabled, or the trailing newline otherwise. If so then
            // we append the "blank" to the variable value in the exact mode
            // prior to bailing out.
            //
            if (cin.fail ())
            {
              if (ops.exact ())
              {
                if (ops.whitespace () || ops.newline ())
                  ns.emplace_back (move (s)); // Reuse empty string.
                else if (ns.empty ())
                  ns.emplace_back ("\n");
                else
                  ns[0].value += '\n';
              }

              break;
            }

            if (ops.whitespace () || ops.newline () || ns.empty ())
              ns.emplace_back (move (s));
            else
            {
              ns[0].value += '\n';
              ns[0].value += s;
            }
          }

          cin.close ();

          // Set the variable value and attributes. Note that we need to aquire
          // unique lock before potentially changing the script's variable
          // pool. The obtained variable reference can safelly be used with no
          // locking as the variable pool is an associative container
          // (underneath) and we are only adding new variables into it.
          //
          ulock ul (sp.root.var_pool_mutex);
          const variable& var (sp.root.var_pool.insert (move (vname)));
          ul.unlock ();

          value& lhs (sp.assign (var));

          // If there are no attributes specified then the variable assignment
          // is straightforward. Otherwise we will use the build2 parser helper
          // function.
          //
          if (ats == nullptr)
            lhs.assign (move (ns), &var);
          else
          {
            // If there is an error in the attributes string, our diagnostics
            // will look like this:
            //
            // <attributes>:1:1 error: unknown value attribute x
            //   testscript:10:1 info: while parsing attributes '[x]'
            //
            auto df = make_diag_frame (
              [ats, &ll](const diag_record& dr)
              {
                dr << info (ll) << "while parsing attributes '" << *ats << "'";
              });

            parser p (sp.root.test_target.ctx);
            p.apply_value_attributes (&var,
                                      lhs,
                                      value (move (ns)),
                                      *ats,
                                      token_type::assign,
                                      path ("<attributes>"));
          }
        }
        catch (const io_error& e)
        {
          fail (ll) << "set: " << e;
        }
        catch (const cli::exception& e)
        {
          fail (ll) << "set: " << e;
        }
      }

      // Sorted array of builtins that support filesystem entries cleanup.
      //
      static const char* cleanup_builtins[] = {
        "cp", "ln", "mkdir", "mv", "touch"};

      static inline bool
      cleanup_builtin (const string& name)
      {
        return binary_search (
          cleanup_builtins,
          cleanup_builtins +
          sizeof (cleanup_builtins) / sizeof (*cleanup_builtins),
          name);
      }

      static bool
      run_pipe (scope& sp,
                command_pipe::const_iterator bc,
                command_pipe::const_iterator ec,
                auto_fd ifd,
                size_t ci, size_t li, const location& ll,
                bool diag)
      {
        if (bc == ec) // End of the pipeline.
          return true;

        // The overall plan is to run the first command in the pipe, reading
        // its input from the file descriptor passed (or, for the first
        // command, according to stdin redirect specification) and redirecting
        // its output to the right-hand part of the pipe recursively. Fail if
        // the right-hand part fails. Otherwise check the process exit code,
        // match stderr (and stdout for the last command in the pipe) according
        // to redirect specification(s) and fail if any of the above fails.
        //
        const command& c (*bc);

        // Register the command explicit cleanups. Verify that the path being
        // cleaned up is a sub-path of the testscript working directory. Fail
        // if this is not the case.
        //
        for (const auto& cl: c.cleanups)
        {
          const path& p (cl.path);
          path np (normalize (p, sp, ll));

          const string& ls (np.leaf ().string ());
          bool wc (ls == "*" || ls == "**" || ls == "***");
          const path& cp (wc ? np.directory () : np);
          const dir_path& wd (sp.root.wd_path);

          if (!cp.sub (wd))
            fail (ll) << (wc
                          ? "wildcard"
                          : p.to_directory ()
                            ? "directory"
                            : "file")
                      << " cleanup " << p << " is out of working directory "
                      << wd;

          sp.clean ({cl.type, move (np)}, false);
        }

        const redirect& in  (c.in.effective ());
        const redirect& out (c.out.effective ());
        const redirect& err (c.err.effective ());
        bool eq (c.exit.comparison == exit_comparison::eq);

        // If stdin file descriptor is not open then this is the first pipeline
        // command.
        //
        bool first (ifd.get () == -1);

        command_pipe::const_iterator nc (bc + 1);
        bool last (nc == ec);

        const string& program (c.program.string ());

        // Prior to opening file descriptors for command input/output
        // redirects let's check if the command is the exit builtin. Being a
        // builtin syntactically it differs from the regular ones in a number
        // of ways. It doesn't communicate with standard streams, so
        // redirecting them is meaningless. It may appear only as a single
        // command in a pipeline. It doesn't return any value and stops the
        // scope execution, so checking its exit status is meaningless as
        // well. That all means we can short-circuit here calling the builtin
        // and bailing out right after that. Checking that the user didn't
        // specify any redirects or exit code check sounds like a right thing
        // to do.
        //
        if (program == "exit")
        {
          // In case the builtin is erroneously pipelined from the other
          // command, we will close stdin gracefully (reading out the stream
          // content), to make sure that the command doesn't print any
          // unwanted diagnostics about IO operation failure.
          //
          // Note that dtor will ignore any errors (which is what we want).
          //
          ifdstream is (move (ifd), fdstream_mode::skip);

          if (!first || !last)
            fail (ll) << "exit builtin must be the only pipe command";

          if (in.type != redirect_type::none)
            fail (ll) << "exit builtin stdin cannot be redirected";

          if (out.type != redirect_type::none)
            fail (ll) << "exit builtin stdout cannot be redirected";

          if (err.type != redirect_type::none)
            fail (ll) << "exit builtin stderr cannot be redirected";

          // We can't make sure that there is no exit code check. Let's, at
          // least, check that non-zero code is not expected.
          //
          if (eq != (c.exit.code == 0))
            fail (ll) << "exit builtin exit code cannot be non-zero";

          exit_builtin (c.arguments, ll); // Throws exit_scope exception.
        }

        // Create a unique path for a command standard stream cache file.
        //
        auto std_path = [&sp, &ci, &li, &ll] (const char* n) -> path
        {
          path p (n);

          // 0 if belongs to a single-line test scope, otherwise is the
          // command line number (start from one) in the test scope.
          //
          if (li > 0)
            p += "-" + to_string (li);

          // 0 if belongs to a single-command expression, otherwise is the
          // command number (start from one) in the expression.
          //
          // Note that the name like stdin-N can relate to N-th command of a
          // single-line test or to N-th single-command line of multi-line
          // test. These cases are mutually exclusive and so are unambiguous.
          //
          if (ci > 0)
            p += "-" + to_string (ci);

          return normalize (move (p), sp, ll);
        };

        // If this is the first pipeline command, then open stdin descriptor
        // according to the redirect specified.
        //
        path isp;

        if (!first)
          assert (in.type == redirect_type::none); // No redirect expected.
        else
        {
          // Open a file for passing to the command stdin.
          //
          auto open_stdin = [&isp, &ifd, &ll] ()
          {
            assert (!isp.empty ());

            try
            {
              ifd = fdopen (isp, fdopen_mode::in);
            }
            catch (const io_error& e)
            {
              fail (ll) << "unable to read " << isp << ": " << e;
            }
          };

          switch (in.type)
          {
          case redirect_type::pass:
            {
              try
              {
                ifd = fddup (0);
              }
              catch (const io_error& e)
              {
                fail (ll) << "unable to duplicate stdin: " << e;
              }

              break;
            }

          case redirect_type::none:
            // Somehow need to make sure that the child process doesn't read
            // from stdin. That is tricky to do in a portable way. Here we
            // suppose that the program which (erroneously) tries to read some
            // data from stdin being redirected to /dev/null fails not being
            // able to read the expected data, and so the test doesn't pass
            // through.
            //
            // @@ Obviously doesn't cover the case when the process reads
            //    whatever available.
            // @@ Another approach could be not to redirect stdin and let the
            //    process to hang which can be interpreted as a test failure.
            // @@ Both ways are quite ugly. Is there some better way to do
            //    this?
            //
            // Fall through.
            //
          case redirect_type::null:
            {
              try
              {
                ifd = fdnull ();
              }
              catch (const io_error& e)
              {
                fail (ll) << "unable to write to null device: " << e;
              }

              break;
            }

          case redirect_type::file:
            {
              isp = normalize (in.file.path, sp, ll);

              open_stdin ();
              break;
            }

          case redirect_type::here_str_literal:
          case redirect_type::here_doc_literal:
            {
              // We could write to the command stdin directly but instead will
              // cache the data for potential troubleshooting.
              //
              isp = std_path ("stdin");

              save (
                isp, transform (in.str, false, in.modifiers, sp.root), ll);

              sp.clean_special (isp);

              open_stdin ();
              break;
            }

          case redirect_type::trace:
          case redirect_type::merge:
          case redirect_type::here_str_regex:
          case redirect_type::here_doc_regex:
          case redirect_type::here_doc_ref:   assert (false); break;
          }
        }

        assert (ifd.get () != -1);

        // Prior to opening file descriptors for command outputs redirects
        // let's check if the command is the set builtin. Being a builtin
        // syntactically it differs from the regular ones in a number of ways.
        // It either succeeds or terminates abnormally, so redirecting stderr
        // is meaningless. It also never produces any output and may appear
        // only as a terminal command in a pipeline. That means we can
        // short-circuit here calling the builtin and returning right after
        // that. Checking that the user didn't specify any meaningless
        // redirects or exit code check sounds as a right thing to do.
        //
        if (program == "set")
        {
          if (!last)
            fail (ll) << "set builtin must be the last pipe command";

          if (out.type != redirect_type::none)
            fail (ll) << "set builtin stdout cannot be redirected";

          if (err.type != redirect_type::none)
            fail (ll) << "set builtin stderr cannot be redirected";

          if (eq != (c.exit.code == 0))
            fail (ll) << "set builtin exit code cannot be non-zero";

          set_builtin (sp, c.arguments, move (ifd), ll);
          return true;
        }

        // Open a file for command output redirect if requested explicitly
        // (file overwrite/append redirects) or for the purpose of the output
        // validation (none, here_*, file comparison redirects), register the
        // file for cleanup, return the file descriptor. Interpret trace
        // redirect according to the verbosity level (as null if below 2, as
        // pass otherwise). Return nullfd, standard stream descriptor duplicate
        // or null-device descriptor for merge, pass or null redirects
        // respectively (not opening any file).
        //
        auto open = [&sp, &ll, &std_path] (const redirect& r,
                                           int dfd,
                                           path& p) -> auto_fd
        {
          assert (dfd == 1 || dfd == 2);
          const char* what (dfd == 1 ? "stdout" : "stderr");

          fdopen_mode m (fdopen_mode::out | fdopen_mode::create);

          auto_fd fd;
          redirect_type rt (r.type != redirect_type::trace
                            ? r.type
                            : verb < 2
                              ? redirect_type::null
                              : redirect_type::pass);
          switch (rt)
          {
          case redirect_type::pass:
            {
              try
              {
                fd = fddup (dfd);
              }
              catch (const io_error& e)
              {
                fail (ll) << "unable to duplicate " << what << ": " << e;
              }

              return fd;
            }

          case redirect_type::null:
            {
              try
              {
                fd = fdnull ();
              }
              catch (const io_error& e)
              {
                fail (ll) << "unable to write to null device: " << e;
              }

              return fd;
            }

          case redirect_type::merge:
            {
              // Duplicate the paired file descriptor later.
              //
              return fd; // nullfd
            }

          case redirect_type::file:
            {
              // For the cmp mode the user-provided path refers a content to
              // match against, rather than a content to be produced (as for
              // overwrite and append modes). And so for cmp mode we redirect
              // the process output to a temporary file.
              //
              p = r.file.mode == redirect_fmode::compare
                ? std_path (what)
                : normalize (r.file.path, sp, ll);

              m |= r.file.mode == redirect_fmode::append
                ? fdopen_mode::at_end
                : fdopen_mode::truncate;

              break;
            }

          case redirect_type::none:
          case redirect_type::here_str_literal:
          case redirect_type::here_doc_literal:
          case redirect_type::here_str_regex:
          case redirect_type::here_doc_regex:
            {
              p = std_path (what);
              m |= fdopen_mode::truncate;
              break;
            }

          case redirect_type::trace:
          case redirect_type::here_doc_ref: assert (false); break;
          }

          try
          {
            fd = fdopen (p, m);

            if ((m & fdopen_mode::at_end) != fdopen_mode::at_end)
            {
              if (rt == redirect_type::file)
                sp.clean ({cleanup_type::always, p}, true);
              else
                sp.clean_special (p);
            }
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to write " << p << ": " << e;
          }

          return fd;
        };

        path osp;
        fdpipe ofd;

        // If this is the last command in the pipeline than redirect the
        // command process stdout to a file. Otherwise create a pipe and
        // redirect the stdout to the write-end of the pipe. The read-end will
        // be passed as stdin for the next command in the pipeline.
        //
        // @@ Shouldn't we allow the here-* and file output redirects for a
        //    command with pipelined output? Say if such redirect is present
        //    then the process output is redirected to a file first (as it is
        //    when no output pipelined), and only after the process exit code
        //    and the output are validated the next command in the pipeline is
        //    executed taking the file as an input. This could be usefull for
        //    test failures investigation and for tests "tightening".
        //
        if (last)
          ofd.out = open (out, 1, osp);
        else
        {
          assert (out.type == redirect_type::none); // No redirect expected.

          try
          {
            ofd = fdopen_pipe ();
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to open pipe: " << e;
          }
        }

        path esp;
        auto_fd efd (open (err, 2, esp));

        // Merge standard streams.
        //
        bool mo (out.type == redirect_type::merge);
        if (mo || err.type == redirect_type::merge)
        {
          auto_fd& self  (mo ? ofd.out : efd);
          auto_fd& other (mo ? efd : ofd.out);

          try
          {
            assert (self.get () == -1 && other.get () != -1);
            self = fddup (other.get ());
          }
          catch (const io_error& e)
          {
            fail (ll) << "unable to duplicate " << (mo ? "stderr" : "stdout")
                      << ": " << e;
          }
        }

        // All descriptors should be open to the date.
        //
        assert (ofd.out.get () != -1 && efd.get () != -1);

        optional<process_exit> exit;
        builtin_function* bf (builtins.find (program));

        bool success;

        auto process_args = [&c] () -> cstrings
        {
          cstrings args {c.program.string ().c_str ()};

          for (const auto& a: c.arguments)
            args.push_back (a.c_str ());

          args.push_back (nullptr);
          return args;
        };

        if (bf != nullptr)
        {
          // Execute the builtin.
          //
          if (verb >= 2)
            print_process (process_args ());

          // Some of the testscript builtins (cp, mkdir, etc) extend libbutl
          // builtins (via callbacks) registering/moving cleanups for the
          // filesystem entries they create/move, unless explicitly requested
          // not to do so via the --no-cleanup option.
          //
          // Let's "wrap up" the cleanup-related flags into the single object
          // to rely on "small function object" optimization.
          //
          struct cleanup
          {
            // Whether the cleanups are enabled for the builtin. Can be set to
            // false by the parse_option callback if --no-cleanup is
            // encountered.
            //
            bool enabled = true;

            // Whether to register cleanup for a filesystem entry being
            // created/updated depending on its existence. Calculated by the
            // create pre-hook and used by the subsequent post-hook.
            //
            bool add;

            // Whether to move existing cleanups for the filesystem entry
            // being moved, rather than to erase them. Calculated by the move
            // pre-hook and used by the subsequent post-hook.
            //
            bool move;
          };

          // nullopt if the builtin doesn't support cleanups.
          //
          optional<cleanup> cln;

          if (cleanup_builtin (program))
            cln = cleanup ();

          builtin_callbacks bcs {

            // create
            //
            // Unless cleanups are suppressed, test that the filesystem entry
            // doesn't exist (pre-hook) and, if that's the case, register the
            // cleanup for the newly created filesystem entry (post-hook).
            //
            [&sp, &cln] (const path& p, bool pre)
            {
              // Cleanups must be supported by a filesystem entry-creating
              // builtin.
              //
              assert (cln);

              if (cln->enabled)
              {
                if (pre)
                  cln->add = !butl::entry_exists (p);
                else if (cln->add)
                  sp.clean ({cleanup_type::always, p}, true /* implicit */);
              }
            },

            // move
            //
            // Validate the source and destination paths (pre-hook) and,
            // unless suppressed, adjust the cleanups that are sub-paths of
            // the source path (post-hook).
            //
            [&sp, &cln]
            (const path& from, const path& to, bool force, bool pre)
            {
              // Cleanups must be supported by a filesystem entry-moving
              // builtin.
              //
              assert (cln);

              if (pre)
              {
                const dir_path&  wd (sp.wd_path);
                const dir_path& rwd (sp.root.wd_path);

                auto fail = [] (const string& d) {throw runtime_error (d);};

                if (!from.sub (rwd) && !force)
                  fail ("'" + from.representation () +
                        "' is out of working directory '" + rwd.string () +
                        "'");

                auto check_wd = [&wd, fail] (const path& p)
                {
                  if (wd.sub (path_cast<dir_path> (p)))
                    fail ("'" + p.string () +
                          "' contains test working directory '" +
                          wd.string () + "'");
                };

                check_wd (from);
                check_wd (to);

                // Unless cleanups are disabled, "move" the matching cleanups
                // if the destination path doesn't exist and it is a sub-path
                // of the working directory and just remove them otherwise.
                //
                if (cln->enabled)
                  cln->move = !butl::entry_exists (to) && to.sub (rwd);
              }
              else if (cln->enabled)
              {
                // Move or remove the matching cleanups (see above).
                //
                // Note that it's not enough to just change the cleanup paths.
                // We also need to make sure that these cleanups happen before
                // the destination directory (or any of its parents) cleanup,
                // that is potentially registered. To achieve that we can just
                // relocate these cleanup entries to the end of the list,
                // preserving their mutual order. Remember that cleanups in
                // the list are executed in the reversed order.
                //
                cleanups cs;

                // Remove the source path sub-path cleanups from the list,
                // adjusting/caching them if required (see above).
                //
                for (auto i (sp.cleanups.begin ()); i != sp.cleanups.end (); )
                {
                  build2::test::script::cleanup& c (*i);
                  path& p (c.path);

                  if (p.sub (from))
                  {
                    if (cln->move)
                    {
                      // Note that we need to preserve the cleanup path
                      // trailing separator which indicates the removal
                      // method. Also note that leaf(), in particular, does
                      // that.
                      //
                      p = p != from
                        ? to / p.leaf (path_cast<dir_path> (from))
                        : p.to_directory ()
                        ? path_cast<dir_path> (to)
                        : to;

                      cs.push_back (move (c));
                    }

                    i = sp.cleanups.erase (i);
                  }
                  else
                    ++i;
                }

                // Re-insert the adjusted cleanups at the end of the list.
                //
                sp.cleanups.insert (sp.cleanups.end (),
                                    make_move_iterator (cs.begin ()),
                                    make_move_iterator (cs.end ()));

              }
            },

            // remove
            //
            // Validate the filesystem entry path (pre-hook).
            //
            [&sp] (const path& p, bool force, bool pre)
            {
              if (pre)
              {
                const dir_path&  wd (sp.wd_path);
                const dir_path& rwd (sp.root.wd_path);

                auto fail = [] (const string& d) {throw runtime_error (d);};

                if (!p.sub (rwd) && !force)
                  fail ("'" + p.representation () +
                        "' is out of working directory '" + rwd.string () +
                        "'");

                if (wd.sub (path_cast<dir_path> (p)))
                  fail ("'" + p.string () +
                        "' contains test working directory '" + wd.string () +
                        "'");
              }
            },

            // parse_option
            //
            [&cln] (const strings& args, size_t i)
            {
              // Parse --no-cleanup, if it is supported by the builtin.
              //
              if (cln && args[i] == "--no-cleanup")
              {
                cln->enabled = false;
                return 1;
              }

              return 0;
            },

            // sleep
            //
            // Deactivate the thread before going to sleep.
            //
            [&sp] (const duration& d)
            {
              // If/when required we could probably support the precise sleep
              // mode (e.g., via an option).
              //
              sp.root.test_target.ctx.sched.sleep (d);
            }
          };

          try
          {
            uint8_t r; // Storage.
            builtin b (bf (r,
                           c.arguments,
                           move (ifd), move (ofd.out), move (efd),
                           sp.wd_path,
                           bcs));

            success = run_pipe (sp,
                                nc,
                                ec,
                                move (ofd.in),
                                ci + 1, li, ll, diag);

            exit = process_exit (b.wait ());
          }
          catch (const system_error& e)
          {
            fail (ll) << "unable to execute " << c.program << " builtin: "
                      << e << endf;
          }
        }
        else
        {
          // Execute the process.
          //
          cstrings args (process_args ());

          // Resolve the relative not simple program path against the scope's
          // working directory. The simple one will be left for the process
          // path search machinery.
          //
          path p;

          try
          {
            p = path (args[0]);

            if (p.relative () && !p.simple ())
            {
              p = sp.wd_path / p;
              args[0] = p.string ().c_str ();
            }
          }
          catch (const invalid_path& e)
          {
            fail (ll) << "invalid program path " << e.path;
          }

          try
          {
            process_path pp (process::path_search (args[0]));

            if (verb >= 2)
              print_process (args);

            process pr (
              pp,
              args.data (),
              {ifd.get (), -1}, process::pipe (ofd), {-1, efd.get ()},
              sp.wd_path.string ().c_str ());

            ifd.reset ();
            ofd.out.reset ();
            efd.reset ();

            success = run_pipe (sp,
                                nc,
                                ec,
                                move (ofd.in),
                                ci + 1, li, ll, diag);

            pr.wait ();

            exit = move (pr.exit);
          }
          catch (const process_error& e)
          {
            error (ll) << "unable to execute " << args[0] << ": " << e;

            if (e.child)
              std::exit (1);

            throw failed ();
          }
        }

        assert (exit);

        // If the righ-hand side pipeline failed than the whole pipeline fails,
        // and no further checks are required.
        //
        if (!success)
          return false;

        const path& pr (c.program);

        // If there is no valid exit code available by whatever reason then we
        // print the proper diagnostics, dump stderr (if cached and not too
        // large) and fail the whole test. Otherwise if the exit code is not
        // correct then we print diagnostics if requested and fail the
        // pipeline.
        //
        bool valid (exit->normal ());

        // On Windows the exit code can be out of the valid codes range being
        // defined as uint16_t.
        //
#ifdef _WIN32
        if (valid)
          valid = exit->code () < 256;
#endif

        success = valid && eq == (exit->code () == c.exit.code);

        if (!valid || (!success && diag))
        {
          // In the presense of a valid exit code we print the diagnostics and
          // return false rather than throw.
          //
          diag_record d (valid ? error (ll) : fail (ll));

          if (!exit->normal ())
            d << pr << " " << *exit;
          else
          {
            uint16_t ec (exit->code ()); // Make sure is printed as integer.

            if (!valid)
              d << pr << " exit code " << ec << " out of 0-255 range";
            else if (!success)
            {
              if (diag)
                d << pr << " exit code " << ec << (eq ? " != " : " == ")
                  << static_cast<uint16_t> (c.exit.code);
            }
            else
              assert (false);
          }

          if (non_empty (esp, ll))
            d << info << "stderr: " << esp;

          if (non_empty (osp, ll))
            d << info << "stdout: " << osp;

          if (non_empty (isp, ll))
            d << info << "stdin: " << isp;

          // Print cached stderr.
          //
          print_file (d, esp, ll);
        }

        // If exit code is correct then check if the standard outputs match the
        // expectations. Note that stdout is only redirected to file for the
        // last command in the pipeline.
        //
        if (success)
          success =
            (!last ||
             check_output (pr, osp, isp, out, ll, sp, diag, "stdout")) &&
            check_output (pr, esp, isp, err, ll, sp, diag, "stderr");

        return success;
      }

      static bool
      run_expr (scope& sp,
                const command_expr& expr,
                size_t li, const location& ll,
                bool diag)
      {
        // Print test id once per test expression.
        //
        auto df = make_diag_frame (
          [&sp](const diag_record& dr)
          {
            // Let's not depend on how the path representation can be improved
            // for readability on printing.
            //
            dr << info << "test id: " << sp.id_path.posix_string ();
          });

        // Commands are numbered sequentially throughout the expression
        // starting with 1. Number 0 means the command is a single one.
        //
        size_t ci (expr.size () == 1 && expr.back ().pipe.size () == 1
                   ? 0
                   : 1);

        // If there is no ORs to the right of a pipe then the pipe failure is
        // fatal for the whole expression. In particular, the pipe must print
        // the diagnostics on failure (if generally allowed). So we find the
        // pipe that "switches on" the diagnostics potential printing.
        //
        command_expr::const_iterator trailing_ands; // Undefined if diag is
                                                    // disallowed.
        if (diag)
        {
          auto i (expr.crbegin ());
          for (; i != expr.crend () && i->op == expr_operator::log_and; ++i) ;
          trailing_ands = i.base ();
        }

        bool r (false);
        bool print (false);

        for (auto b (expr.cbegin ()), i (b), e (expr.cend ()); i != e; ++i)
        {
          if (diag && i + 1 == trailing_ands)
            print = true;

          const command_pipe& p (i->pipe);
          bool or_op (i->op == expr_operator::log_or);

          // Short-circuit if the pipe result must be OR-ed with true or AND-ed
          // with false.
          //
          if (!((or_op && r) || (!or_op && !r)))
            r = run_pipe (
              sp, p.begin (), p.end (), auto_fd (), ci, li, ll, print);

          ci += p.size ();
        }

        return r;
      }

      void default_runner::
      run (scope& sp,
           const command_expr& expr, command_type ct,
           size_t li,
           const location& ll)
      {
        // Noop for teardown commands if keeping tests output is requested.
        //
        if (ct == command_type::teardown &&
            common_.after == output_after::keep)
          return;

        if (verb >= 3)
        {
          char c ('\0');

          switch (ct)
          {
          case command_type::test:     c = ' '; break;
          case command_type::setup:    c = '+'; break;
          case command_type::teardown: c = '-'; break;
          }

          text << ": " << c << expr;
        }

        if (!run_expr (sp, expr, li, ll, true))
          throw failed (); // Assume diagnostics is already printed.
      }

      bool default_runner::
      run_if (scope& sp,
              const command_expr& expr,
              size_t li, const location& ll)
      {
        if (verb >= 3)
          text << ": ?" << expr;

        return run_expr (sp, expr, li, ll, false);
      }
    }
  }
}
