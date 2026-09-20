@echo off
REM ===========================================================================
REM  build.bat - Configures and builds nmtreediff in one CMake configuration.
REM
REM  Usage:
REM      build.bat [Debug^|Release^|RelWithDebInfo^|MinSizeRel] [build arguments]
REM
REM  The configuration defaults to RelWithDebInfo, the same default the top
REM  level CMakeLists.txt applies. Anything after the configuration is handed
REM  to the build step unchanged, so targets and build tool options work the
REM  way they do with CMake itself:
REM
REM      build.bat Debug --target nmtreediff_core
REM      build.bat Release -- -j4
REM
REM  Three things keep this fast:
REM
REM    - Ninja is used when it is on the path. It compiles on every core, where
REM      MSBuild with its defaults compiles one file at a time. Without Ninja
REM      the Visual Studio generator is used, with its parallelism switched on.
REM    - Every configuration shares one multi configuration build tree, so the
REM      dependencies are fetched and configured once rather than once per
REM      configuration. Object files are still kept per configuration, so a
REM      Debug build and a Release build do not evict each other.
REM    - The configure step runs only when the tree does not exist yet. After
REM      that the build tool re-runs CMake by itself whenever a CMakeLists.txt
REM      changes, so configuring on every invocation is wasted time.
REM
REM  Cache options for the configure step go in the NMTREEDIFF_CMAKE_ARGS environment
REM  variable. While it is set, the configure step runs on every invocation so
REM  that a changed value is picked up:
REM
REM      set NMTREEDIFF_CMAKE_ARGS=-DNMTREEDIFF_BUILD_GUI=OFF
REM      build.bat Debug
REM
REM  The generator is chosen here and must be a multi configuration one, so do
REM  not pass -G. To start over, delete the directory under build\.
REM
REM  After a successful build the NMTREEDIFF_BUILD_DIR and NMTREEDIFF_BUILD_CONFIG
REM  environment variables hold the build tree and the configuration, spelled
REM  the canonical way, for a script that carries on from there; package.bat
REM  is one.
REM
REM  Exit codes: 0 on success, 2 for a bad argument, otherwise whatever CMake
REM  returned, so a caller such as a CI job can test it.
REM ===========================================================================

setlocal EnableDelayedExpansion

set "SOURCE_DIR=%~dp0."
set "DEFAULT_CONFIG=RelWithDebInfo"
set "KNOWN_CONFIGS=Debug Release RelWithDebInfo MinSizeRel"
set "KNOWN_CONFIGS_LIST=Debug;Release;RelWithDebInfo;MinSizeRel"
set "EXE_NAME=nmtreediff.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS_RELATIVE_PATH=VC\Auxiliary\Build\vcvars64.bat"
set "ENV_CACHE_DIR=%SOURCE_DIR%\build"
set "ENV_CACHE=%ENV_CACHE_DIR%\compiler-environment.bat"
set "ENV_CACHE_VARIABLES=PATH INCLUDE LIB LIBPATH"

REM The configuration is the first argument; everything after it belongs to the
REM build step. Collecting the rest with shift rather than %* keeps quoted
REM arguments quoted.
set "CONFIG=%~1"
if not "%CONFIG%"=="" shift
if "%CONFIG%"=="" set "CONFIG=%DEFAULT_CONFIG%"

set "BUILD_ARGS="
:collect_build_args
if "%~1"=="" goto build_args_collected
set "BUILD_ARGS=!BUILD_ARGS! %1"
shift
goto collect_build_args
:build_args_collected

REM Reject an unknown configuration here rather than letting the build tool
REM fail on it, and recover the canonical spelling so that "debug" also works.
set "CANONICAL_CONFIG="
for %%C in (%KNOWN_CONFIGS%) do (
    if /I "%CONFIG%"=="%%C" set "CANONICAL_CONFIG=%%C"
)
if not defined CANONICAL_CONFIG (
    echo build.bat: unknown configuration "%CONFIG%" 1>&2
    echo build.bat: expected one of: %KNOWN_CONFIGS% 1>&2
    exit /b 2
)
set "CONFIG=%CANONICAL_CONFIG%"

REM Pick the generator. Ninja needs the compiler environment that a developer
REM prompt provides; if that cannot be set up, fall back to Visual Studio,
REM which finds the compiler without it.
set "USE_NINJA="
where ninja >nul 2>&1 && set "USE_NINJA=1"
if defined USE_NINJA call :ensure_compiler_environment
if errorlevel 1 set "USE_NINJA="

if defined USE_NINJA (
    set "BUILD_DIR=%SOURCE_DIR%\build\ninja"
    set "GENERATOR_ARGS=-G "Ninja Multi-Config""
    set "GENERATED_FILE=build.ninja"
    set "PARALLEL_ARGS="
) else (
    REM --parallel builds the projects side by side, and /MP, which cl.exe
    REM reads from the CL variable, compiles the files inside each of them
    REM side by side. MSBuild does neither unless asked.
    set "BUILD_DIR=%SOURCE_DIR%\build\vs"
    set "GENERATOR_ARGS="
    set "GENERATED_FILE=CMakeCache.txt"
    set "PARALLEL_ARGS= --parallel"
    set "CL=/MP !CL!"
)

REM Configure only when it is needed: on a first run, which is also what fetches
REM the pinned dependencies, or when there are configure options to apply.
set "NEEDS_CONFIGURE="
if not exist "%BUILD_DIR%\%GENERATED_FILE%" set "NEEDS_CONFIGURE=1"
if defined NMTREEDIFF_CMAKE_ARGS set "NEEDS_CONFIGURE=1"
if not defined NEEDS_CONFIGURE goto configured

echo === Configuring in %BUILD_DIR%
cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" %GENERATOR_ARGS% "-DCMAKE_CONFIGURATION_TYPES=%KNOWN_CONFIGS_LIST%" %NMTREEDIFF_CMAKE_ARGS%
set "RESULT=!errorlevel!"
if not "!RESULT!"=="0" (
    echo build.bat: configuration failed 1>&2
    exit /b !RESULT!
)
:configured

echo === Building %CONFIG%
cmake --build "%BUILD_DIR%" --config %CONFIG%%PARALLEL_ARGS%!BUILD_ARGS!
set "RESULT=!errorlevel!"
if not "!RESULT!"=="0" (
    echo build.bat: build failed 1>&2
    exit /b !RESULT!
)

set "EXE=%BUILD_DIR%\bin\%CONFIG%\%EXE_NAME%"
if exist "%EXE%" (
    echo === Built %EXE%
) else (
    echo === Built %CONFIG% in %BUILD_DIR%
)

REM Tell a calling script which tree was built and in which configuration.
REM Both are decided here, so a caller that worked them out for itself could
REM disagree. The line is expanded before endlocal runs, which is what carries
REM the values out of this script's own environment.
endlocal & set "NMTREEDIFF_BUILD_DIR=%BUILD_DIR%" & set "NMTREEDIFF_BUILD_CONFIG=%CONFIG%"
exit /b 0

REM ---------------------------------------------------------------------------
REM  :ensure_compiler_environment
REM
REM  Makes sure a compiler can be found by Ninja, loading the environment of
REM  the newest Visual Studio installation if there is none yet.
REM
REM  Returns 0 when a compiler is available and 1 otherwise. Nothing is loaded
REM  when cl.exe is already on the path, as in a developer prompt, or when CXX
REM  names a compiler explicitly.
REM
REM  Loading the environment from Visual Studio takes several seconds, which
REM  is most of a build that has nothing to do, so the result is saved under
REM  build\ and read back on the following runs.
REM ---------------------------------------------------------------------------
:ensure_compiler_environment
if defined CXX exit /b 0
where cl >nul 2>&1 && exit /b 0

REM Try the environment saved by an earlier run. It goes stale when Visual
REM Studio is updated, which shows as cl.exe no longer being where it says.
if exist "%ENV_CACHE%" call "%ENV_CACHE%"
where cl >nul 2>&1 && exit /b 0
if not exist "%VSWHERE%" exit /b 1

set "VS_PATH="
for /f "usebackq delims=" %%P in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%P"
if not defined VS_PATH exit /b 1
if not exist "%VS_PATH%\%VCVARS_RELATIVE_PATH%" exit /b 1

REM The script reports a vswhere.exe it cannot find on the path and carries on
REM regardless, so its error output is noise; the check below is what counts.
call "%VS_PATH%\%VCVARS_RELATIVE_PATH%" >nul 2>&1
where cl >nul 2>&1 || exit /b 1

REM Save what the compiler and the linker read for the next run. Delayed
REM expansion writes the values out verbatim, whatever characters they hold.
if not exist "%ENV_CACHE_DIR%" mkdir "%ENV_CACHE_DIR%"
(
    for %%V in (%ENV_CACHE_VARIABLES%) do echo set "%%V=!%%V!"
) > "%ENV_CACHE%"
exit /b 0
