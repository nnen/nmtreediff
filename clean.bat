@echo off
REM ===========================================================================
REM  clean.bat - Removes what the build produced and keeps the build trees.
REM
REM  Usage:
REM      clean.bat [Debug^|Release^|RelWithDebInfo^|MinSizeRel]
REM
REM  Without an argument every configuration is cleaned; with one, only that
REM  configuration is. Object files, libraries and executables go. The CMake
REM  cache, the generated build files and the fetched dependency sources stay,
REM  so the next build.bat compiles everything again but neither configures
REM  nor downloads anything. Use clean-all.bat to remove those as well.
REM
REM  Exit codes: 0 on success, including when there is nothing to clean, 2 for
REM  a bad argument, otherwise whatever CMake returned last.
REM ===========================================================================

setlocal EnableDelayedExpansion

set "BUILD_ROOT=%~dp0build"
set "KNOWN_CONFIGS=Debug Release RelWithDebInfo MinSizeRel"
REM The trees build.bat creates, one per generator it can choose.
set "BUILD_TREES=ninja vs"
set "CACHE_FILE=CMakeCache.txt"

REM Work out which configurations to clean, recovering the canonical spelling
REM of a named one so that "debug" also works.
set "CONFIGS=%KNOWN_CONFIGS%"
if not "%~1"=="" (
    set "CONFIGS="
    for %%C in (%KNOWN_CONFIGS%) do (
        if /I "%~1"=="%%C" set "CONFIGS=%%C"
    )
    if not defined CONFIGS (
        echo clean.bat: unknown configuration "%~1" 1>&2
        echo clean.bat: expected one of: %KNOWN_CONFIGS% 1>&2
        exit /b 2
    )
)

REM Clean every tree that has been configured; one that has not holds nothing
REM a build produced.
set "RESULT=0"
set "CLEANED_ANY="
for %%T in (%BUILD_TREES%) do (
    if exist "%BUILD_ROOT%\%%T\%CACHE_FILE%" call :clean_tree "%BUILD_ROOT%\%%T"
)
if not defined CLEANED_ANY echo === Nothing to clean

exit /b %RESULT%

REM ---------------------------------------------------------------------------
REM  :clean_tree <tree directory>
REM
REM  Runs the clean target of one build tree for each configuration in
REM  CONFIGS. A failure is recorded in RESULT and the remaining configurations
REM  are still cleaned.
REM ---------------------------------------------------------------------------
:clean_tree
set "CLEANED_ANY=1"
for %%C in (%CONFIGS%) do (
    echo === Cleaning %%C in %~1
    cmake --build "%~1" --config %%C --target clean
    if errorlevel 1 (
        set "RESULT=!errorlevel!"
        echo clean.bat: cleaning %%C failed 1>&2
    )
)
exit /b 0
