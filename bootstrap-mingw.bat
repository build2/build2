@echo off

rem file      : bootstrap-mingw.bat
rem copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
rem license   : MIT; see accompanying LICENSE file

setlocal
goto start

:usage
echo.
echo Usage: %0 [/?] [cxx [cxx-option...]]
echo.
echo The batch file expects to find the libbutl\ or libbutl-*\ directory
echo either in the current directory (build2 root) or one level up.
echo.
echo Note that is any cxx-option arguments are specified, then they must
echo be preceded by the compiler executable (use g++ as the default). For
echo example:
echo.
echo %0 g++ -O3
echo.
echo See the INSTALL file for details.
echo.
goto end

:start

if "_%1_" == "_/?_" goto usage

rem See if there is libbutl or libbutl-* in the current directory and one
rem directory up. Note that globbing returns paths in alphabetic order.
rem
if exist libbutl\ (
  set "libbutl=libbutl"
) else (
  for /D %%d in (libbutl-*) do set "libbutl=%%d"
)

if "_%libbutl%_" == "__" (
  if exist ..\libbutl\ (
      set "libbutl=..\libbutl"
  ) else (
    for /D %%d in (..\libbutl-*) do set "libbutl=%%d"
  )
)

if "_%libbutl%_" == "__" (
  echo error: unable to find libbutl, run %0 /? for details
  goto error
)

rem All the source directories.
rem
set "src=build2\*.cxx"
set "src=%src% build2\config\*.cxx"
set "src=%src% build2\dist\*.cxx"
set "src=%src% build2\bin\*.cxx"
set "src=%src% build2\c\*.cxx"
set "src=%src% build2\cc\*.cxx"
set "src=%src% build2\cxx\*.cxx"
set "src=%src% build2\cli\*.cxx"
set "src=%src% build2\test\*.cxx"
set "src=%src% build2\install\*.cxx"
set "src=%src% %libbutl%\butl\*.cxx"

rem Get the compiler executable.
rem
if "_%1_" == "__" (
  set "cxx=g++"
) else (
  set "cxx=%1"
)

rem Get the compile options.
rem
set "ops=-std=c++1y -static"
:ops_next
shift
if "_%1_" == "__" (
  goto ops_done
) else (
  set "ops=%ops% %1"
  goto ops_next
)
:ops_done

rem Compile.
rem
echo %cxx% -I%libbutl% -I. -DBUILD2_HOST_TRIPLET=\"i686-w64-mingw32\" %ops% -o build2\b-boot.exe %src%
     %cxx% -I%libbutl% -I. -DBUILD2_HOST_TRIPLET=\"i686-w64-mingw32\" %ops% -o build2\b-boot.exe %src%
if errorlevel 1 goto error

goto end

:error
endlocal
exit /b 1

:end
endlocal
