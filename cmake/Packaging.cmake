# What a release is made of, and how it is packed.
#
# The install rules are the one list of what ships: the executable, the
# licence, the notices of the libraries it is built with, the user guide and
# the guide to writing format providers. CPack
# packs exactly that list into a zip archive and, on Windows with the WiX
# Toolset installed, into the MSI installers of WindowsInstaller.cmake, which
# also put the tool on the path. They are what the release job in
# .github/workflows/ci.yml publishes:
#
#     cpack --config build/ninja/CPackConfig.cmake -C Release -B build/package

# Everything that ships belongs to this install component. The fetched
# dependencies bring install rules of their own, for headers and libraries that
# are of no use next to a finished executable, and naming a component is what
# keeps those out of the package.
set(NMTREEDIFF_RUNTIME_COMPONENT runtime)

# A Windows release is unpacked or installed into a directory of its own and
# run from there, so everything sits side by side at the top. Elsewhere the
# files go where the platform expects them.
if(WIN32)
    set(NMTREEDIFF_INSTALL_BINDIR .)
    set(NMTREEDIFF_INSTALL_DOCDIR .)
else()
    include(GNUInstallDirs)
    set(NMTREEDIFF_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
    set(NMTREEDIFF_INSTALL_DOCDIR ${CMAKE_INSTALL_DOCDIR})
endif()

install(TARGETS nmtreediff
    RUNTIME DESTINATION ${NMTREEDIFF_INSTALL_BINDIR}
    COMPONENT ${NMTREEDIFF_RUNTIME_COMPONENT}
)
# Writes the file of notices and says where it is.
include(${CMAKE_CURRENT_LIST_DIR}/ThirdPartyNotices.cmake)

install(FILES
        ${CMAKE_SOURCE_DIR}/LICENSE
        ${NMTREEDIFF_NOTICES_FILE}
        ${CMAKE_SOURCE_DIR}/USAGE.md
    DESTINATION ${NMTREEDIFF_INSTALL_DOCDIR}
    COMPONENT ${NMTREEDIFF_RUNTIME_COMPONENT}
)
# The user guide links to this one as docs/PROVIDERS.md, so it keeps that place
# relative to the guide and the links still lead somewhere.
install(FILES ${CMAKE_SOURCE_DIR}/docs/PROVIDERS.md
    DESTINATION ${NMTREEDIFF_INSTALL_DOCDIR}/docs
    COMPONENT ${NMTREEDIFF_RUNTIME_COMPONENT}
)

set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_VENDOR "Jan Milík")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_GENERATOR ZIP)

# The files sit at the top of the archive rather than inside a directory named
# after the version, so the path to the executable inside the archive, which a
# package manager's manifest has to state, is the same in every release.
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)

# Pack the one component and nothing else. The fields are the build directory,
# the project name, the component, and the directory inside the package.
set(CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${PROJECT_NAME};${NMTREEDIFF_RUNTIME_COMPONENT};/"
)

if(WIN32)
    include(${CMAKE_CURRENT_LIST_DIR}/WindowsInstaller.cmake)
endif()

include(CPack)
