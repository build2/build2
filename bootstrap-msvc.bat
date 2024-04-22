@echo off

rem file      : bootstrap-msvc.bat
rem license   : MIT; see accompanying LICENSE file

setlocal EnableDelayedExpansion
goto start

:usage
echo.
echo Usage: %0 [/?] ^<cxx^> [^<cxx-option^>...]
echo.
echo Normally this batch file is executed from one of the Visual Studio
echo command prompts with cl.exe as the compiler executable (^<cxx^>).
echo It assumes that all the relevant compiler environment variables
echo ^(INCLUDE, LIB^) are set.
echo.
echo The batch file expects to find the libbutl\ or libbutl-*\ directory
echo either in the current directory ^(build2 root^) or one level up. The
echo result is saved as build2\b-boot.exe.
echo.
echo Example usage:
echo.
echo %0 cl
echo.
echo See the INSTALL file for details.
echo.
goto end

rem Clean up .obj files from all the directories passed as arguments.
rem
:clean_obj
  for %%d in (%*) do (
    if exist %%d\*.obj del %%d\*.obj
  )
goto :eof

:compile
  rem Note that echo does not override errorlevel.
  rem
  echo on
  %cxx% /I"%owd%\%libbutl%" /I"%owd%" /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS /DBUILD2_BOOTSTRAP /DBUILD2_HOST_TRIPLET=\"x86_64-microsoft-win32-msvc\" %ops% /c /TP %*
  @echo off
  if errorlevel 1 exit /b 1
goto :eof

:link
  echo on
  %cxx% %ops% %*
  @echo off
  if errorlevel 1 exit /b 1
goto :eof

:start

set "owd=%CD%"

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
set "src=%src% libbuild2\script"
set "src=%src% libbuild2\build\script"
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
set "ops=/nologo /utf-8 /EHsc /MT /MP"
:ops_next
shift
if "_%1_" == "__" (
  goto ops_done
) else (
  set "ops=%ops% %1"
  goto ops_next
)
:ops_done

rem First clean up any stale .obj files we might have laying around.
rem
call :clean_obj %src%
if errorlevel 1 goto error

rem Compile.
rem
rem VC dumps .obj files in the current directory not caring if the names
rem clash. And boy do they clash.
rem
set "obj="
for %%d in (%src%) do (
  cd %%d

  rem Filter out *.test.cxx sources.
  rem
  rem Note that we don't need to worry about *.obj since we clean them all up
  rem before compiling so after compiling we will only have the ones we need.
  rem
  set "r="
  for /F "tokens=*" %%i in ('dir /b *.cxx ^| findstr /v "\.test\.cxx"') do set "r=!r! %%i"

  call :compile !r!
  if errorlevel 1 goto error
  cd %owd%
  set "obj=!obj! %%d\*.obj"
)

rem Link.
rem
call :link /Fe: build2\b-boot.exe %obj% shell32.lib imagehlp.lib
if errorlevel 1 goto error

rem Clean up.
rem
call :clean_obj %src%
if errorlevel 1 goto error

goto end

:error
cd %owd%
endlocal
exit /b 1

:end
endlocal
