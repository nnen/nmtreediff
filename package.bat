@echo off
REM ===========================================================================
REM  package.bat - Builds nmtreediff and packs what a release is made of.
REM
REM  Usage:
REM      package.bat [Debug^|Release^|RelWithDebInfo^|MinSizeRel]
REM
REM  The configuration defaults to Release, which is the one a release ships.
REM  It is built first, by build.bat, so the packages always hold what the
REM  sources say now. Then CPack runs over the tree build.bat built, and
REM  leaves under build\package:
REM
REM      nmtreediff-<version>-win64.zip       the files, to unpack anywhere
REM      nmtreediff-<version>-win64.msi       the installer for the whole machine
REM      nmtreediff-<version>-win64-user.msi  the installer for the current user
REM
REM  What goes into them is decided by the install rules in
REM  cmake\Packaging.cmake, and cmake\WindowsInstaller.cmake says how the two
REM  installers differ. These are the same CPack runs the continuous
REM  integration makes.
REM
REM  The installers need the WiX Toolset, version 3. It is looked for when the
REM  build tree is configured, and build.bat configures a tree only once, so a
REM  tree made before WiX was installed says there is none. When that is what
REM  the tree says, this script configures it again, which repeats the search,
REM  before believing it. Where WiX is still not found only the archive is
REM  packed, and the script says so rather than failing.
REM
REM  Packages from earlier runs are left where they are. clean.bat deletes
REM  them, and so does clean-all.bat along with everything else under build\.
REM
REM  Exit codes: 0 on success, including when the installers were skipped, 1
REM  when CPack failed, otherwise whatever build.bat returned, which is 2 for a
REM  bad argument.
REM ===========================================================================

setlocal

set "DEFAULT_CONFIG=Release"
set "PACKAGE_DIR=%~dp0build\package"
set "MAIN_CPACK_CONFIG=CPackConfig.cmake"
set "USER_INSTALLER_CPACK_CONFIG=CPackUserInstallerConfig.cmake"
set "WIX_GENERATOR_PATTERN=^set(CPACK_GENERATOR .*WIX"

REM What to list at the end. Narrowed when the installers are skipped, so
REM that ones left by an earlier run are not shown as if this run made them.
set "PACKED_FILES=*.zip *.msi"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=%DEFAULT_CONFIG%"

REM Build first. build.bat checks the configuration, picks the build tree, and
REM reports both back in the two variables read below.
call "%~dp0build.bat" %CONFIG%
if errorlevel 1 exit /b %errorlevel%
set "BUILD_DIR=%NMTREEDIFF_BUILD_DIR%"
set "CONFIG=%NMTREEDIFF_BUILD_CONFIG%"

REM The generator list in the CPack configuration is where the configure step
REM recorded whether it found WiX. A tree that says it did not may only be
REM older than the WiX installation, so it is asked once more.
call :check_for_wix
if not defined HAVE_WIX (
    echo === Looking for the WiX Toolset again
    cmake "%BUILD_DIR%" >nul
    call :check_for_wix
)

REM The archive, and with it the installer for the whole machine where WiX
REM was found: they share a CPack configuration.
echo === Packaging %CONFIG% into %PACKAGE_DIR%
cpack --config "%BUILD_DIR%\%MAIN_CPACK_CONFIG%" -C %CONFIG% -B "%PACKAGE_DIR%"
if errorlevel 1 (
    echo package.bat: packaging failed 1>&2
    exit /b 1
)

REM Without WiX the second configuration could only fail, so it is skipped.
if not defined HAVE_WIX (
    echo === WiX Toolset 3 not found; the MSI installers were not packed
    set "PACKED_FILES=*.zip"
    goto packaged
)

REM The installer for the current user, which has a configuration of its own.
cpack --config "%BUILD_DIR%\%USER_INSTALLER_CPACK_CONFIG%" -C %CONFIG% -B "%PACKAGE_DIR%"
if errorlevel 1 (
    echo package.bat: packaging the installer for one user failed 1>&2
    exit /b 1
)

:packaged
echo === Packages in %PACKAGE_DIR%
pushd "%PACKAGE_DIR%"
dir /b %PACKED_FILES% 2>nul
popd

exit /b 0

REM ---------------------------------------------------------------------------
REM  :check_for_wix
REM
REM  Defines HAVE_WIX when the build tree's CPack configuration lists the WiX
REM  generator, and leaves it undefined otherwise.
REM ---------------------------------------------------------------------------
:check_for_wix
set "HAVE_WIX="
findstr /R /C:"%WIX_GENERATOR_PATTERN%" "%BUILD_DIR%\%MAIN_CPACK_CONFIG%" >nul 2>&1
if not errorlevel 1 set "HAVE_WIX=1"
exit /b 0
