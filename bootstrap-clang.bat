@echo off

rem file      : bootstrap-clang.bat
rem license   : MIT; see accompanying LICENSE file

setlocal EnableDelayedExpansion
goto start

:usage
echo.
echo Usage: %0 [/?] ^<cxx^> [^<cxx-option^>...]
echo.
echo The batch file expects to find the libbutl\ or libbutl-*\ directory
echo either in the current directory ^(build2 root^) or one level up. The
echo result is saved as build2\b-boot.exe.
echo.
echo Example usage:
echo.
echo %0 clang++
echo.
echo See the INSTALL file for details.
echo.
goto end

:start

if "_%1_" == "_/?_" goto usage

rem Compiler executable.
rem
if "_%1_" == "__" (
  echo error: compiler executable expected, run %0 /? for details
  goto error
) else (
  set "cxx=%1"
)

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
set "src=build2"

set "src=%src% libbuild2"
set "src=%src% libbuild2\config"
set "src=%src% libbuild2\dist"
set "src=%src% libbuild2\test"
set "src=%src% libbuild2\test\script"
set "src=%src% libbuild2\install"
set "src=%src% libbuild2\bin"
set "src=%src% libbuild2\c"
set "src=%src% libbuild2\cc"
set "src=%src% libbuild2\cxx"
set "src=%src% libbuild2\version"
set "src=%src% libbuild2\in"

set "src=%src% %libbutl%\libbutl"

rem Get the compile options.
rem
rem Note that we compile in the C++14 mode to be consistent with other
rem bootstrap scripts/makefiles (see bootstrap-mingw.bat for details).
rem
rem Clang 8 has a bunch of apparently harmless linker warnings that can be
rem worked around by passing an obscure internal option. Clang 9 doesn't
rem have this problem. To keep things simple, let's just suppress this
rem warning.
rem
set "ops=-m64 -std=c++1y -D_MT -D_CRT_SECURE_NO_WARNINGS -Xlinker /ignore:4217"
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
rem Note that echo does not override errorlevel.
rem

rem Filter out *.test.cxx sources.
rem
set "r="
for %%d in (%src%) do (
  for /F "tokens=*" %%i in ('dir /b "%%d\*.cxx" ^| findstr /v "\.test\.cxx"') do set "r=!r! %%d\%%i"
)

echo on
%cxx% -I%libbutl% -I. -DBUILD2_BOOTSTRAP -DBUILD2_HOST_TRIPLET=\"x86_64-microsoft-win32-msvc\" %ops% -o build2\b-boot.exe %r% -lshell32 -limagehlp
@echo off
if errorlevel 1 goto error

goto end

:error
endlocal
exit /b 1

:end
endlocal
