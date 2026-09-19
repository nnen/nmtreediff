@echo off
REM ===========================================================================
REM  build-debug.bat - Builds the Debug configuration.
REM
REM  A thin wrapper over build.bat; any arguments given here are passed to its
REM  build step. See build.bat for the details.
REM ===========================================================================

call "%~dp0build.bat" Debug %*
exit /b %errorlevel%
