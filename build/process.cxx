// file      : build/process.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build/process>

#ifndef _WIN32
#  include <unistd.h>    // execvp, fork, dup2, pipe, {STDIN,STDERR}_FILENO
#  include <sys/wait.h>  // waitpid
#else
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   // CreatePipe, CreateProcess
#  include <io.h>        // _open_osfhandle
#  include <fcntl.h>     // _O_TEXT
#endif

using namespace std;

namespace build
{
#ifndef _WIN32

  process::
  process (char const* args[], bool in, bool err, bool out)
  {
    int out_fd[2];
    int in_efd[2];
    int in_ofd[2];

    if ((in && pipe (out_fd) == -1)  ||
        (err && pipe (in_efd) == -1) ||
        (out && pipe (in_ofd) == -1))
      throw process_error (errno, false);

    id = fork ();

    if (id == -1)
      throw process_error (errno, false);

    if (id == 0)
    {
      // Child. If requested, close the write end of the pipe and duplicate
      // the read end to stdin. Then close the original read end descriptor.
      //
      if (in)
      {
        if (close (out_fd[1]) == -1 ||
            dup2 (out_fd[0], STDIN_FILENO) == -1 ||
            close (out_fd[0]) == -1)
          throw process_error (errno, true);
      }

      // Do the same for the stderr if requested.
      //
      if (err)
      {
        if (close (in_efd[0]) == -1 ||
            dup2 (in_efd[1], STDERR_FILENO) == -1 ||
            close (in_efd[1]) == -1)
          throw process_error (errno, true);
      }

      // Do the same for the stdout if requested.
      //
      if (out)
      {
        if (close (in_ofd[0]) == -1 ||
            dup2 (in_ofd[1], STDOUT_FILENO) == -1 ||
            close (in_ofd[1]) == -1)
          throw process_error (errno, true);
      }

      if (execvp (args[0], const_cast<char**> (&args[0])) == -1)
        throw process_error (errno, true);
    }
    else
    {
      // Parent. Close the other ends of the pipes.
      //
      if ((in && close (out_fd[0]) == -1)  ||
          (err && close (in_efd[1]) == -1) ||
          (out && close (in_ofd[1]) == -1))
        throw process_error (errno, false);
    }

    this->out_fd = in ? out_fd[1] : 0;
    this->in_efd = err ? in_efd[0] : 0;
    this->in_ofd = out ? in_ofd[0] : 0;
  }

  bool process::
  wait ()
  {
    int status;
    int r (waitpid (id, &status, 0));
    id = 0; // We have tried.

    if (r == -1)
      throw process_error (errno, false);

    return WIFEXITED (status) && WEXITSTATUS (status) == 0;
  }

#else // _WIN32

  static void
  print_error (char const* name)
  {
    LPTSTR msg;
    DWORD e (GetLastError());

    if (!FormatMessage(
          FORMAT_MESSAGE_ALLOCATE_BUFFER |
          FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
          0,
          e,
          MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
          (LPTSTR) &msg,
          0,
          0))
    {
      cerr << name << ": error: unknown error code " << e << endl;
      return;
    }

    cerr << name << ": error: " << msg << endl;
    LocalFree (msg);
  }

  static process_info
  start_process (char const* args[], char const* name, bool err, bool out)
  {
    HANDLE out_h[2];
    HANDLE in_eh[2];
    HANDLE in_oh[2];
    SECURITY_ATTRIBUTES sa;

    sa.nLength = sizeof (SECURITY_ATTRIBUTES);
    sa.bInheritHandle = true;
    sa.lpSecurityDescriptor = 0;

    if (!CreatePipe (&out_h[0], &out_h[1], &sa, 0) ||
        !SetHandleInformation (out_h[1], HANDLE_FLAG_INHERIT, 0))
    {
      print_error (name);
      throw process_failure ();
    }

    if (err)
    {
      if (!CreatePipe (&in_eh[0], &in_eh[1], &sa, 0) ||
          !SetHandleInformation (in_eh[0], HANDLE_FLAG_INHERIT, 0))
      {
        print_error (name);
        throw process_failure ();
      }
    }

    if (out)
    {
      if (!CreatePipe (&in_oh[0], &in_oh[1], &sa, 0) ||
          !SetHandleInformation (in_oh[0], HANDLE_FLAG_INHERIT, 0))
      {
        print_error (name);
        throw process_failure ();
      }
    }

    // Create the process.
    //
    path file (args[0]);

    // Do PATH search.
    //
    if (file.directory ().empty ())
      file = path_search (file);

    if (file.empty ())
    {
      cerr << args[0] << ": error: file not found" << endl;
      throw process_failure ();
    }

    // Serialize the arguments to string.
    //
    string cmd_line;

    for (char const** p (args); *p != 0; ++p)
    {
      if (p != args)
        cmd_line += ' ';

      // On Windows we need to protect values with spaces using quotes.
      // Since there could be actual quotes in the value, we need to
      // escape them.
      //
      string a (*p);
      bool quote (a.find (' ') != string::npos);

      if (quote)
        cmd_line += '"';

      for (size_t i (0); i < a.size (); ++i)
      {
        if (a[i] == '"')
          cmd_line += "\\\"";
        else
          cmd_line += a[i];
      }

      if (quote)
        cmd_line += '"';
    }

    // Prepare other info.
    //
    STARTUPINFO si;
    PROCESS_INFORMATION pi;

    memset (&si, 0, sizeof (STARTUPINFO));
    memset (&pi, 0, sizeof (PROCESS_INFORMATION));

    si.cb = sizeof(STARTUPINFO);

    if (err)
      si.hStdError = in_eh[1];
    else
      si.hStdError = GetStdHandle (STD_ERROR_HANDLE);

    if (out)
      si.hStdOutput = in_oh[1];
    else
      si.hStdOutput = GetStdHandle (STD_OUTPUT_HANDLE);

    si.hStdInput = out_h[0];
    si.dwFlags |= STARTF_USESTDHANDLES;

    if (!CreateProcess (
          file.string ().c_str (),
          const_cast<char*> (cmd_line.c_str ()),
          0,    // Process security attributes.
          0,    // Primary thread security attributes.
          true, // Inherit handles.
          0,    // Creation flags.
          0,    // Use our environment.
          0,    // Use our current directory.
          &si,
          &pi))
    {
      print_error (name);
      throw process_failure ();
    }

    CloseHandle (pi.hThread);
    CloseHandle (out_h[0]);

    if (err)
      CloseHandle (in_eh[1]);

    if (out)
      CloseHandle (in_oh[1]);

    process_info r;
    r.id = pi.hProcess;
    r.out_fd = _open_osfhandle ((intptr_t) (out_h[1]), 0);

    if (r.out_fd == -1)
    {
      cerr << name << ": error: unable to obtain C file handle" << endl;
      throw process_failure ();
    }

    if (err)
    {
      // Pass _O_TEXT to get newline translation.
      //
      r.in_efd = _open_osfhandle ((intptr_t) (in_eh[0]), _O_TEXT);

      if (r.in_efd == -1)
      {
        cerr << name << ": error: unable to obtain C file handle" << endl;
        throw process_failure ();
      }
    }
    else
      r.in_efd = 0;

    if (out)
    {
      // Pass _O_TEXT to get newline translation.
      //
      r.in_ofd = _open_osfhandle ((intptr_t) (in_oh[0]), _O_TEXT);

      if (r.in_ofd == -1)
      {
        cerr << name << ": error: unable to obtain C file handle" << endl;
        throw process_failure ();
      }
    }
    else
      r.in_ofd = 0;

    return r;
  }

  static bool
  wait_process (process_info pi, char const* name)
  {
    DWORD status;

    if (WaitForSingleObject (pi.id, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess (pi.id, &status))
    {
      print_error (name);
      throw process_failure ();
    }

    CloseHandle (pi.id);
    return status == 0;
  }

#endif // _WIN32
}
