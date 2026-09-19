@echo off
REM ===========================================================================
REM  clean-all.bat - Removes everything build.bat created.
REM
REM  Usage:
REM      clean-all.bat
REM
REM  Deletes the whole build\ directory: what the build produced, the CMake
REM  cache and the generated build files, the fetched dependency sources and
REM  the saved compiler environment. The next build.bat starts from nothing,
REM  so it configures and downloads the dependencies again. Use clean.bat to
REM  keep the cache and the dependencies.
REM
REM  Only build\ is touched. A tree made elsewhere, such as the out\ directory
REM  Visual Studio uses when it opens the folder, is left alone.
REM
REM  Exit codes: 0 on success, including when there is nothing to delete, and
REM  1 when something could not be deleted, usually because a program built
REM  there is still running.
REM ===========================================================================

setlocal

set "BUILD_ROOT=%~dp0build"

if not exist "%BUILD_ROOT%" (
    echo === Nothing to clean
    exit /b 0
)

echo === Deleting %BUILD_ROOT%
rmdir /s /q "%BUILD_ROOT%"

REM rmdir reports no failure through its exit code, so look instead.
if exist "%BUILD_ROOT%" (
    echo clean-all.bat: could not delete all of %BUILD_ROOT% 1>&2
    exit /b 1
)

exit /b 0
