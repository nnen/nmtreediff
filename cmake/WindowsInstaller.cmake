# The Windows installers: MSI packages built by CPack's WiX generator from the
# same install rules as the zip archive. Included by Packaging.cmake before
# CPack.
#
# There are two, so that the choice between them is made where they are
# downloaded rather than inside one installer that has to be both:
#
#   - nmtreediff-<version>-win64.msi installs for the whole machine, under
#     Program Files, asks for elevation, and appends the install directory to
#     the system path. It is packed together with the zip archive.
#   - nmtreediff-<version>-win64-user.msi installs for the current user, under
#     their local application data, never asks for elevation, and appends the
#     install directory to that user's own path. It is packed by running CPack
#     again with the configuration written at the end of this file:
#
#         cpack --config build/ninja/CPackUserInstallerConfig.cmake -C Release -B build/package
#
# What is set here describes the first; UserInstallerConfig.cmake.in overrides
# what differs for the second. Installer.wxs.in is the WiX source of both.
# Building them needs the WiX Toolset, version 3, which is the one every CMake
# this project accepts can drive.

# A machine without WiX still packs the zip archive instead of failing, so the
# installer joins the generators only where it can be built. CPack looks for
# WiX in the same places: the WIX variable its installer sets, then the path.
#
# The variable is also read from the registry, where the WiX installer wrote
# it for the whole machine. A prompt that was open during the install still
# has the environment from before it, and WiX does not put itself on the path,
# so without this the search fails in exactly the prompt it is first tried in.
file(TO_CMAKE_PATH "$ENV{WIX}" NMTREEDIFF_WIX_ROOT)
set(NMTREEDIFF_MACHINE_ENVIRONMENT_KEY
    "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment")
find_program(NMTREEDIFF_WIX_CANDLE candle
    PATHS "${NMTREEDIFF_WIX_ROOT}" "[${NMTREEDIFF_MACHINE_ENVIRONMENT_KEY};WIX]"
    PATH_SUFFIXES bin
    DOC "The WiX Toolset compiler; the MSI installers are packed when it is found"
)
if(NMTREEDIFF_WIX_CANDLE)
    list(APPEND CPACK_GENERATOR WIX)

    # CPack looks for WiX again when it packs, in an environment that may not
    # be the one this ran in. Telling it where WiX was found keeps the two
    # from disagreeing, which would fail the packaging of a tree that was
    # configured to build installers.
    get_filename_component(CPACK_WIX_ROOT "${NMTREEDIFF_WIX_CANDLE}" DIRECTORY)
else()
    message(STATUS "WiX Toolset not found; the MSI installers will not be packed")
endif()

set(CPACK_WIX_TEMPLATE ${CMAKE_CURRENT_LIST_DIR}/Installer.wxs.in)
set(CPACK_WIX_PRODUCT_ICON ${CMAKE_SOURCE_DIR}/assets/icon/nmtreediff.ico)

# The licence page of an installer shows rich text. CPack turns a plain text
# licence into that, but it tells the two apart by the file extension, and
# LICENSE has none, so it is given a copy that has. This replaces the value
# Packaging.cmake set, which no other generator reads.
configure_file(${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_BINARY_DIR}/LICENSE.txt COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_BINARY_DIR}/LICENSE.txt)

# What tells Windows Installer that two packages are the same product, so that
# installing a newer version replaces the older one rather than sitting beside
# it. It must never change. The installer for one user has an identifier of its
# own, because Windows Installer does not upgrade across install scopes and
# the two are separate products to it.
set(CPACK_WIX_UPGRADE_GUID 7211A101-19E1-45C4-A3C6-2D41B4F5FDE5)

# The directory under Program Files. CPack would put the version in its name,
# and then every upgrade would move the tool and the path entry with it.
set(CPACK_PACKAGE_INSTALL_DIRECTORY ${PROJECT_NAME})

# The values Installer.wxs.in leaves open. CPack hands every variable whose
# name starts with CPACK_ on to packaging time, which is how they reach it.
# The identifier is that of the component holding the path entry, and like the
# one above it must never change.
set(CPACK_NMTREEDIFF_INSTALL_SCOPE perMachine)
set(CPACK_NMTREEDIFF_PATH_IS_SYSTEM yes)
set(CPACK_NMTREEDIFF_PATH_COMPONENT_GUID 8459E8AD-404E-491F-A7BB-B12324CBAC97)

# The configuration of the second installer. It reads the one CPack writes and
# changes what differs, so it needs to know only where that one is.
configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/UserInstallerConfig.cmake.in
    ${CMAKE_BINARY_DIR}/CPackUserInstallerConfig.cmake
    @ONLY
)
