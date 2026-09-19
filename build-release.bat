@echo off
REM ===========================================================================
REM  build-release.bat - Builds the Release configuration.
REM
REM  A thin wrapper over build.bat; any arguments given here are passed to its
REM  build step. See build.bat for the details, including RelWithDebInfo for an
REM  optimised build that still carries debug information.
REM ===========================================================================

call "%~dp0build.bat" Release %*
exit /b %errorlevel%
